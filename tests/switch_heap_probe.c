/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Optional native-Horizon regression probe. Run its ExeFS in an isolated
 * emulator profile, never install it as the production background service. */
#include "heap.h"
#include "startup.h"
#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t quota = 16u * 1024u * 1024u;
static unsigned home_queries;
static bool zero_heap_at_boot = true;
/* A standalone emulator has no HOME process or Atmosphere launch record.
 * Only this excluded probe simulates that boundary; the shipped NSP uses PM. */
Result __wrap_ncmBootHasHomeMenu(bool *launched) {
    zero_heap_at_boot = zero_heap_at_boot && ncmHeapCommittedSize() == 0;
    *launched = ++home_queries > 3;
    return 0;
}
Result __real_svcSetHeapSize(void **address, size_t size);
Result __wrap_svcSetHeapSize(void **address, size_t size) {
    return size > quota ? MAKERESULT(Module_Kernel, 132) : __real_svcSetHeapSize(address, size);
}
int main(void) {
    if (R_FAILED(ncmStartupResult()))
        return 1;
    FILE *log = fopen("sdmc:/config/netease-switch/heap-probe.log", "w");
    if (!log)
        return 2;
    int result = 1;
    unsigned char *a = NULL, *b = NULL, *aligned = NULL;
#define CHECK(condition, message)                                                                  \
    do {                                                                                           \
        const bool ok = (condition);                                                               \
        fprintf(log, "%s: %s (committed=%zu last=%08x)\n", ok ? "PASS" : "FAIL", message,          \
                ncmHeapCommittedSize(), ncmHeapLastResult());                                      \
        fflush(log);                                                                               \
        if (!ok)                                                                                   \
            goto done;                                                                             \
    } while (0)
    CHECK(zero_heap_at_boot && home_queries == 4,
          "zero committed heap while waiting for simulated HOME");
    CHECK(ncmHeapCommittedSize() == 2u * 1024u * 1024u, "first reservation after HOME");
    a = malloc(3u * 1024u * 1024u);
    CHECK(a != NULL, "newlib malloc grows across a kernel heap boundary");
    memset(a, 0x5a, 3u * 1024u * 1024u);
    quota = ncmHeapCommittedSize();
    b = malloc(4u * 1024u * 1024u);
    CHECK(b == NULL && errno == ENOMEM, "kernel LimitReached becomes ENOMEM");
    CHECK(ncmHeapLastResult() == 0x10801 && a[0] == 0x5a && a[3u * 1024u * 1024u - 1] == 0x5a,
          "denied growth preserves existing allocations");
    quota = 16u * 1024u * 1024u;
    b = malloc(4u * 1024u * 1024u);
    CHECK(b != NULL, "newlib retries successfully after quota recovery");
    memset(b, 0xa5, 4u * 1024u * 1024u);
    aligned = memalign(0x1000, 0x20000);
    CHECK(aligned != NULL && ((uintptr_t)aligned & 0xfff) == 0, "worker/transfer buffer alignment");
    CHECK(ncmHeapCommittedSize() <= 16u * 1024u * 1024u, "original maximum budget");
    result = 0;
done:
    free(aligned);
    free(b);
    free(a);
    fprintf(log, "RESULT: %s\n", result ? "FAIL" : "PASS");
    fclose(log);
    return result;
}
