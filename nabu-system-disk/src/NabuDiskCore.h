// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
// Read/write engine for the nabu_system loop-style disk.
//
// Deliberately free of IOKit: the kernel glue (NabuSystemDisk.cpp) only owns
// the IOBlockStorageDevice plumbing and delegates every byte to this engine,
// so the routing and the synthetic GPT are exercised by host tests.
//
// Semantics, in one paragraph: the virtual disk covers the whole 60 GiB
// nabu_system partition.  LBA 0..33 and the last 33 LBAs are served from an
// in-memory GPT (never written to the medium, so the SHNGROOT header and
// every flashed byte stay untouched).  The OS / SystemOS / Data windows are
// forwarded byte-for-byte to nabu_storage_read/write.  Everything else --
// the gaps the user asked us to pretend do not exist, plus the reserved log
// region -- reads as zeros and refuses writes, so a stray scan can neither
// corrupt a gap nor mistake it for content.
#include "NabuDiskMap.h"

namespace nabu {

// Mirrors EINVAL / EIO / EROFS / ENXIO so the kernel and the host agree
// without pulling <sys/errno.h> into the host test.
enum : int {
    kOk = 0,
    kIoError = 5,
    kInvalid = 22,
    kReadOnly = 30,
    kNoDevice = 6,
};

struct Backend {
    void *context;
    // Both return 0 on success or a positive errno.  `offset` is relative to
    // the start of the window, and `bytes` is always a multiple of 512.
    int (*read)(void *context, unsigned window, uint64_t offset, void *out, size_t bytes);
    int (*write)(void *context, unsigned window, uint64_t offset, const void *in, size_t bytes);
};

struct Stats {
    uint64_t reads, readBytes, readZeroFilled;
    uint64_t writes, writeBytes, writeShadowed, writeRefused;
};

class Disk {
public:
    Disk() = default;

    // Builds the synthetic GPT.  Returns false only if a region is malformed,
    // which would be a build-time bug rather than a runtime condition.
    bool init(const Backend &backend);

    // `bytes` must be a multiple of 512 and `offset + bytes` must stay inside
    // the 60 GiB partition; anything else is kInvalid.
    int read(uint64_t offset, void *out, size_t bytes);
    int write(uint64_t offset, const void *in, size_t bytes);

    const Stats &stats() const { return fStats; }
    const uint8_t *metadata() const { return fMeta; }
    static constexpr size_t kMetaBytes = nabu::kMetaBytes;

private:
    int transfer(uint64_t offset, void *out, const void *in, size_t bytes, bool write);

    Backend fBackend{};
    Stats fStats{};
    uint8_t fMeta[kMetaBytes] = {};
    bool fReady = false;
};

}  // namespace nabu
