// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
// Call after the original IOStorageFamily has registered its classes and
// after nabu_storage_prepare() has validated OS/SystemOS.  Publishes the
// whole 60 GiB nabu_system as one virtual disk with a synthetic GPT.
int nabu_system_disk_start(void);
#ifdef __cplusplus
}
#endif
