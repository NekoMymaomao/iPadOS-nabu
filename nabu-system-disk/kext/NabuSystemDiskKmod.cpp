// SPDX-License-Identifier: BSD-2-Clause-Patent
// Kext entry point.  Mirrors NabuDataKmod.cpp so the prelink tool sees the
// same shape (one _kmod_info, one start symbol) for this bundle.
//
// Exactly one of NabuDataBlock / NabuSystemDisk must be linked into a given
// kernel: they are two views of the same bytes.  See kext/README.md.
#include <mach/kmod.h>

#include "NabuSystemDisk.h"

extern "C" {
static kern_return_t nabu_system_disk_module_start(kmod_info_t *, void *)
{
    return nabu_system_disk_start() ? KERN_FAILURE : KERN_SUCCESS;
}

static kern_return_t nabu_system_disk_module_stop(kmod_info_t *, void *)
{
    return KERN_FAILURE;
}

KMOD_EXPLICIT_DECL(org.nabu.driver.SystemDisk, "1.0.0",
    nabu_system_disk_module_start, nabu_system_disk_module_stop)
}
