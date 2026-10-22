/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
static s64 used[5], limits[5] = {64 * 1024 * 1024, 100, 100, 100, 100};
static unsigned handles;
static size_t committed;
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
    return committed;
}
#include "../background/source/resources.c"
int main(void) {
    NcmMemoryBudget candidate = NcmMemoryBudget_Normal;
    check(ncmCanStartWork() && !ncmCheckStartupResources(&candidate), "Normal resources rejected");
    const size_t boundary = (size_t)limits[0] - NCM_SYSTEM_MEMORY_MARGIN;
    used[0] = limits[0] - NCM_SYSTEM_MEMORY_MARGIN - NCM_STARTUP_MEMORY_ALLOWANCE;
    check(!ncmCheckStartupResources(&candidate), "Measured startup allowance rejected");
    check(candidate == NcmMemoryBudget_Normal, "Normal budget not selected");
    ++used[0];
    check(!ncmCheckStartupResources(&candidate) && candidate == NcmMemoryBudget_Compact,
          "Intermediate headroom did not select compact budget");
    used[0] = limits[0] - 5u * 1024u * 1024u;
    check(!ncmCheckStartupResources(&candidate) && candidate == NcmMemoryBudget_Compact,
          "Exact compact boundary rejected");
    ++used[0];
    check(ncmCheckStartupResources(&candidate), "Compact boundary bypassed");
    const s64 physical_free[] = {5431296, 5668864};
    for (unsigned i = 0; i < 2; ++i) {
        used[0] = limits[0] - physical_free[i];
        check(!ncmCheckStartupResources(&candidate) && candidate == NcmMemoryBudget_Compact,
              "Physical loader coexistence rejected");
    }
    check(ncmMemoryHeapLimit() == NCM_NORMAL_HEAP_LIMIT &&
              ncmSystemMemoryMargin() == NCM_SYSTEM_MEMORY_MARGIN,
          "Read-only probe changed live policy");
    used[0] = 0;
    check(!ncmCheckMemoryGrowth(boundary), "Exact memory margin rejected");
    check(ncmCheckMemoryGrowth(boundary + 1) && ncmCheckMemoryGrowth(SIZE_MAX),
          "Margin or integer overflow bypassed");
    for (unsigned i = 0; i < 5; ++i) {
        used[i] = limits[i];
        check(ncmCheckStartupResources(&candidate) && !ncmCanStartWork(),
              "Exhausted resource accepted");
        used[i] = -1;
        check(ncmCheckStartupResources(&candidate), "Negative resource value accepted");
        used[i] = limits[i] + 1;
        check(ncmCheckStartupResources(&candidate), "Underflow accepted");
        used[i] = 0;
    }
    for (fail_stage = 1; fail_stage <= 4; ++fail_stage) {
        check(ncmCheckStartupResources(&candidate) && ncmCheckMemoryGrowth(1) && !ncmCanStartWork(),
              "Failed/invalid query did not fail closed");
        check(!handles, "Failed query leaked handle");
    }
    fail_stage = 0;
    check(!ncmCheckStartupResources(&candidate) && !handles, "Recovery failed");
    check(ncmCheckStartupResources(NULL), "Null candidate accepted");
    check(ncmCommitStartupBudget((NcmMemoryBudget)99), "Invalid policy accepted");
    committed = 2u * 1024u * 1024u;
    check(ncmCommitStartupBudget(NcmMemoryBudget_Compact), "Policy changed after heap allocation");
    committed = 0;
    check(!ncmCommitStartupBudget(NcmMemoryBudget_Compact), "Compact policy not committed");
    check(ncmCommitStartupBudget(NcmMemoryBudget_Normal), "Frozen policy changed");
    check(ncmMemoryHeapLimit() == 2u * 1024u * 1024u &&
              ncmSystemMemoryMargin() == 3u * 1024u * 1024u,
          "Compact heap/reserve mismatch");
    used[0] = limits[0] - 5431296;
    check(!ncmCheckMemoryGrowth(2u * 1024u * 1024u), "Physical initial heap denied");
    used[0] += 2u * 1024u * 1024u;
    check(ncmCanStartWork(), "Physical post-startup work denied");
    used[0] = limits[0] - NCM_COMPACT_MEMORY_MARGIN;
    check(ncmCanStartWork() && !ncmCheckMemoryGrowth(0), "Exact compact reserve rejected");
    check(ncmCheckMemoryGrowth(1) && ncmCheckMemoryGrowth(SIZE_MAX), "Compact reserve bypassed");
    ++used[0];
    check(!ncmCanStartWork() && ncmCheckMemoryGrowth(0), "Shrinking headroom ignored");
    puts("PASS: all five resource guards, margin boundaries, overflow, query failure and recovery");
}
