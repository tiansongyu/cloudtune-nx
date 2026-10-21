/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "heap.h"
#include <errno.h>
#include <stdint.h>
#include <sys/reent.h>

#define HEAP_STEP (2u * 1024u * 1024u)
#define HEAP_LIMIT (16u * 1024u * 1024u)

/* Do not put the playback heap in BSS. Atmosphere boot2 aborts if loading a
 * flagged process exceeds the shared system resource limit, before main runs.
 * This small fallback lets a failed initial heap request reach startup logging
 * and an ordinary exit. It is never used for the playback runtime. */
static _Alignas(16) unsigned char emergency_heap[256u * 1024u];
static unsigned char *heap_base;
static size_t heap_break, heap_committed;
static bool emergency;
static Result initial_result, last_result;
static Mutex heap_mutex;

void __libnx_initheap(void) {
    void *base = NULL;
    /* Reserve the address, not physical pages. The allocation-free boot gate
     * runs before the first sbrk. Even a successful early 2 MiB allocation can
     * consume resources AM still needs while starting HOME. */
    initial_result = last_result = svcSetHeapSize(&base, 0);
    emergency = R_FAILED(initial_result);
    heap_base = emergency ? emergency_heap : base;
    heap_committed = emergency ? sizeof(emergency_heap) : 0;
    extern char *fake_heap_start, *fake_heap_end;
    fake_heap_start = (char *)heap_base;
    fake_heap_end = (char *)heap_base + heap_committed;
}

/* Override libsysbase's weak fixed-region sbrk. Growth is page-granular and
 * bounded by the same 16 MiB playback budget as before. Never allocate, log or
 * call another malloc user here. A rejected kernel request must leave the
 * current break and all existing allocations intact. */
void *_sbrk_r(struct _reent *reent, ptrdiff_t increment) {
    mutexLock(&heap_mutex);
    size_t next;
    if (increment < 0) {
        const size_t reduction = (size_t)(-(increment + 1)) + 1;
        if (reduction > heap_break)
            goto exhausted;
        next = heap_break - reduction;
    } else {
        const size_t limit = emergency ? sizeof(emergency_heap) : HEAP_LIMIT;
        if ((size_t)increment > limit - heap_break)
            goto exhausted;
        next = heap_break + (size_t)increment;
    }
    if (next > heap_committed) {
        const size_t size = (next + HEAP_STEP - 1) & ~(size_t)(HEAP_STEP - 1);
        void *base = NULL;
        last_result = svcSetHeapSize(&base, size);
        if (R_FAILED(last_result))
            goto exhausted;
        heap_committed = size;
        extern char *fake_heap_end;
        fake_heap_end = (char *)heap_base + heap_committed;
    }
    void *previous = heap_base + heap_break;
    heap_break = next;
    /* Keep committed pages on logical shrink: libnx may have mapped worker
     * stacks/transfer buffers out of this heap. Reuse them on the next growth. */
    mutexUnlock(&heap_mutex);
    return previous;

exhausted:
    reent->_errno = ENOMEM;
    mutexUnlock(&heap_mutex);
    return (void *)-1;
}

Result ncmHeapStartupResult(void) {
    return initial_result;
}
Result ncmHeapLastResult(void) {
    mutexLock(&heap_mutex);
    const Result result = last_result;
    mutexUnlock(&heap_mutex);
    return result;
}
size_t ncmHeapCommittedSize(void) {
    mutexLock(&heap_mutex);
    const size_t size = heap_committed;
    mutexUnlock(&heap_mutex);
    return size;
}
