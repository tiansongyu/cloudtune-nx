/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <switch.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Policy margins, not a reservation or a guarantee against other processes.
 * All queries use SVCs only; safe inside sbrk and before newlib initialization. */
#define NCM_SYSTEM_MEMORY_MARGIN (8u * 1024u * 1024u)
#define NCM_NORMAL_HEAP_LIMIT (16u * 1024u * 1024u)
#define NCM_COMPACT_MEMORY_MARGIN (3u * 1024u * 1024u)
#define NCM_COMPACT_HEAP_LIMIT (2u * 1024u * 1024u)
/* Normal startup budgets the measured 2 MiB heap (sockets and runtime
 * workers included) plus 256 KiB. Compact mode budgets its entire hard cap.
 * Growth checks the active margin independently; neither mode reserves it. */
#define NCM_STARTUP_MEMORY_ALLOWANCE (2u * 1024u * 1024u + 256u * 1024u)
typedef struct {
    s64 used[5], limit[5];
} NcmResourceSnapshot;
typedef enum { NcmMemoryBudget_Normal, NcmMemoryBudget_Compact } NcmMemoryBudget;
Result ncmReadResources(NcmResourceSnapshot *snapshot);
Result ncmCheckMemoryGrowth(size_t additional_bytes);
/* Probe without changing policy. Freeze the selected budget once, before the
 * first heap allocation and worker creation; getters do not acquire locks. */
Result ncmCheckStartupResources(NcmMemoryBudget *candidate);
Result ncmCommitStartupBudget(NcmMemoryBudget candidate);
size_t ncmMemoryHeapLimit(void);
size_t ncmSystemMemoryMargin(void);
bool ncmCanStartWork(void);
#ifdef __cplusplus
}
#endif
