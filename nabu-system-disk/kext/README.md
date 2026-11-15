# Kext build and prelink

The 8796 kernel is not built from source here; drivers are compiled as kext
objects on macOS and then **prelinked** into an existing `kernel.native-apfs.macho`
by `tools/link_nabu_data_block.py` (Mach-O surgery: segment coalescing, GOT,
stubs, `__PRELINK_INFO`, kmod list). This directory supplies the three pieces
that pipeline needs for the system-disk driver.

## 1. Compile (macOS, 8796 tree present)

```sh
python3 kext/build_nabu_system_disk.py --tree <xnu-8796 tree> --output out
```

Produces:

```
out/NabuDiskCore.o                    routing + synthetic GPT
out/NabuSystemDisk.o                  IOBlockStorageDevice glue
out/NabuSystemDiskKmod.o              _kmod_info, KMOD_EXPLICIT_DECL(org.nabu.driver.SystemDisk)
out/IOBlockStorageDevice-reference.o  vtable/ABI reference, same as the DataBlock build
out/compile-command.json
```

Flags are identical to `tools/build_nabu_data_block.py`: `-target
arm64-apple-ios16.5 -mkernel -fapple-kext`, headers from `BUILD/obj/EXPORT_HDRS`.

## 2. Parameterise the prelink tool

`link_nabu_data_block.py` hardcodes the DataBlock bundle id, name, executable
path and start symbol. Apply the patch:

```sh
patch -p0 < kext/link_nabu_data_block.parameterize.patch   # from tools/
```

Defaults are unchanged, so the existing DataBlock build keeps working; the new
flags are `--bundle-id --bundle-name --executable --bundle-path --start-symbol
--report-name`.

### Compile check without macOS

`kext/compile_check.sh` runs the same flags against an `EXPORT_HDRS` tree and the
IOStorageFamily headers, so a Linux host can verify the sources still target the
kernel ABI:

```sh
EXPORT_HDRS=<tree>/EXPORT_HDRS \
IOKIT_STORAGE_INC=<include with IOKit/storage + TargetConditionals.h> \
NABU_STORAGE_INC=<nabu-storage headers> \
XNU_EXTERNAL_HEADERS=<xnu>/EXTERNAL_HEADERS \
./kext/compile_check.sh
```

## 3. Prelink

```sh
python3 tools/link_nabu_data_block.py \
  --kernel out/kernel.native-apfs.macho \
  --reference-object out/IOBlockStorageDevice-reference.o \
  --object out/NabuDiskCore.o \
  --object out/NabuSystemDisk.o \
  --object out/NabuSystemDiskKmod.o \
  --bundle-id org.nabu.driver.SystemDisk \
  --bundle-name 'Nabu System Disk' \
  --executable NabuSystemDisk \
  --bundle-path /System/Library/Extensions/NabuSystemDisk.kext \
  --start-symbol _nabu_system_disk_start \
  --report-name system-disk-link.json \
  --output out
```

`out/prelink-report.json` then lists `org.nabu.driver.SystemDisk` in
`components` and its segments in `extents`.

## Exclusivity

`NabuDataBlock` and `NabuSystemDisk` are two views of the same bytes. Linking
both would publish the Data window twice and risk a double APFS mount. Link
`NabuSystemDisk` **instead of** `NabuDataBlock`, and keep the old bundle only
for `nabu-system-disk=0` A/B boots.

## What is verified where

| step | verified by |
|---|---|
| GPT bytes, routing, read/write semantics | `make check` on Linux, and GitHub Actions |
| C++ compiles for arm64 iOS kernel | `kext/compile_check.sh`: all three objects build as `Mach-O 64-bit arm64`, `_kmod_info` defined, `_nabu_system_disk_start` left as an import |
| prelink and boot | must run on the macOS build host + device |
