/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned char kernel_heap[16u * 1024u * 1024u];
static size_t kernel_size, kernel_limit = sizeof(kernel_heap);
static unsigned kernel_calls;
static bool fail_address;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
char *fake_heap_start, *fake_heap_end;
static void check(bool ok, const char *message) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}
void mutexLock(Mutex *mutex) {
    (void)mutex;
    check(!pthread_mutex_lock(&lock), "Lock failed");
}
void mutexUnlock(Mutex *mutex) {
    (void)mutex;
    check(!pthread_mutex_unlock(&lock), "Unlock failed");
}
Result svcSetHeapSize(void **address, size_t size) {
    ++kernel_calls;
    check(size % (2u * 1024u * 1024u) == 0, "Kernel heap alignment");
    check(size >= kernel_size, "Must not unmap borrowed worker/transfer pages");
    if (size > kernel_limit || fail_address)
        return 0x10801; // The physical report's LimitReached.
    kernel_size = size;
    *address = kernel_heap;
    return 0;
}
#include "../background/source/heap.c"

static void *allocate_thread(void *arg) {
    void **slots = arg;
    struct _reent reent = {0};
    for (unsigned i = 0; i < 256; ++i) {
        slots[i] = _sbrk_r(&reent, 4096);
        check(slots[i] != (void *)-1, "Concurrent growth failed");
        memset(slots[i], 0x5a, 4096);
    }
    return NULL;
}
static int compare(const void *a, const void *b) {
    const uintptr_t x = (uintptr_t) * (void *const *)a, y = (uintptr_t) * (void *const *)b;
    return (x > y) - (x < y);
}
static void scenario(unsigned which) {
    struct _reent reent = {0};
    if (which == 1)
        fail_address = true;
    __libnx_initheap();
    check(kernel_calls == 1, "Boot reserves more than one initial chunk");
    if (which == 1) {
        check(ncmHeapStartupResult() == 0x10801, "Initial kernel error lost");
        check(ncmHeapCommittedSize() == 256u * 1024u, "Emergency budget changed");
        check(_sbrk_r(&reent, 4096) != (void *)-1, "Emergency logging heap missing");
        check(_sbrk_r(&reent, 256u * 1024u) == (void *)-1 && reent._errno == ENOMEM,
              "Emergency path tried to run the player");
        check(kernel_calls == 1, "Emergency heap tried further allocation");
        return;
    }
    check(ncmHeapStartupResult() == 0 && ncmHeapCommittedSize() == 0 && kernel_size == 0,
          "Boot must not commit a playback heap before HOME");
    check(fake_heap_end == fake_heap_start, "Initial heap bounds");
    if (which == 2) {
        pthread_t workers[8];
        void *slots[8 * 256];
        for (unsigned i = 0; i < 8; ++i)
            check(!pthread_create(&workers[i], NULL, allocate_thread, &slots[i * 256]),
                  "Thread failed");
        for (unsigned i = 0; i < 8; ++i)
            pthread_join(workers[i], NULL);
        qsort(slots, 8 * 256, sizeof(void *), compare);
        for (unsigned i = 0; i < 8 * 256; ++i)
            check(slots[i] == kernel_heap + (size_t)i * 4096, "Concurrent break overlap/gap");
        check(ncmHeapCommittedSize() == 8u * 1024u * 1024u, "Concurrent budget");
        return;
    }
    check(_sbrk_r(&reent, HEAP_STEP - 16) == kernel_heap, "Initial allocation address");
    memset(kernel_heap, 0xa5, HEAP_STEP - 16);
    kernel_limit = HEAP_STEP;
    check(_sbrk_r(&reent, 32) == (void *)-1 && reent._errno == ENOMEM,
          "Quota failure must return ENOMEM, not abort");
    check(ncmHeapLastResult() == 0x10801 && _sbrk_r(&reent, 0) == kernel_heap + HEAP_STEP - 16,
          "Failed growth changed the break or lost the native result");
    check(ncmHeapCommittedSize() == HEAP_STEP && kernel_heap[HEAP_STEP - 17] == 0xa5,
          "Failed growth damaged committed memory");
    kernel_limit = sizeof(kernel_heap);
    check(_sbrk_r(&reent, 32) == kernel_heap + HEAP_STEP - 16, "Retry failed");
    check(ncmHeapCommittedSize() == 2 * HEAP_STEP, "Growth must round to 2 MiB");
    check(_sbrk_r(&reent, -32) == kernel_heap + HEAP_STEP + 16, "Logical shrink failed");
    const unsigned calls = kernel_calls;
    check(_sbrk_r(&reent, 32) != (void *)-1 && kernel_calls == calls, "Reuse remapped memory");
    check(_sbrk_r(&reent, PTRDIFF_MIN) == (void *)-1, "Negative overflow accepted");
    check(_sbrk_r(&reent, PTRDIFF_MAX) == (void *)-1, "Positive overflow accepted");
    const size_t remaining = HEAP_LIMIT - (HEAP_STEP + 16);
    check(_sbrk_r(&reent, (ptrdiff_t)remaining) != (void *)-1, "Original peak budget lost");
    check(_sbrk_r(&reent, 1) == (void *)-1 && kernel_size == HEAP_LIMIT,
          "Playback heap exceeded the 16 MiB cap");
}
int main(void) {
    for (unsigned i = 0; i < 3; ++i) {
        pid_t child = fork();
        check(child >= 0, "fork failed");
        if (!child) {
            scenario(i);
            _exit(0);
        }
        int status;
        check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "Heap scenario aborted or failed");
    }
    puts("Heap: incremental growth, kernel quota denial, recovery, emergency logging, bounds and "
         "concurrency passed");
    return 0;
}
