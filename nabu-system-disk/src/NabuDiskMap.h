// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
// LBA -> storage-window routing for the nabu_system loop-style disk.
//
// The virtual device covers the whole 60 GiB partition.  Behind it sit
// three real windows (OS, SystemOS, Data) plus the reserved log region.
// Anything that is not backed by a window -- the gaps the user asked us to
// "pretend do not exist" -- reads as zeros and refuses writes, so a stray
// scan can never corrupt the gaps or the SHNGROOT header.
//
// IOKit-free: host-testable.
#include "NabuGpt.h"

namespace nabu {

struct Window {
    uint64_t offset;   // bytes from the start of nabu_system
    uint64_t bytes;
    unsigned index;    // nabu_storage_* window number
    bool writable;
};

// Mirrors NabuStorageLayout.h: OS=0, SystemOS=1, Data=2 (writable).
static constexpr Window kWindows[3] = {
    {1048576,    5068816384,  0, false},
    {6442450944, 2663383040,  1, false},
    {10737418240, 52613349376, 2, true},
};

enum class RouteKind {
    Metadata,   // synthetic GPT bytes
    Window,     // backed by nabu_storage_* window
    Reserved,   // log region: no window, reads zero, writes refuse
    Gap,        // unmapped gap: reads zero, writes refuse
};

struct Route {
    RouteKind kind;
    unsigned window;      // valid for RouteKind::Window
    uint64_t windowOffset;  // bytes into the window (RouteKind::Window only)
    uint32_t metaOffset;   // into the metadata blob (RouteKind::Metadata only)
};

// Resolve one logical sector.  Returns false only for out-of-range LBAs.
static inline bool routeLba(uint64_t lba, Route *out)
{
    if (lba >= kTotalLba) return false;
    uint32_t meta = 0;
    if (syntheticOffset(lba, &meta)) {
        *out = {RouteKind::Metadata, 0, 0, meta};
        return true;
    }
    const uint64_t at = lba * kLogicalSector;
    for (unsigned i = 0; i < 3; ++i) {
        const Window &w = kWindows[i];
        if (at >= w.offset && at < w.offset + w.bytes) {
            *out = {RouteKind::Window, w.index, at - w.offset, 0};
            return true;
        }
    }
    // Reserved log region sits at 59 GiB..60 GiB.
    if (at >= kRegions[3].offset && at < kRegions[3].offset + kRegions[3].bytes) {
        *out = {RouteKind::Reserved, 0, 0, 0};
        return true;
    }
    *out = {RouteKind::Gap, 0, 0, 0};
    return true;
}

// Largest number of contiguous sectors that share a route starting at lba.
static inline uint64_t contiguousSectors(uint64_t lba, uint64_t maxSectors)
{
    Route first;
    if (!routeLba(lba, &first)) return 0;
    uint64_t n = 1;
    while (n < maxSectors) {
        Route next;
        if (!routeLba(lba + n, &next)) break;
        if (next.kind != first.kind) break;
        if (first.kind == RouteKind::Window &&
            (next.window != first.window ||
             next.windowOffset != first.windowOffset + n * kLogicalSector))
            break;
        if (first.kind == RouteKind::Metadata &&
            next.metaOffset != first.metaOffset + n * kLogicalSector)
            break;
        ++n;
    }
    return n;
}

}  // namespace nabu
