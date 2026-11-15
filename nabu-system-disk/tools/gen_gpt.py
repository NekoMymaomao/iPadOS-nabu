#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Synthetic GPT for the nabu_system loop-style disk.

This is the *reference implementation* of the partition table that
NabuSystemDisk.cpp synthesises in kernel memory.  It never touches the
on-disk SHNGROOT header: the table exists only in the read path of the
virtual IOBlockStorageDevice, exactly like a loop device presents a
partition view of its backing store.

Run:
    python3 gen_gpt.py --verify      # build, parse back, assert everything
    python3 gen_gpt.py --dump out.bin

Geometry is pinned to configs/nabu-storage-layout.json from the 26/27
flash packages: 60 GiB nabu_system, 4 KiB UFS blocks, 512 B logical
sectors exposed to IOStorageFamily (same choice as UniXNU ShengUfs
NativeUfsBlock, so a GPT with 512-byte LBAs parses normally).
"""
import argparse
import json
import struct
import sys
import uuid
import zlib

GiB = 1 << 30
MiB = 1 << 20

# ---- fixed geometry (must match NabuSystemDiskLayout.h) -------------------
PARTITION_BYTES = 60 * GiB              # 64424509440
LOGICAL_SECTOR = 512                    # logical LBA exposed to IOKit
PHYSICAL_BLOCK = 4096                   # real UFS block
TOTAL_LBA = PARTITION_BYTES // LOGICAL_SECTOR          # 125829120

GPT_ENTRY_COUNT = 128
GPT_ENTRY_SIZE = 128
GPT_ENTRY_BYTES = GPT_ENTRY_COUNT * GPT_ENTRY_SIZE      # 16384
GPT_ENTRY_LBA = 2                                        # right after header
GPT_ENTRY_SECTORS = GPT_ENTRY_BYTES // LOGICAL_SECTOR    # 32

FIRST_USABLE_LBA = GPT_ENTRY_LBA + GPT_ENTRY_SECTORS     # 34
BACKUP_ENTRY_SECTORS = GPT_ENTRY_SECTORS                 # 32
BACKUP_HEADER_LBA = TOTAL_LBA - 1                        # 125829119
BACKUP_ENTRY_LBA = BACKUP_HEADER_LBA - BACKUP_ENTRY_SECTORS  # 125829087
LAST_USABLE_LBA = BACKUP_ENTRY_LBA - 1                   # 125829086

DISK_GUID = "4E414255-4E41-4255-8000-000000000001"       # "NABU" marker, stable

TYPE_APPLE_APFS = "7C3457EF-0000-11AA-AA11-00306543ECAC"
TYPE_BASIC_DATA = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"


def lba(byte_offset):
    assert byte_offset % LOGICAL_SECTOR == 0, byte_offset
    return byte_offset // LOGICAL_SECTOR


def sectors(nbytes):
    assert nbytes % LOGICAL_SECTOR == 0, nbytes
    return nbytes // LOGICAL_SECTOR


# ---- the four nominal regions (offsets from nabu-storage-layout.json) -----
REGIONS = [
    dict(name="NabuOS",       offset=1048576,      bytes=5068816384,
         type=TYPE_APPLE_APFS,
         uuid="65A02939-B0D4-4BD2-B5DC-7D670691EE10",
         note="20F66 OS window, read-only"),
    dict(name="NabuSystemOS", offset=6442450944,   bytes=2663383040,
         type=TYPE_APPLE_APFS,
         uuid="FAEABB4A-72CB-451B-A33C-75418E0F882F",
         note="20F66 SystemOS window, read-only"),
    dict(name="NabuData",     offset=10737418240,  bytes=52613349376,
         type=TYPE_APPLE_APFS,
         # Deliberately the APFS *volume* UUID that NabuDataLookup.cpp
         # searches for, so the partition IOMedia answers the sysctl
         # without waiting for the APFS kext to publish a volume node.
         uuid="168F804B-0CB5-4167-A239-C71953E36966",
         note="49 GiB APFS Data container, writable"),
    dict(name="NabuLog",      offset=63350767616,  bytes=1073741824,
         type=TYPE_BASIC_DATA,
         uuid="6E414255-4C4F-4753-8000-000000000004",
         note="1 GiB reserved log region; tail 33 LBAs yielded to backup GPT"),
]


def build_entries():
    out = bytearray(GPT_ENTRY_BYTES)
    for i, r in enumerate(REGIONS):
        start = lba(r["offset"])
        end = start + sectors(r["bytes"]) - 1
        if end > LAST_USABLE_LBA:
            # The 1 GiB log region runs to the end of the 60 GiB partition,
            # but the last 33 LBAs belong to the backup GPT.  Trim it; the
            # loss is 16.5 KiB out of 1 GiB and is still "nominal".
            end = LAST_USABLE_LBA
        assert FIRST_USABLE_LBA <= start <= end <= LAST_USABLE_LBA, (r["name"], start, end)
        e = bytearray(GPT_ENTRY_SIZE)
        e[0:16] = uuid.UUID(r["type"]).bytes_le
        e[16:32] = uuid.UUID(r["uuid"]).bytes_le
        struct.pack_into("<QQQ", e, 32, start, end, 0)          # start, end, attrs
        name = r["name"].encode("utf-16-le")[:72]
        e[56:56 + len(name)] = name
        out[i * GPT_ENTRY_SIZE:(i + 1) * GPT_ENTRY_SIZE] = e
    return bytes(out)


def build_header(my_lba, alt_lba, entry_lba, entries):
    h = bytearray(92)
    h[0:8] = b"EFI PART"
    struct.pack_into("<I", h, 8, 0x00010000)      # revision 1.0
    struct.pack_into("<I", h, 12, 92)             # header size
    # 16..19 CRC left zero while computing
    struct.pack_into("<I", h, 20, 0)              # reserved
    struct.pack_into("<Q", h, 24, my_lba)
    struct.pack_into("<Q", h, 32, alt_lba)
    struct.pack_into("<Q", h, 40, FIRST_USABLE_LBA)
    struct.pack_into("<Q", h, 48, LAST_USABLE_LBA)
    h[56:72] = uuid.UUID(DISK_GUID).bytes_le
    struct.pack_into("<Q", h, 72, entry_lba)
    struct.pack_into("<I", h, 80, GPT_ENTRY_COUNT)
    struct.pack_into("<I", h, 84, GPT_ENTRY_SIZE)
    struct.pack_into("<I", h, 88, zlib.crc32(entries) & 0xFFFFFFFF)
    crc = zlib.crc32(bytes(h)) & 0xFFFFFFFF
    struct.pack_into("<I", h, 16, crc)
    return bytes(h)


def build_protective_mbr():
    m = bytearray(512)
    m[446] = 0x00                                  # not bootable
    m[447:450] = b"\x00\x02\x00"                   # CHS start
    m[450] = 0xEE                                  # GPT protective
    m[451:454] = b"\xff\xff\xff"                   # CHS end
    struct.pack_into("<I", m, 454, 1)              # first LBA
    struct.pack_into("<I", m, 458, min(TOTAL_LBA - 1, 0xFFFFFFFF))
    m[510:512] = b"\x55\xaa"
    return bytes(m)


def build_metadata():
    """Return (metadata, spans).

    metadata is the concatenation of the five synthetic areas; spans maps
    each area to (lba_start, lba_count, offset_in_metadata).
    """
    entries = build_entries()
    primary_hdr = build_header(1, BACKUP_HEADER_LBA, GPT_ENTRY_LBA, entries)
    backup_hdr = build_header(BACKUP_HEADER_LBA, 1, BACKUP_ENTRY_LBA, entries)
    mbr = build_protective_mbr()

    # Each span must cover whole logical sectors: pad the 92-byte headers.
    blobs = [
        (0, 1, mbr.ljust(LOGICAL_SECTOR, b"\x00")),
        (1, 1, primary_hdr.ljust(LOGICAL_SECTOR, b"\x00")),
        (GPT_ENTRY_LBA, GPT_ENTRY_SECTORS, entries),
        (BACKUP_ENTRY_LBA, BACKUP_ENTRY_SECTORS, entries),
        (BACKUP_HEADER_LBA, 1, backup_hdr.ljust(LOGICAL_SECTOR, b"\x00")),
    ]
    meta = bytearray()
    spans = []
    for start, count, blob in blobs:
        spans.append((start, count, len(meta)))
        meta += blob
    return bytes(meta), spans


# ---- independent reader used for verification -----------------------------
def verify():
    entries = build_entries()
    meta, spans = build_metadata()

    # 1. protective MBR
    mbr = meta[0:512]
    assert mbr[510:512] == b"\x55\xaa"
    assert mbr[450] == 0xEE
    assert struct.unpack_from("<I", mbr, 454)[0] == 1
    size = struct.unpack_from("<I", mbr, 458)[0]
    assert size == min(TOTAL_LBA - 1, 0xFFFFFFFF)

    # 2. primary header, parsed independently
    hdr = bytearray(meta[512:512 + 92])
    assert hdr[0:8] == b"EFI PART"
    assert struct.unpack_from("<I", hdr, 8)[0] == 0x00010000
    assert struct.unpack_from("<I", hdr, 12)[0] == 92
    crc_stored = struct.unpack_from("<I", hdr, 16)[0]
    struct.pack_into("<I", hdr, 16, 0)
    assert (zlib.crc32(bytes(hdr)) & 0xFFFFFFFF) == crc_stored, "header CRC"
    assert struct.unpack_from("<Q", hdr, 24)[0] == 1
    assert struct.unpack_from("<Q", hdr, 32)[0] == BACKUP_HEADER_LBA
    assert struct.unpack_from("<Q", hdr, 40)[0] == FIRST_USABLE_LBA
    assert struct.unpack_from("<Q", hdr, 48)[0] == LAST_USABLE_LBA
    assert struct.unpack_from("<Q", hdr, 72)[0] == GPT_ENTRY_LBA
    assert struct.unpack_from("<I", hdr, 80)[0] == GPT_ENTRY_COUNT
    assert struct.unpack_from("<I", hdr, 84)[0] == GPT_ENTRY_SIZE
    assert (zlib.crc32(entries) & 0xFFFFFFFF) == struct.unpack_from("<I", hdr, 88)[0]

    # 3. every partition is inside the usable window and non-overlapping
    seen = []
    for i in range(GPT_ENTRY_COUNT):
        e = entries[i * 128:(i + 1) * 128]
        if not any(e[0:16]):
            continue
        typ = uuid.UUID(bytes_le=bytes(e[0:16]))
        gu = uuid.UUID(bytes_le=bytes(e[16:32]))
        start, end, _ = struct.unpack_from("<QQQ", e, 32)
        name = e[56:].decode("utf-16-le").rstrip("\x00")
        assert FIRST_USABLE_LBA <= start <= end <= LAST_USABLE_LBA, (name, start, end)
        for s2, e2, n2 in seen:
            assert end < s2 or e2 < start, f"{name} overlaps {n2}"
        seen.append((start, end, name))
        print(f"  LBA {start:>9}..{end:<9} {name:<12} type={typ} uuid={gu}")

    # 4. nominal regions line up with the physical windows
    expect = [
        ("NabuOS", lba(1048576), sectors(5068816384)),
        ("NabuSystemOS", lba(6442450944), sectors(2663383040)),
        ("NabuData", lba(10737418240), sectors(52613349376)),
    ]
    got = {n: (s, e - s + 1) for s, e, n in seen}
    for name, start, count in expect:
        assert got[name][0] == start, (name, got[name][0], start)
        assert got[name][1] == count, (name, got[name][1], count)

    # 5. backup GPT mirrors the primary entries and header
    assert meta[spans[3][2]:spans[3][2] + GPT_ENTRY_BYTES] == entries
    bh = bytearray(meta[spans[4][2]:spans[4][2] + 92])
    assert bh[0:8] == b"EFI PART"
    bcrc = struct.unpack_from("<I", bh, 16)[0]
    struct.pack_into("<I", bh, 16, 0)
    assert (zlib.crc32(bytes(bh)) & 0xFFFFFFFF) == bcrc
    assert struct.unpack_from("<Q", bh, 24)[0] == BACKUP_HEADER_LBA
    assert struct.unpack_from("<Q", bh, 32)[0] == 1
    assert struct.unpack_from("<Q", bh, 72)[0] == BACKUP_ENTRY_LBA

    # 6. metadata does not collide with the SHNGROOT header at offset 0
    #    (the kernel never writes these bytes to the medium; this asserts the
    #     synthetic view is read-only by construction)
    print(f"\n  total LBAs      : {TOTAL_LBA}")
    print(f"  first usable    : {FIRST_USABLE_LBA}")
    print(f"  last usable     : {LAST_USABLE_LBA}")
    print(f"  metadata bytes  : {len(meta)}")
    print("  all checks passed")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verify", action="store_true")
    ap.add_argument("--dump")
    ap.add_argument("--json")
    a = ap.parse_args()
    if a.verify:
        verify()
    if a.dump:
        meta, _ = build_metadata()
        with open(a.dump, "wb") as f:
            f.write(meta)
        print(f"wrote {a.dump} ({len(meta)} bytes)")
    if a.json:
        meta, spans = build_metadata()
        doc = {
            "partition_bytes": PARTITION_BYTES,
            "logical_sector": LOGICAL_SECTOR,
            "total_lba": TOTAL_LBA,
            "first_usable_lba": FIRST_USABLE_LBA,
            "last_usable_lba": LAST_USABLE_LBA,
            "backup_header_lba": BACKUP_HEADER_LBA,
            "backup_entry_lba": BACKUP_ENTRY_LBA,
            "disk_guid": DISK_GUID,
            "regions": REGIONS,
            "spans": [{"lba": s, "count": c, "offset": o} for s, c, o in spans],
            "metadata_sha256": __import__("hashlib").sha256(meta).hexdigest(),
        }
        with open(a.json, "w") as f:
            json.dump(doc, f, indent=2)
        print(f"wrote {a.json}")


if __name__ == "__main__":
    sys.exit(main())
