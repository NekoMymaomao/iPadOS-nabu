# nabu_system as one disk with a synthetic GPT

Branch: `nabu/system-disk-partitions` (feature branch; `main` untouched).

## The failure this fixes

Boot reaches the kernel storage layer, validates both system windows, then dies in
userland with:

```
NABU INIT FAILURE: find persistent APFS Data phase=5 errno=0
```

`phase=5` is `NabuDataMount.inc` exhausting its 50 polls of `kern.nabu_data_device`.
That sysctl is produced by `NabuDataLookup.cpp`, which walks every `IOMedia` and
returns the BSD node whose property `UUID` equals the Data volume UUID
`168f804b-0cb5-4167-a239-c71953e36966`. Nothing matches, because nothing ever
published a partition IOMedia: `NabuDataBlock.cpp` says it outright — *"The
published device is only the Data window, never the containing LUN"*.

APFS does not probe a bare window for a container. It attaches to an `IOMedia`
whose **Content Hint is `Apple_APFS`**, and only `IOGUIDPartitionScheme` sets that
hint, and only when the provider is a whole disk with a GPT.

## Evidence: what `3.apfs` actually is

Parsing the GPT dump that came with the OS image:

| 段 | LBA | 字节 |
|---|---|---|
| protective MBR | 0 | 512 |
| primary header | 1 | 512 |
| primary entries (128×128) | 2..33 | 16,384 |
| **Apple_APFS partition** | **34..9,900,065** | **5,068,816,384** |
| backup entries | 9,900,066..9,900,097 | 16,384 |
| backup header | 9,900,098 | 512 |

The disk is 9,900,099 × 512 = 5,068,850,688 bytes; subtract the 34,304 bytes of
GPT metadata and you get exactly the 5,068,816,384 bytes of `3.apfs`. In other
words Apple ships the OS as **disk + partition table + Apple_APFS partition**,
not as a loose container file. `3.apfs` is partition 1 of that disk.

The 26 package already knew this on the write side: `probe_apfs()` looks for
`EFI PART` at LBA 1 and jumps to the Apple_APFS entry's start LBA. The read side
never put the table back.

## What this branch does

`NabuSystemDisk` publishes the whole 60 GiB `nabu_system` partition as one
`IOBlockStorageDevice` with 512-byte logical blocks, and synthesises a GPT over
it in RAM:

```
LBA      2048..9902079    NabuOS        5,068,816,384   ro
LBA  12582912..17784831   NabuSystemOS  2,663,383,040   ro
LBA  20971520..123731967  NabuData     52,613,349,376   rw
LBA 123731968..125829086  NabuLog       1,073,741,824   ro
```

`IOGUIDPartitionScheme` then parses that table, publishes one `IOMedia` per
partition with `Content Hint = Apple_APFS`, and `NabuDataLookup.cpp` finally
finds its `UUID` — because the `NabuData` entry's unique GUID *is* the Data volume
UUID. Removing that step removes `phase=5` without touching a single flashed byte.

The table is a **view**, exactly like a loop device's partition view: LBA 0..33
and the last 33 LBAs are served from `NabuGpt.h`, writes to them are dropped, and
the SHNGROOT header on the medium is never read or written through this path.

## Files

| path | role |
|---|---|
| `src/NabuGpt.h` | builds the 34,304-byte GPT blob (MBR, primary header/entries, backup entries/header) |
| `src/NabuDiskMap.h` | LBA → route (metadata / window 0..2 / reserved / gap) |
| `src/NabuDiskCore.{h,cpp}` | IOKit-free read/write engine; splits requests at route boundaries |
| `src/NabuSystemDisk.{h,cpp}` | `IOBlockStorageDevice` glue, mirrors `NabuDataBlock.cpp` |
| `tools/gen_gpt.py` | Python reference implementation + independent read-back assertions |
| `tests/test_disk_map.cpp` | blob structure, CRCs, routing, contiguity |
| `tests/test_disk_core.cpp` | read/write semantics against a fake transport |
| `kext/NabuSystemDiskKmod.cpp` | `_kmod_info` + `KMOD_EXPLICIT_DECL(org.nabu.driver.SystemDisk)` |
| `kext/build_nabu_system_disk.py` | macOS kext compile (same flags as the DataBlock build) |
| `kext/link_nabu_data_block.parameterize.patch` | turns the prelink tool's hardcoded bundle metadata into flags |
| `kext/README.md` | compile → parameterise → prelink, verbatim commands |
| `ci/nabu-system-disk.yml` | GitHub Actions: `make check` on every push (copy to `.github/workflows/` at repo root) |

## Verification

`make check` in this directory:

```
tools/gen_gpt.py --verify      all checks passed       (34304 metadata bytes)
test_disk_map reference.bin    all host checks passed
test_disk_core reference.bin   core checks passed: reads=10 writes=4 refused=1024 shadowed=512
```

The C++ blob is compared byte-for-byte against the Python reference; the engine
test drives a fake transport and asserts content, routing, refusals, GPT write
shadowing, boundary crossings, and that a 128 KiB request stays one transport
call.

`kext/compile_check.sh` additionally cross-compiles the three kernel sources with
`-target arm64-apple-ios16.5 -mkernel -fapple-kext` against an exported 8796
header set: all three produce `Mach-O 64-bit arm64` objects, `_kmod_info` is
defined, and `_nabu_system_disk_start` is left as the import the prelink step
resolves.

## Integration (kernel side)

1. Call `nabu_system_disk_start()` after `nabu_storage_prepare()` has validated
   OS/SystemOS, in place of `nabu_data_block_start()`.
2. The two views are mutually exclusive: publishing both would expose the same
   Data bytes twice and risk a double mount. Gate the old call behind
   `nabu-system-disk=0`.
3. `NabuSystemDisk.cpp` mirrors the override set of `NabuDataBlock.cpp` but must
   be compiled against the 8796 XNU headers; only the IOKit-free core is covered
   by the host tests here. `kext/README.md` has the compile and prelink commands.

## Known limits

- The backup GPT necessarily lands in the last 33 LBAs of the 60 GiB partition,
  i.e. the final 17 KiB of the 1 GiB log reserve (tail of slot 15). The active
  manifest is slot 14, so nothing live is shadowed today; if the log writer ever
  fills slot 15, those 17 KiB are masked by the synthetic table.
- `NabuLog` is declared `[59 GiB, LAST_USABLE_LBA]`, 17,408 bytes short of 1 GiB,
  for the same reason.
- The disk is writable as a whole; per-window policy lives in the transport
  (`nabu_storage_writable`), so OS/SystemOS reject writes and Data accepts them.
