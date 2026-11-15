// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test: the kernel-side synthetic GPT must match the Python reference
// byte for byte, and every LBA must route where the layout says it does.
//
//   g++ -std=c++17 -I../src test_disk_map.cpp -o test_disk_map
//   ./test_disk_map /tmp/nabu-gpt-meta.bin
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "NabuDiskMap.h"

static int failures = 0;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (!(cond)) {                                      \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                       \
            std::printf("\n");                              \
            ++failures;                                     \
        }                                                   \
    } while (0)

int main(int argc, char **argv)
{
    // 1. Byte-identical to the Python reference implementation.
    std::vector<uint8_t> meta(nabu::kMetaBytes);
    nabu::buildMetadata(meta.data());

    if (argc > 1) {
        FILE *f = std::fopen(argv[1], "rb");
        if (!f) {
            std::printf("FAIL: cannot open %s\n", argv[1]);
            return 2;
        }
        std::vector<uint8_t> ref(nabu::kMetaBytes + 1);
        const size_t got = std::fread(ref.data(), 1, ref.size(), f);
        std::fclose(f);
        CHECK(got == nabu::kMetaBytes, "reference size %zu != %u", got, nabu::kMetaBytes);
        if (got == nabu::kMetaBytes) {
            for (uint32_t i = 0; i < nabu::kMetaBytes; ++i) {
                if (meta[i] != ref[i]) {
                    std::printf("FAIL: byte %u: kernel %02x python %02x\n",
                                i, meta[i], ref[i]);
                    ++failures;
                    break;
                }
            }
        }
    } else {
        std::printf("note: no reference blob, skipping byte comparison\n");
    }

    // 2. Structural assertions on the built blob.
    CHECK(meta[510] == 0x55 && meta[511] == 0xaa, "protective MBR signature");
    CHECK(std::memcmp(&meta[512], "EFI PART", 8) == 0, "primary GPT signature");
    CHECK(std::memcmp(&meta[nabu::kMetaBytes - 512], "EFI PART", 8) == 0,
          "backup GPT signature");

    uint8_t hdr[92];
    std::memcpy(hdr, &meta[512], 92);
    uint32_t stored = 0;
    std::memcpy(&stored, hdr + 16, 4);
    uint8_t zero[92];
    std::memcpy(zero, hdr, 92);
    std::memset(zero + 16, 0, 4);
    const uint32_t recomputed = nabu::crc32(zero, 92);
    CHECK(stored == recomputed, "primary header self CRC %08x vs %08x",
          stored, recomputed);

    // 3. Every region maps to the window the layout declares.
    struct Expect { uint64_t offset; unsigned window; uint64_t bytes; };
    static const Expect expect[3] = {
        {1048576, 0, 5068816384},
        {6442450944, 1, 2663383040},
        {10737418240, 2, 52613349376},
    };
    for (unsigned i = 0; i < 3; ++i) {
        const uint64_t first = expect[i].offset / nabu::kLogicalSector;
        const uint64_t last = first + expect[i].bytes / nabu::kLogicalSector - 1;
        nabu::Route r;
        CHECK(nabu::routeLba(first, &r), "route first of window %u", i);
        CHECK(r.kind == nabu::RouteKind::Window && r.window == expect[i].window &&
                  r.windowOffset == 0,
              "window %u start: kind=%d window=%u off=%llu", i, (int)r.kind,
              r.window, (unsigned long long)r.windowOffset);
        CHECK(nabu::routeLba(last, &r), "route last of window %u", i);
        CHECK(r.kind == nabu::RouteKind::Window && r.window == expect[i].window &&
                  r.windowOffset + nabu::kLogicalSector == expect[i].bytes,
              "window %u end: kind=%d window=%u off=%llu", i, (int)r.kind,
              r.window, (unsigned long long)r.windowOffset);
    }

    // 4. GPT metadata wins over the SHNGROOT header at LBA 0.
    nabu::Route r = {};
    CHECK(nabu::routeLba(0, &r) && r.kind == nabu::RouteKind::Metadata &&
              r.metaOffset == 0,
          "LBA 0 must serve the protective MBR");
    CHECK(nabu::routeLba(1, &r) && r.kind == nabu::RouteKind::Metadata &&
              r.metaOffset == 512,
          "LBA 1 must serve the primary header");

    // 5. Gaps read as zero-class routes, never as a window.
    const uint64_t gapLba = 12000000;   // ~5.7 GiB, between OS and SystemOS
    CHECK(nabu::routeLba(gapLba, &r) && r.kind == nabu::RouteKind::Gap,
          "gap LBA %llu kind=%d", (unsigned long long)gapLba, (int)r.kind);

    // 6. Reserved log region.
    const uint64_t logLba = 63350767616ULL / nabu::kLogicalSector;
    CHECK(nabu::routeLba(logLba, &r) && r.kind == nabu::RouteKind::Reserved,
          "log LBA %llu kind=%d", (unsigned long long)logLba, (int)r.kind);

    // 7. Contiguity: a full 128 KiB read inside Data must be one run.
    const uint64_t dataLba = 10737418240ULL / nabu::kLogicalSector + 4096;
    CHECK(nabu::contiguousSectors(dataLba, 256) == 256, "Data contiguity");

    // 8. Out of range.
    CHECK(!nabu::routeLba(nabu::kTotalLba, &r), "LBA past the end must fail");

    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all host checks passed (%u metadata bytes)\n", nabu::kMetaBytes);
    return 0;
}
