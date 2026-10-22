/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
static s64 used[5], limits[5] = {64 * 1024 * 1024, 100, 100, 100, 100};
static unsigned handles;
static int fail_stage;
static void check(bool ok, const char *message) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}
Result svcGetInfo(u64 *out, u32 type, Handle handle, u64 sub) {
    check(type == InfoType_ResourceLimit && !handle && !sub, "Wrong query");
    if (fail_stage == 1)
        return 0x1234;
    *out = fail_stage == 4 ? 0 : 42;
    if (*out)
        ++handles;
    return 0;
}
Result svcGetResourceLimitCurrentValue(s64 *out, Handle h, LimitableResource r) {
    check(h == 42 && handles == 1, "Invalid handle");
    *out = used[r];
    return fail_stage == 2 ? 0x1234 : 0;
}
Result svcGetResourceLimitLimitValue(s64 *out, Handle h, LimitableResource r) {
    check(h == 42 && handles == 1, "Invalid handle");
    *out = limits[r];
    return fail_stage == 3 ? 0x1234 : 0;
}
Result svcCloseHandle(Handle h) {
    check(h == 42 && handles == 1, "Double close");
    --handles;
    return 0;
}
void ncmStartupRecord(const char *s, Result r) {
    (void)s;
    (void)r;
}
void ncmStartupWrite(const char *s) {
    (void)s;
}
size_t ncmHeapCommittedSize(void) {
    return 0;
}
#include "../background/source/resources.c"
int main(void) {
    check(ncmCanStartWork() && !ncmCheckStartupResources(), "Normal resources rejected");
    const size_t boundary = (size_t)limits[0] - NCM_SYSTEM_MEMORY_MARGIN;
    used[0] = limits[0] - NCM_SYSTEM_MEMORY_MARGIN - NCM_STARTUP_MEMORY_ALLOWANCE;
    check(!ncmCheckStartupResources(), "Measured startup allowance rejected");
    ++used[0];
    check(ncmCheckStartupResources(), "Startup boundary bypassed");
    used[0] = limits[0] - 5u * 1024u * 1024u;
    check(ncmCheckStartupResources() && ncmCheckMemoryGrowth(2u * 1024u * 1024u),
          "Physical 5 MiB shortage bypassed the unchanged 8 MiB reserve");
    used[0] = 0;
    check(!ncmCheckMemoryGrowth(boundary), "Exact memory margin rejected");
    check(ncmCheckMemoryGrowth(boundary + 1) && ncmCheckMemoryGrowth(SIZE_MAX),
          "Margin or integer overflow bypassed");
    for (unsigned i = 0; i < 5; ++i) {
        used[i] = limits[i];
        check(ncmCheckStartupResources() && !ncmCanStartWork(), "Exhausted resource accepted");
        used[i] = -1;
        check(ncmCheckStartupResources(), "Negative resource value accepted");
        used[i] = limits[i] + 1;
        check(ncmCheckStartupResources(), "Underflow accepted");
        used[i] = 0;
    }
    for (fail_stage = 1; fail_stage <= 4; ++fail_stage) {
        check(ncmCheckStartupResources() && ncmCheckMemoryGrowth(1) && !ncmCanStartWork(),
              "Failed/invalid query did not fail closed");
        check(!handles, "Failed query leaked handle");
    }
    fail_stage = 0;
    check(!ncmCheckStartupResources() && !handles, "Recovery failed");
    puts("PASS: all five resource guards, margin boundaries, overflow, query failure and recovery");
}
