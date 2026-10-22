/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "resources.h"
#include "heap.h"
#include "startup.h"
#include <stdio.h>

static NcmMemoryBudget memory_budget = NcmMemoryBudget_Normal;
static bool budget_committed;

size_t ncmMemoryHeapLimit(void) {
    return memory_budget == NcmMemoryBudget_Compact ? NCM_COMPACT_HEAP_LIMIT
                                                    : NCM_NORMAL_HEAP_LIMIT;
}

size_t ncmSystemMemoryMargin(void) {
    return memory_budget == NcmMemoryBudget_Compact ? NCM_COMPACT_MEMORY_MARGIN
                                                    : NCM_SYSTEM_MEMORY_MARGIN;
}

Result ncmCommitStartupBudget(NcmMemoryBudget candidate) {
    if (budget_committed || ncmHeapCommittedSize() != 0 ||
        (candidate != NcmMemoryBudget_Normal && candidate != NcmMemoryBudget_Compact))
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    memory_budget = candidate;
    budget_committed = true;
    ncmStartupWrite(candidate == NcmMemoryBudget_Compact ? "memory-policy: compact\n"
                                                         : "memory-policy: normal\n");
    ncmStartupRecord("memory-reserve-bytes", (Result)ncmSystemMemoryMargin());
    ncmStartupRecord("heap-limit-bytes", (Result)ncmMemoryHeapLimit());
    return 0;
}

Result ncmReadResources(NcmResourceSnapshot *snapshot) {
    u64 value = 0;
    Result result = svcGetInfo(&value, InfoType_ResourceLimit, INVALID_HANDLE, 0);
    if (R_FAILED(result))
        return result;
    if (value == INVALID_HANDLE)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    const Handle handle = (Handle)value;
    for (unsigned i = 0; i < 5; ++i) {
        result = svcGetResourceLimitCurrentValue(&snapshot->used[i], handle, (LimitableResource)i);
        if (R_SUCCEEDED(result))
            result =
                svcGetResourceLimitLimitValue(&snapshot->limit[i], handle, (LimitableResource)i);
        if (R_FAILED(result))
            break;
        if (snapshot->used[i] < 0 || snapshot->limit[i] < snapshot->used[i]) {
            result = MAKERESULT(Module_Libnx, LibnxError_BadInput);
            break;
        }
    }
    svcCloseHandle(handle);
    return result;
}

static Result check_margin(const NcmResourceSnapshot *snapshot, const s64 margin[5]) {
    for (unsigned i = 0; i < 5; ++i)
        if (snapshot->limit[i] - snapshot->used[i] < margin[i])
            return MAKERESULT(Module_Kernel, 132); /* LimitReached */
    return 0;
}

Result ncmCheckMemoryGrowth(size_t additional_bytes) {
    NcmResourceSnapshot snapshot;
    Result result = ncmReadResources(&snapshot);
    if (R_FAILED(result))
        return result;
    const u64 free_bytes = (u64)(snapshot.limit[0] - snapshot.used[0]);
    const size_t margin = ncmSystemMemoryMargin();
    if (free_bytes < margin || additional_bytes > free_bytes - margin)
        return MAKERESULT(Module_Kernel, 132);
    return 0;
}

Result ncmCheckStartupResources(NcmMemoryBudget *candidate) {
    static const s64 margin[5] = {NCM_COMPACT_MEMORY_MARGIN + NCM_COMPACT_HEAP_LIMIT, 16, 16, 4,
                                  16};
    if (!candidate)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    NcmResourceSnapshot snapshot;
    Result result = ncmReadResources(&snapshot);
    if (R_SUCCEEDED(result))
        result = check_margin(&snapshot, margin);
    if (R_SUCCEEDED(result))
        *candidate = snapshot.limit[0] - snapshot.used[0] >=
                             NCM_SYSTEM_MEMORY_MARGIN + NCM_STARTUP_MEMORY_ALLOWANCE
                         ? NcmMemoryBudget_Normal
                         : NcmMemoryBudget_Compact;
    return result;
}

bool ncmCanStartWork(void) {
    const s64 margin[5] = {(s64)ncmSystemMemoryMargin(), 8, 8, 2, 8};
    NcmResourceSnapshot snapshot;
    return R_SUCCEEDED(ncmReadResources(&snapshot)) && R_SUCCEEDED(check_margin(&snapshot, margin));
}

void ncmStartupRecordResources(const char *stage) {
    ncmStartupRecord(stage, 0);
    NcmResourceSnapshot snapshot;
    Result result = ncmReadResources(&snapshot);
    if (R_FAILED(result)) {
        ncmStartupRecord("resource-query", result);
        return;
    }
    static const char *names[] = {"memory", "threads", "events", "transfer-memory", "sessions"};
    for (unsigned i = 0; i < 5; ++i) {
        char line[160];
        snprintf(line, sizeof(line), "resource %s: used=%lld limit=%lld free=%lld\n", names[i],
                 (long long)snapshot.used[i], (long long)snapshot.limit[i],
                 (long long)(snapshot.limit[i] - snapshot.used[i]));
        ncmStartupWrite(line);
    }
    ncmStartupRecord("heap-committed-bytes", (Result)ncmHeapCommittedSize());
}
