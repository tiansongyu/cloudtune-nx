/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <switch.h>

#ifdef __cplusplus
extern "C" {
#endif
Result ncmHeapStartupResult(void);
Result ncmHeapLastResult(void);
size_t ncmHeapCommittedSize(void);
#ifdef __cplusplus
}
#endif
