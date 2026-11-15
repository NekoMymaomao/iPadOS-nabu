// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
// Drop-in replacement for NabuDataBlock.h.  Same symbol the kmod calls and the
// same bundle id NabuNativeApfs.c loads, but the published device is the whole
// 60 GiB nabu_system with a synthetic GPT instead of the bare Data window.
int nabu_data_block_start(void);
#ifdef __cplusplus
}
#endif
