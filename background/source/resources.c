/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "heap.h"
#include "startup.h"
#include <stdio.h>

void ncmStartupRecordResources(const char *stage) {
    ncmStartupRecord(stage, 0);
    u64 value = 0;
    Result result = svcGetInfo(&value, InfoType_ResourceLimit, INVALID_HANDLE, 0);
    if (R_FAILED(result) || value == INVALID_HANDLE) {
        ncmStartupRecord("resource-handle", result);
        return;
    }
    const Handle handle = (Handle)value;
    static const char *names[] = {"memory", "threads", "events", "transfer-memory", "sessions"};
    for (unsigned i = 0; i < 5; ++i) {
        s64 used = 0, limit = 0;
        result = svcGetResourceLimitCurrentValue(&used, handle, (LimitableResource)i);
        if (R_SUCCEEDED(result))
            result = svcGetResourceLimitLimitValue(&limit, handle, (LimitableResource)i);
        if (R_FAILED(result)) {
            ncmStartupRecord(names[i], result);
            continue;
        }
        char line[160];
        snprintf(line, sizeof(line), "resource %s: used=%lld limit=%lld free=%lld\n", names[i],
                 (long long)used, (long long)limit, (long long)(limit - used));
        ncmStartupWrite(line);
    }
    svcCloseHandle(handle);
    ncmStartupRecord("heap-committed-bytes", (Result)ncmHeapCommittedSize());
}
