// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test for the loop-style read/write engine.
//
//   g++ -std=c++17 -O1 -Wall -Wextra -I../src test_disk_core.cpp
//       ../src/NabuDiskCore.cpp -o test_disk_core
//   ./test_disk_core [python-reference.bin]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "NabuDiskCore.h"

static int failures = 0;

#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);  \
            std::printf(__VA_ARGS__);                        \
            std::printf("\n");                               \
            ++failures;                                      \
        }                                                    \
    } while (0)

// A window backend that behaves like the real transport: byte content is a
// deterministic function of the window and the offset, so a routing mistake
// shows up as a content mismatch instead of a silent pass.
struct Fake {
    uint64_t bytes[3] = {5068816384ULL, 2663383040ULL, 52613349376ULL};
    bool writable[3] = {false, false, true};
    uint64_t reads[3] = {};
    uint64_t writes[3] = {};
    uint64_t lastReadOffset[3] = {};
    size_t lastReadBytes[3] = {};
    uint64_t lastWriteOffset[3] = {};
    std::vector<uint8_t> lastWrite;
};

static uint8_t pattern(unsigned window, uint64_t offset)
{
    return static_cast<uint8_t>((offset * 131u) ^ (window * 37u) ^ (offset >> 13));
}

static int fakeRead(void *context, unsigned window, uint64_t offset, void *out, size_t bytes)
{
    Fake *fake = static_cast<Fake *>(context);
    if (window >= 3) return nabu::kInvalid;
    if (offset > fake->bytes[window] || bytes > fake->bytes[window] - offset)
        return nabu::kIoError;
    uint8_t *dst = static_cast<uint8_t *>(out);
    for (size_t i = 0; i < bytes; ++i) dst[i] = pattern(window, offset + i);
    ++fake->reads[window];
    fake->lastReadOffset[window] = offset;
    fake->lastReadBytes[window] = bytes;
    return 0;
}

static int fakeWrite(void *context, unsigned window, uint64_t offset, const void *in, size_t bytes)
{
    Fake *fake = static_cast<Fake *>(context);
    if (window >= 3) return nabu::kInvalid;
    if (!fake->writable[window]) return nabu::kReadOnly;
    if (offset > fake->bytes[window] || bytes > fake->bytes[window] - offset)
        return nabu::kIoError;
    ++fake->writes[window];
    fake->lastWriteOffset[window] = offset;
    fake->lastWrite.assign(static_cast<const uint8_t *>(in),
                           static_cast<const uint8_t *>(in) + bytes);
    return 0;
}

int main(int argc, char **argv)
{
    Fake fake;
    nabu::Backend backend = {&fake, fakeRead, fakeWrite};
    nabu::Disk disk;
    CHECK(disk.init(backend), "init");

    // 1. The engine's blob is the same one the reference test validated.
    std::vector<uint8_t> expect(nabu::kMetaBytes);
    nabu::buildMetadata(expect.data());
    CHECK(std::memcmp(disk.metadata(), expect.data(), nabu::kMetaBytes) == 0,
          "engine metadata differs from buildMetadata()");

    if (argc > 1) {
        FILE *f = std::fopen(argv[1], "rb");
        if (!f) {
            std::printf("FAIL: cannot open %s\n", argv[1]);
            return 2;
        }
        std::vector<uint8_t> ref(nabu::kMetaBytes + 1);
        const size_t got = std::fread(ref.data(), 1, ref.size(), f);
        std::fclose(f);
        CHECK(got == nabu::kMetaBytes, "reference size %zu", got);
        if (got == nabu::kMetaBytes)
            CHECK(std::memcmp(disk.metadata(), ref.data(), nabu::kMetaBytes) == 0,
                  "engine metadata differs from the Python reference");
    }

    // 2. LBA 0 and 1 are served from the blob, never from the medium.
    uint8_t sector[512];
    CHECK(disk.read(0, sector, 512) == nabu::kOk, "read LBA 0");
    CHECK(sector[510] == 0x55 && sector[511] == 0xaa, "protective MBR signature");
    CHECK(std::memcmp(sector, disk.metadata(), 512) == 0, "LBA 0 content");
    CHECK(disk.read(512, sector, 512) == nabu::kOk, "read LBA 1");
    CHECK(std::memcmp(sector, "EFI PART", 8) == 0, "primary GPT signature");
    CHECK(std::memcmp(sector, disk.metadata() + 512, 512) == 0, "LBA 1 content");

    // 3. A single 17 KiB request must cross the three head spans in one go.
    {
        std::vector<uint8_t> head(17408);
        CHECK(disk.read(0, head.data(), head.size()) == nabu::kOk, "read head 17 KiB");
        CHECK(std::memcmp(head.data(), disk.metadata(), head.size()) == 0,
              "head spans are not contiguous");
    }

    // 4. OS window: byte-for-byte from window 0, offset 0.
    {
        std::vector<uint8_t> buf(4096);
        CHECK(disk.read(1048576, buf.data(), buf.size()) == nabu::kOk, "read OS start");
        CHECK(fake.reads[0] == 1 && fake.lastReadOffset[0] == 0, "OS routed to window 0");
        for (size_t i = 0; i < buf.size(); ++i)
            if (buf[i] != pattern(0, i)) {
                CHECK(false, "OS content mismatch at %zu", i);
                break;
            }
    }

    // 5. Crossing the OS start boundary: gap zeros, then window bytes.
    {
        std::vector<uint8_t> buf(1024);
        CHECK(disk.read(1048576 - 512, buf.data(), buf.size()) == nabu::kOk,
              "read across OS start");
        bool zeros = true;
        for (size_t i = 0; i < 512; ++i) zeros = zeros && buf[i] == 0;
        CHECK(zeros, "gap half must read as zeros");
        for (size_t i = 0; i < 512; ++i)
            if (buf[512 + i] != pattern(0, i)) {
                CHECK(false, "OS half mismatch at %zu", i);
                break;
            }
    }

    // 6. Crossing the Data end: window bytes, then reserved zeros.
    {
        const uint64_t boundary = 63350767616ULL;   // 59 GiB
        std::vector<uint8_t> buf(1024);
        CHECK(disk.read(boundary - 512, buf.data(), buf.size()) == nabu::kOk,
              "read across Data end");
        const uint64_t dataStart = 10737418240ULL;
        for (size_t i = 0; i < 512; ++i)
            if (buf[i] != pattern(2, boundary - 512 - dataStart + i)) {
                CHECK(false, "Data tail mismatch at %zu", i);
                break;
            }
        bool zeros = true;
        for (size_t i = 512; i < 1024; ++i) zeros = zeros && buf[i] == 0;
        CHECK(zeros, "log reserve must read as zeros");
    }

    // 7. Writes: Data accepted, OS refused, gap refused, GPT shadowed.
    {
        std::vector<uint8_t> data(4096, 0xa5);
        CHECK(disk.write(10737418240ULL, data.data(), data.size()) == nabu::kOk,
              "Data write");
        CHECK(fake.writes[2] == 1 && fake.lastWriteOffset[2] == 0, "Data write routed");
        CHECK(fake.lastWrite.size() == data.size() &&
                  std::memcmp(fake.lastWrite.data(), data.data(), data.size()) == 0,
              "Data write content");

        const uint64_t before = disk.stats().writeRefused;
        CHECK(disk.write(1048576, data.data(), 512) == nabu::kReadOnly, "OS write refused");
        CHECK(disk.stats().writeRefused > before, "refused counter");
        CHECK(fake.writes[0] == 0, "OS write must not reach the transport");

        const uint64_t gap = 12000000ULL * 512;
        CHECK(disk.write(gap, data.data(), 512) == nabu::kReadOnly, "gap write refused");

        std::vector<uint8_t> snapshot(disk.metadata(), disk.metadata() + nabu::kMetaBytes);
        CHECK(disk.write(512, data.data(), 512) == nabu::kOk, "GPT write must be accepted");
        CHECK(std::memcmp(disk.metadata(), snapshot.data(), snapshot.size()) == 0,
              "GPT write must be dropped, not applied");
        CHECK(disk.stats().writeShadowed == 512, "shadowed counter");
    }

    // 8. Range and alignment checks.
    {
        std::vector<uint8_t> buf(1024);
        CHECK(disk.read(1, buf.data(), 512) == nabu::kInvalid, "unaligned offset");
        CHECK(disk.read(0, buf.data(), 513) == nabu::kInvalid, "unaligned length");
        CHECK(disk.read(64424509440ULL, buf.data(), 512) == nabu::kInvalid, "past the end");
        CHECK(disk.read(64424509440ULL - 512, buf.data(), 1024) == nabu::kInvalid,
              "straddling the end");
    }

    // 9. The final LBA is the backup header, still served from the blob.
    {
        uint8_t tail[512];
        CHECK(disk.read(64424509440ULL - 512, tail, 512) == nabu::kOk, "read last LBA");
        CHECK(std::memcmp(tail, disk.metadata() + nabu::kMetaBytes - 512, 512) == 0,
              "backup header content");
    }

    // 10. A 128 KiB request inside Data stays a single transport call.
    {
        std::vector<uint8_t> buf(128 * 1024);
        const uint64_t at = 10737418240ULL + (1ULL << 30);
        const uint64_t beforeReads = fake.reads[2];
        CHECK(disk.read(at, buf.data(), buf.size()) == nabu::kOk, "128 KiB read");
        CHECK(fake.reads[2] == beforeReads + 1, "128 KiB must not be split");
        CHECK(fake.lastReadBytes[2] == buf.size(), "128 KiB transfer size");
    }

    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("core checks passed: reads=%llu writes=%llu refused=%llu shadowed=%llu\n",
                static_cast<unsigned long long>(disk.stats().reads),
                static_cast<unsigned long long>(disk.stats().writes),
                static_cast<unsigned long long>(disk.stats().writeRefused),
                static_cast<unsigned long long>(disk.stats().writeShadowed));
    return 0;
}
