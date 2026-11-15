// SPDX-License-Identifier: BSD-2-Clause-Patent
// Publish the whole 60 GiB nabu_system as ONE virtual disk with a synthetic
// GPT, the way a loop device exposes partitions of its backing store.
//
// Why this exists
// ---------------
// IOStorageFamily only hands APFS a mountable node when a partition scheme
// has published a per-partition IOMedia carrying
//
//     Content Hint = Apple_APFS
//
// APFS itself never guesses a container offset inside a bare window.  The
// previous code published the Data window alone (NabuDataBlock.cpp: "only the
// Data window, never the containing LUN"), so no IOGUIDPartitionScheme ever
// ran, no IOMedia carried Apple_APFS, and NabuDataLookup.cpp -- which scans
// IOMedia for UUID == 168f804b-... -- always came up empty.  That is the
// `find persistent APFS Data phase=5 errno=0` failure.
//
// Nothing here writes to the medium.  LBA 0..33 and the last 33 LBAs are
// synthesised in RAM from NabuGpt.h, so the SHNGROOT header, the official
// root header and every flashed byte are untouched; the table is a *view*.
//
// ABI note
// --------
// This translation unit exports `nabu_data_block_start`, the symbol
// NabuDataKmod.cpp calls and the bundle NabuNativeApfs.c loads by id
// (`org.nabu.driver.DataBlock`).  Keeping both names means the change is a
// pure object swap: no new kmod, no bundle rename, nothing else relinked.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSString.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>

#include "NabuDiskCore.h"
#include "NabuStorageApi.h"
#include "NabuSystemDisk.h"

// The routing engine is small and deliberately IOKit-free.  Compiling it into
// this translation unit keeps the drop-in swap to a single object: the prelink
// list replaces NabuDataBlock.o with NabuSystemDisk.o and nothing else moves.
#include "NabuDiskCore.cpp"

class NabuSystemDisk : public IOBlockStorageDevice {
    OSDeclareDefaultStructors(NabuSystemDisk)

    static constexpr unsigned kTransferBytes = 128 * 1024;
    IOLock *gate = nullptr;
    uint8_t *bounce = nullptr;
    // QEMU passes the Data image in RAM; on hardware everything goes through
    // the transport and this stays null.
    uint8_t *memory = nullptr;
    uint64_t bytes = 0;
    nabu::Disk disk = {};
    bool logical = false;
    uint64_t writeCalls = 0, writtenBytes = 0, refusedCalls = 0;

    static int transportRead(void *, unsigned window, uint64_t offset, void *out, size_t bytes)
    {
        return nabu_storage_read(window, offset, out, bytes);
    }

    static int transportWrite(void *, unsigned window, uint64_t offset, const void *in,
                              size_t bytes)
    {
        if (!nabu_storage_writable(window)) return EROFS;
        return nabu_storage_write(window, offset, in, bytes);
    }

public:
    bool start(IOService *provider) override;
    void free() override;
    IOReturn doAsyncReadWrite(IOMemoryDescriptor *, UInt64, UInt64,
        IOStorageAttributes *, IOStorageCompletion *) override;
    IOReturn doSynchronizeCache() override;
    IOReturn doSynchronize(UInt64, UInt64, IOStorageSynchronizeOptions) override
        { return doSynchronizeCache(); }

    IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
    IOReturn doFormatMedia(UInt64) override { return kIOReturnUnsupported; }
    UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
    char *getVendorString() override { return const_cast<char *>("UniXNU"); }
    char *getProductString() override
        { return const_cast<char *>(logical ? "Nabu System Disk" : "APFS Data"); }
    char *getRevisionString() override { return const_cast<char *>("1.0"); }
    char *getAdditionalDeviceInfoString() override {
        return const_cast<char *>(memory ? "QEMU Data image"
                                         : "60 GiB nabu_system, synthetic GPT");
    }
    IOReturn reportBlockSize(UInt64 *value) override
        { *value = nabu::kLogicalSector; return kIOReturnSuccess; }
    IOReturn reportMaxValidBlock(UInt64 *value) override
        { *value = bytes / nabu::kLogicalSector - 1; return kIOReturnSuccess; }
    IOReturn reportEjectability(bool *value) override
        { *value = false; return kIOReturnSuccess; }
    IOReturn reportRemovability(bool *value) override
        { *value = false; return kIOReturnSuccess; }
    // The disk as a whole is writable; the per-window write policy lives in
    // the transport, so OS/SystemOS stay read-only but Data can be mounted rw.
    IOReturn reportWriteProtection(bool *value) override
        { *value = false; return kIOReturnSuccess; }
    IOReturn reportMediaState(bool *present, bool *changed) override {
        *present = true;
        if (changed) *changed = false;
        return kIOReturnSuccess;
    }
    IOReturn getWriteCacheState(bool *value) override
        { *value = false; return kIOReturnSuccess; }
    IOReturn setWriteCacheState(bool enabled) override
        { return enabled ? kIOReturnUnsupported : kIOReturnSuccess; }
};

OSDefineMetaClassAndStructors(NabuSystemDisk, IOBlockStorageDevice)

bool NabuSystemDisk::start(IOService *provider)
{
    if (!IOBlockStorageDevice::start(provider)) return false;

    // The QEMU loader owns this RAM allocation for the whole boot.  There the
    // image *is* the backing store, so publish it alone, exactly as before.
    IORegistryEntry *map = IORegistryEntry::fromPath("/chosen/memory-map", gIODTPlane);
    if (map) {
        OSData *range = OSDynamicCast(OSData, map->getProperty("NabuDataDisk"));
        if (range) {
            uint64_t physical;
            memcpy(&physical, range->getBytesNoCopy(), sizeof(physical));
            memcpy(&bytes, static_cast<const uint8_t *>(range->getBytesNoCopy()) + 8,
                   sizeof(bytes));
            memory = reinterpret_cast<uint8_t *>(ml_static_ptovirt(physical));
        }
        map->release();
    }

    if (memory) {
        logical = false;
    } else {
        // `nabu-system-disk=0` keeps the old window-only behaviour for A/B.
        uint32_t enabled = 1;
        PE_parse_boot_argn("nabu-system-disk", &enabled, sizeof(enabled));
        if (enabled != 1) {
            IOLog("NABU SYSTEM DISK: disabled by boot arg\n");
            return false;
        }
        // nabu_storage_prepare() may already have run for the OS/SystemOS
        // validation pass; in that case only insist that the transport is live.
        if (nabu_storage_prepare() && !nabu_storage_size(2)) {
            IOLog("NABU SYSTEM DISK: storage layering refused\n");
            return false;
        }
        // Refuse to invent a geometry the transport does not actually have.
        for (unsigned i = 0; i < 3; ++i) {
            const uint64_t size = nabu_storage_size(nabu::kWindows[i].index);
            if (size != nabu::kWindows[i].bytes) {
                IOLog("NABU SYSTEM DISK: window %u is %llu bytes, expected %llu\n", i,
                      static_cast<unsigned long long>(size),
                      static_cast<unsigned long long>(nabu::kWindows[i].bytes));
                return false;
            }
        }
        logical = true;
        bytes = nabu::kPartitionBytes;
    }

    bounce = static_cast<uint8_t *>(IOMalloc(kTransferBytes));
    if (!bounce) return false;
    gate = IOLockAlloc();
    if (!gate) return false;

    if (logical) {
        nabu::Backend backend = {this, transportRead, transportWrite};
        if (!disk.init(backend)) return false;
    }

    OSDictionary *protocol = OSDictionary::withCapacity(2);
    OSString *type = OSString::withCString(kIOPropertyPhysicalInterconnectTypeVirtual);
    OSString *location = OSString::withCString(
        memory ? kIOPropertyInterconnectRAMKey : kIOPropertyInternalKey);
    protocol->setObject(kIOPropertyPhysicalInterconnectTypeKey, type);
    protocol->setObject(kIOPropertyPhysicalInterconnectLocationKey, location);
    setProperty(kIOPropertyProtocolCharacteristicsKey, protocol);
    type->release(); location->release(); protocol->release();
    setName("NabuDataBlock");

    if (!logical) {
        IOLog("NABU DATA BLOCK: QEMU RAM bytes=%llu logical-block=%u\n",
              static_cast<unsigned long long>(bytes), nabu::kLogicalSector);
    } else {
        IOLog("NABU SYSTEM DISK: %llu bytes, %llu LBAs of %u, GPT %u bytes\n",
              static_cast<unsigned long long>(bytes),
              static_cast<unsigned long long>(nabu::kTotalLba), nabu::kLogicalSector,
              static_cast<unsigned>(nabu::kMetaBytes));
        for (unsigned i = 0; i < 4; ++i)
            IOLog("NABU SYSTEM DISK: %-12s LBA %10llu..%10llu %s\n", nabu::kRegions[i].name,
                  static_cast<unsigned long long>(nabu::kRegions[i].offset /
                                                  nabu::kLogicalSector),
                  static_cast<unsigned long long>((nabu::kRegions[i].offset +
                      nabu::kRegions[i].bytes) / nabu::kLogicalSector - 1),
                  i == 2 ? "rw" : "ro");
    }
    registerService();
    return true;
}

IOReturn NabuSystemDisk::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block,
    UInt64 count, IOStorageAttributes *, IOStorageCompletion *completion)
{
    if (block > bytes / nabu::kLogicalSector ||
        count > bytes / nabu::kLogicalSector - block)
        return kIOReturnBadArgument;
    const uint64_t length = count * nabu::kLogicalSector;
    const uint64_t offset = block * nabu::kLogicalSector;
    const bool write = buffer->getDirection() == kIODirectionOut;

    uint64_t done = 0;
    IOReturn status = kIOReturnSuccess;
    IOLockLock(gate);
    if (!logical) {
        // QEMU RAM image: one straight copy, no routing.
        done = write ? buffer->readBytes(0, memory + offset, static_cast<IOByteCount>(length))
                     : buffer->writeBytes(0, memory + offset, static_cast<IOByteCount>(length));
        if (done != length) status = kIOReturnIOError;
    } else {
        while (done < length) {
            const uint64_t take = length - done < kTransferBytes ? length - done : kTransferBytes;
            int error;
            if (write) {
                if (buffer->readBytes(done, bounce, static_cast<IOByteCount>(take)) != take) {
                    error = EFAULT;
                } else {
                    error = disk.write(offset + done, bounce, static_cast<size_t>(take));
                }
            } else {
                error = disk.read(offset + done, bounce, static_cast<size_t>(take));
                if (!error && buffer->writeBytes(done, bounce, static_cast<IOByteCount>(take)) != take)
                    error = EFAULT;
            }
            if (error) { status = kIOReturnIOError; break; }
            done += take;
        }
    }
    if (write) {
        ++writeCalls;
        writtenBytes += done;
        if (status) ++refusedCalls;
        if (writeCalls <= 3 || status)
            IOLog("NABU SYSTEM IO: write offset=%llu bytes=%llu completed=%llu status=%#x\n",
                  offset, length, done, status);
    }
    IOLockUnlock(gate);
    completion->action(completion->target, completion->parameter, status, done);
    return kIOReturnSuccess;
}

IOReturn NabuSystemDisk::doSynchronizeCache()
{
    IOLockLock(gate);
    const int error = logical ? nabu_storage_sync(2) : 0;
    IOLog("NABU SYSTEM IO: sync writes=%llu bytes=%llu refused=%llu errno=%d\n",
          writeCalls, writtenBytes, refusedCalls, error);
    IOLockUnlock(gate);
    return error ? kIOReturnIOError : kIOReturnSuccess;
}

void NabuSystemDisk::free()
{
    if (bounce) IOFree(bounce, kTransferBytes);
    if (gate) IOLockFree(gate);
    IOBlockStorageDevice::free();
}

#if defined(NABU_HOST_BUILD)
// The Darwin kext toolchain emits no C++ atexit registration for this
// translation unit.  Host clang does, so define the two inert symbols here --
// only under an explicit host-build macro -- to keep the prelink graph closed
// while validating the object swap on Linux.  Never defined for the real kext.
extern "C" {
void *__dso_handle = nullptr;
int __cxa_atexit(void (*)(void *), void *, void *) { return 0; }
}
#endif

// Same symbol NabuDataKmod.cpp calls, so this object drops into the existing
// kext without touching the kmod or the bundle id.
extern "C" int nabu_data_block_start(void)
{
    NabuSystemDisk *device = new NabuSystemDisk;
    if (!device) return ENOMEM;
    if (!device->init(nullptr)) { device->release(); return ENOMEM; }
    IOService *provider = IOService::getServiceRoot();
    if (!device->attach(provider)) { device->release(); return ENXIO; }
    if (!device->start(provider)) {
        device->detach(provider);
        device->release();
        return EIO;
    }
    device->release();
    return 0;
}
