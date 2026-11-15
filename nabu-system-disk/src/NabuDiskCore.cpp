// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NabuDiskCore.h"

#include <string.h>

namespace nabu {

bool Disk::init(const Backend &backend)
{
    if (!backend.read || !backend.write) return false;
    fBackend = backend;
    buildMetadata(fMeta);
    fReady = true;
    return true;
}

int Disk::read(uint64_t offset, void *out, size_t bytes)
{
    return transfer(offset, out, nullptr, bytes, false);
}

int Disk::write(uint64_t offset, const void *in, size_t bytes)
{
    return transfer(offset, nullptr, in, bytes, true);
}

int Disk::transfer(uint64_t offset, void *out, const void *in, size_t bytes, bool write)
{
    if (!fReady) return kNoDevice;
    if (!bytes) return kOk;
    if ((offset | bytes) & (kLogicalSector - 1)) return kInvalid;
    if (offset > kPartitionBytes || bytes > kPartitionBytes - offset) return kInvalid;

    uint8_t *dst = static_cast<uint8_t *>(out);
    const uint8_t *src = static_cast<const uint8_t *>(in);
    uint64_t lba = offset / kLogicalSector;
    uint64_t sectors = bytes / kLogicalSector;
    size_t done = 0;

    while (sectors) {
        Route route;
        if (!routeLba(lba, &route)) return kInvalid;

        uint64_t span = contiguousSectors(lba, sectors);
        if (!span) return kInvalid;
        const size_t chunk = static_cast<size_t>(span * kLogicalSector);

        switch (route.kind) {
        case RouteKind::Metadata:
            // GPT bytes are never persisted.  Writes are accepted and dropped
            // so a partitioning tool cannot rewrite the table, and -- more
            // importantly -- cannot touch LBA 0, which is the SHNGROOT header
            // on the medium.
            if (write) {
                ++fStats.writes;
                fStats.writeBytes += chunk;
                fStats.writeShadowed += chunk;
            } else {
                memcpy(dst + done, fMeta + route.metaOffset, chunk);
                ++fStats.reads;
                fStats.readBytes += chunk;
            }
            break;

        case RouteKind::Window: {
            const int error = write
                ? fBackend.write(fBackend.context, route.window, route.windowOffset,
                                 src + done, chunk)
                : fBackend.read(fBackend.context, route.window, route.windowOffset,
                                dst + done, chunk);
            if (write) {
                ++fStats.writes;
                if (error) { fStats.writeRefused += chunk; return error; }
                fStats.writeBytes += chunk;
            } else {
                if (error) return error;
                ++fStats.reads;
                fStats.readBytes += chunk;
            }
            break;
        }

        case RouteKind::Reserved:
        case RouteKind::Gap:
            if (write) {
                // The log reserve is written through nabu_storage directly,
                // never through the IOKit view.  A write here means some
                // client is trying to scribble outside every real window.
                ++fStats.writes;
                fStats.writeRefused += chunk;
                return kReadOnly;
            }
            memset(dst + done, 0, chunk);
            ++fStats.reads;
            fStats.readBytes += chunk;
            fStats.readZeroFilled += chunk;
            break;
        }

        lba += span;
        sectors -= span;
        done += chunk;
    }
    return kOk;
}

}  // namespace nabu
