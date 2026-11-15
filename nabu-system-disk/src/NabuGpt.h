// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
// Synthetic GPT for the nabu_system loop-style disk.
//
// The table is built in kernel memory and served only from the virtual
// device's read path.  Nothing is ever written to the medium, so the
// SHNGROOT header at offset 0 and every flashed byte stay untouched: the
// device presents a *view*, exactly like a loop device does for its
// backing store.
//
// This header is deliberately free of IOKit dependencies so the same code
// is compiled by the host test (tests/test_disk_map.cpp) and compared byte
// for byte against tools/gen_gpt.py.
#include <stdint.h>
#include <stddef.h>
#if defined(KERNEL)
#include <libkern/libkern.h>     // memcpy/memset live here inside the kernel
#else
#include <string.h>
#endif

namespace nabu {

static constexpr uint64_t kGiB = 1073741824ULL;
static constexpr uint64_t kPartitionBytes = 60 * kGiB;   // 64424509440
static constexpr uint32_t kLogicalSector = 512;          // LBA exposed to IOKit
static constexpr uint64_t kTotalLba = kPartitionBytes / kLogicalSector;

static constexpr uint32_t kEntryCount = 128;
static constexpr uint32_t kEntrySize = 128;
static constexpr uint32_t kEntryBytes = kEntryCount * kEntrySize;   // 16384
static constexpr uint64_t kEntryLba = 2;
static constexpr uint32_t kEntrySectors = kEntryBytes / kLogicalSector;   // 32
static constexpr uint64_t kFirstUsableLba = kEntryLba + kEntrySectors;    // 34
static constexpr uint64_t kBackupHeaderLba = kTotalLba - 1;               // 125829119
static constexpr uint64_t kBackupEntryLba = kBackupHeaderLba - kEntrySectors;  // 125829087
static constexpr uint64_t kLastUsableLba = kBackupEntryLba - 1;           // 125829086

// Metadata as concatenated spans served by the read path.
static constexpr uint32_t kMbrBytes = 512;
static constexpr uint32_t kHeaderBytes = 512;            // 92 used, sector padded
static constexpr uint32_t kMetaBytes =
    kMbrBytes + kHeaderBytes + kEntryBytes + kEntryBytes + kHeaderBytes;   // 34304

struct GptSpan {
    uint64_t lba;
    uint32_t sectors;
    uint32_t offset;      // into the metadata blob
};

// Served order: protective MBR, primary header, primary entries,
// backup entries, backup header.
static constexpr GptSpan kSpans[5] = {
    {0, 1, 0},
    {1, 1, kMbrBytes},
    {kEntryLba, kEntrySectors, kMbrBytes + kHeaderBytes},
    {kBackupEntryLba, kEntrySectors, kMbrBytes + kHeaderBytes + kEntryBytes},
    {kBackupHeaderLba, 1, kMbrBytes + kHeaderBytes + 2 * kEntryBytes},
};

static inline uint32_t crc32(const uint8_t *data, size_t bytes)
{
    uint32_t crc = 0xffffffffU;
    for (size_t i = 0; i < bytes; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
    }
    return ~crc;
}

// "7C3457EF-0000-11AA-AA11-00306543ECAC" -> 16 GPT bytes (mixed endian).
static inline bool parseGuid(const char *text, uint8_t out[16])
{
    uint8_t raw[16];
    unsigned nibble = 0;
    int high = -1;
    for (const char *p = text; *p; ++p) {
        int value;
        if (*p == '-') continue;
        if (*p >= '0' && *p <= '9') value = *p - '0';
        else if (*p >= 'a' && *p <= 'f') value = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') value = *p - 'A' + 10;
        else return false;
        if (high < 0) { high = value; }
        else {
            if (nibble >= 16) return false;
            raw[nibble++] = static_cast<uint8_t>((high << 4) | value);
            high = -1;
        }
    }
    if (nibble != 16 || high >= 0) return false;
    // GPT stores the first three fields little-endian.
    out[0] = raw[3]; out[1] = raw[2]; out[2] = raw[1]; out[3] = raw[0];
    out[4] = raw[5]; out[5] = raw[4];
    out[6] = raw[7]; out[7] = raw[6];
    for (unsigned i = 8; i < 16; ++i) out[i] = raw[i];
    return true;
}

static inline void putLe32(uint8_t *at, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) at[i] = static_cast<uint8_t>(value >> (8 * i));
}

static inline void putLe64(uint8_t *at, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) at[i] = static_cast<uint8_t>(value >> (8 * i));
}

struct Region {
    const char *name;
    uint64_t offset;      // bytes from the start of nabu_system
    uint64_t bytes;
    const char *type;
    const char *uuid;
};

// Offsets and sizes are pinned to configs/nabu-storage-layout.json.
static constexpr Region kRegions[4] = {
    {"NabuOS",       1048576,    5068816384,  "7C3457EF-0000-11AA-AA11-00306543ECAC",
     "65A02939-B0D4-4BD2-B5DC-7D670691EE10"},
    {"NabuSystemOS", 6442450944, 2663383040,  "7C3457EF-0000-11AA-AA11-00306543ECAC",
     "FAEABB4A-72CB-451B-A33C-75418E0F882F"},
    // The unique GUID is the APFS *volume* UUID NabuDataLookup.cpp looks
    // for, so the partition IOMedia answers kern.nabu_data_device directly.
    {"NabuData",     10737418240, 52613349376, "7C3457EF-0000-11AA-AA11-00306543ECAC",
     "168F804B-0CB5-4167-A239-C71953E36966"},
    {"NabuLog",      63350767616, 1073741824,  "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7",
     "6E414255-4C4F-4753-8000-000000000004"},
};

static constexpr const char *kDiskGuid = "4E414255-4E41-4255-8000-000000000001";

static inline void putUtf16Name(uint8_t *entry, const char *name)
{
    for (unsigned i = 0; i < 36; ++i) {
        const char c = name[i];
        if (!c) break;
        entry[56 + 2 * i] = static_cast<uint8_t>(c);
        entry[57 + 2 * i] = 0;
    }
}

static inline void buildEntries(uint8_t *out)
{
    memset(out, 0, kEntryBytes);
    for (unsigned i = 0; i < 4; ++i) {
        const Region &r = kRegions[i];
        uint8_t *entry = out + i * kEntrySize;
        if (!parseGuid(r.type, entry) || !parseGuid(r.uuid, entry + 16)) continue;
        uint64_t start = r.offset / kLogicalSector;
        uint64_t end = start + r.bytes / kLogicalSector - 1;
        // The 1 GiB log region reaches the end of the 60 GiB partition, but
        // the final 33 LBAs belong to the backup GPT.  Trim it.
        if (end > kLastUsableLba) end = kLastUsableLba;
        putLe64(entry + 32, start);
        putLe64(entry + 40, end);
        putUtf16Name(entry, r.name);
    }
}

static inline void buildHeader(uint8_t *out, uint64_t myLba, uint64_t altLba,
                               uint64_t entryLba, const uint8_t *entries)
{
    memset(out, 0, kHeaderBytes);
    memcpy(out, "EFI PART", 8);
    putLe32(out + 8, 0x00010000U);       // revision 1.0
    putLe32(out + 12, 92);               // header size
    putLe64(out + 24, myLba);
    putLe64(out + 32, altLba);
    putLe64(out + 40, kFirstUsableLba);
    putLe64(out + 48, kLastUsableLba);
    uint8_t guid[16];
    if (parseGuid(kDiskGuid, guid)) memcpy(out + 56, guid, 16);
    putLe64(out + 72, entryLba);
    putLe32(out + 80, kEntryCount);
    putLe32(out + 84, kEntrySize);
    putLe32(out + 88, crc32(entries, kEntryBytes));
    putLe32(out + 16, crc32(out, 92));
}

static inline void buildProtectiveMbr(uint8_t *out)
{
    memset(out, 0, kMbrBytes);
    out[446] = 0x00;
    out[447] = 0x00; out[448] = 0x02; out[449] = 0x00;
    out[450] = 0xee;
    out[451] = 0xff; out[452] = 0xff; out[453] = 0xff;
    putLe32(out + 454, 1);
    putLe32(out + 458, static_cast<uint32_t>(kTotalLba - 1 > 0xffffffffULL
                                                 ? 0xffffffffULL
                                                 : kTotalLba - 1));
    out[510] = 0x55; out[511] = 0xaa;
}

// Build the whole metadata blob (kMetaBytes).
//
// The blob is assembled in place inside the caller's buffer.  Keeping the two
// 16 KiB entry arrays off the stack matters: a 32 KiB frame makes clang emit a
// call to ___chkstk_darwin, which the prelinked kernel does not export, so the
// relink fails on that symbol.
static inline void buildMetadata(uint8_t *out)
{
    uint8_t *primary = out + kMbrBytes;
    uint8_t *entries = primary + kHeaderBytes;
    uint8_t *backupEntries = entries + kEntryBytes;
    uint8_t *backup = backupEntries + kEntryBytes;

    buildProtectiveMbr(out);
    buildEntries(entries);
    memcpy(backupEntries, entries, kEntryBytes);
    buildHeader(primary, 1, kBackupHeaderLba, kEntryLba, entries);
    buildHeader(backup, kBackupHeaderLba, 1, kBackupEntryLba, entries);
}

// If [lba, lba+sectors) touches synthetic metadata, return the number of
// contiguous bytes available from the blob for the request starting at lba.
// 0 means the range is backed by the medium instead.
static inline uint32_t syntheticBytes(uint64_t lba, uint64_t sectors,
                                      const uint8_t **bytesAt)
{
    for (unsigned i = 0; i < 5; ++i) {
        const GptSpan &s = kSpans[i];
        if (lba < s.lba || lba >= s.lba + s.sectors) continue;
        const uint64_t inside = lba - s.lba;
        const uint64_t avail = (s.sectors - inside) * kLogicalSector;
        const uint64_t want = sectors * kLogicalSector;
        *bytesAt = nullptr;   // caller supplies the blob pointer
        return static_cast<uint32_t>(avail < want ? avail : want);
    }
    return 0;
}

static inline bool syntheticOffset(uint64_t lba, uint32_t *offset)
{
    for (unsigned i = 0; i < 5; ++i) {
        const GptSpan &s = kSpans[i];
        if (lba < s.lba || lba >= s.lba + s.sectors) continue;
        *offset = s.offset + static_cast<uint32_t>((lba - s.lba) * kLogicalSector);
        return true;
    }
    return false;
}

}  // namespace nabu
