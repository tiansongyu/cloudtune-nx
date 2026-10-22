/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "resources.h"
#include "heap.h"
#include "startup.h"
#include <stdio.h>

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
    if (free_bytes < NCM_SYSTEM_MEMORY_MARGIN ||
        additional_bytes > free_bytes - NCM_SYSTEM_MEMORY_MARGIN)
        return MAKERESULT(Module_Kernel, 132);
    return 0;
}

Result ncmCheckStartupResources(void) {
    /* Initial heap/socket/worker allowance in addition to the 8 MiB margin. */
    static const s64 margin[5] = {NCM_SYSTEM_MEMORY_MARGIN + 4u * 1024u * 1024u, 16, 16, 4, 16};
    NcmResourceSnapshot snapshot;
    Result result = ncmReadResources(&snapshot);
    return R_FAILED(result) ? result : check_margin(&snapshot, margin);
}

bool ncmCanStartWork(void) {
    static const s64 margin[5] = {NCM_SYSTEM_MEMORY_MARGIN, 8, 8, 2, 8};
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
