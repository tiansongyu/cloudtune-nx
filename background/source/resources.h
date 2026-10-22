/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <switch.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Policy margins, not a reservation or a guarantee against other processes.
 * All queries use SVCs only; safe inside sbrk and before newlib initialization. */
#define NCM_SYSTEM_MEMORY_MARGIN (8u * 1024u * 1024u)
typedef struct {
    s64 used[5], limit[5];
} NcmResourceSnapshot;
Result ncmReadResources(NcmResourceSnapshot *snapshot);
Result ncmCheckMemoryGrowth(size_t additional_bytes);
Result ncmCheckStartupResources(void);
bool ncmCanStartWork(void);
#ifdef __cplusplus
}
#endif
