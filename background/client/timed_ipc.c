/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "timed_ipc.h"
#include <string.h>

enum { PageSize = 256, MaxInline = 192 };
static u8 page[PageSize] __attribute__((aligned(16)));
static Thread worker;
static Mutex work_mutex;
static CondVar request_ready, reply_ready;
static bool worker_started, worker_stop;
static enum { Idle, Pending, Done } state;
static Handle work_session;
static bool close_work_session;

/* Use libnx's ordinary TLS/CMIF transport, as established music sysmodules
 * do. One worker owns the entire request, including after a caller timeout.
 * Never retry a timed-out command or hand caller-owned memory to the worker. */
static void ipcWorker(void* unused) {
    (void)unused;
    mutexLock(&work_mutex);
    for (;;) {
        while (!worker_stop && state != Pending) condvarWait(&request_ready, &work_mutex);
        if (worker_stop) break;
        memcpy(armGetTls(), page, PageSize);
        const Handle session = work_session;
        mutexUnlock(&work_mutex);
        const Result result = svcSendSyncRequest(session);
        mutexLock(&work_mutex);
        if (close_work_session) {
            svcCloseHandle(session);
            close_work_session = false;
        }
        if (R_SUCCEEDED(result)) memcpy(page, armGetTls(), PageSize);
        else {
            memset(page, 0, PageSize);
            memcpy(page + 8, &result, sizeof(result));
        }
        state = Done;
        condvarWakeAll(&reply_ready);
    }
    mutexUnlock(&work_mutex);
}

static Result startWorker(void) {
    if (worker_started) return 0;
    Result result = threadCreate(&worker, ipcWorker, NULL, NULL, 0x4000, 44, -2);
    if (R_FAILED(result)) return result;
    worker_stop = false;
    result = threadStart(&worker);
    if (R_FAILED(result)) { threadClose(&worker); return result; }
    worker_started = true;
    return 0;
}

static Result protocolError(void) { return MAKERESULT(421, 1); }
static Result timeoutError(void) { return MAKERESULT(Module_Libnx, LibnxError_Timeout); }

/* The kernel validates handle descriptors, but data counts and CMIF contents
 * still come from the peer. Validate bounds before exposing any output. */
static Result response(void* output, size_t size, Handle* moved, bool* replied) {
    const u32* words = (const u32*)page;
    if (!words[0] && !words[1])
        return words[2] ? words[2] : protocolError(); /* Worker transport failure. */
    HipcHeader header;
    memcpy(&header, page, sizeof(header));
    HipcParsedRequest parsed = hipcParseRequest(page);
    Result result = protocolError();
    const uintptr_t start = (uintptr_t)parsed.data.data_words;
    const uintptr_t end = (uintptr_t)page + PageSize;
    const uintptr_t aligned = (start + 15) & ~(uintptr_t)15;
    const size_t bytes = (size_t)header.num_data_words * sizeof(u32);
    const size_t padding = aligned - start;
    // libnx/SM replies use type 0; the existing CloudTune server preserves
    // CmifCommandType_Request. The kernel copies this field unchanged.
    if ((header.type == 0 || header.type == CmifCommandType_Request) &&
        !header.num_send_statics && !header.num_send_buffers &&
        !header.num_recv_buffers && !header.num_exch_buffers &&
        !header.recv_static_mode && !parsed.meta.send_pid &&
        start >= (uintptr_t)page && start <= end && bytes <= end - start &&
        bytes >= padding + sizeof(CmifOutHeader)) {
        CmifOutHeader cmif;
        memcpy(&cmif, (void*)aligned, sizeof(cmif));
        const bool success = R_SUCCEEDED(cmif.result);
        const u32 expected_handles = success && moved ? 1u : 0u;
        if (cmif.magic == CMIF_OUT_HEADER_MAGIC && cmif.version == 0 &&
            !parsed.meta.num_copy_handles &&
            parsed.meta.num_move_handles == expected_handles &&
            (!success || bytes >= padding + sizeof(cmif) + size) &&
            (!expected_handles || parsed.data.move_handles[0] != INVALID_HANDLE)) {
            result = cmif.result;
            if (success) {
                if (size) memcpy(output, (void*)(aligned + sizeof(cmif)), size);
                if (moved) {
                    *moved = parsed.data.move_handles[0];
                    parsed.data.move_handles[0] = INVALID_HANDLE;
                }
            }
            if (replied) *replied = true;
        }
    }
    for (u32 i = 0; i < parsed.meta.num_copy_handles; ++i)
        if (parsed.data.copy_handles[i]) svcCloseHandle(parsed.data.copy_handles[i]);
    for (u32 i = 0; i < parsed.meta.num_move_handles; ++i)
        if (parsed.data.move_handles[i]) svcCloseHandle(parsed.data.move_handles[i]);
    return result;
}

bool ncmIpcIdle(void) {
    mutexLock(&work_mutex);
    const bool idle = state != Pending;
    if (state == Done) {
        // Late successful GetService replies still own a handle; discard it.
        response(NULL, 0, NULL, NULL);
        state = Idle;
    }
    mutexUnlock(&work_mutex);
    return idle;
}
void ncmIpcCloseSession(Handle session) {
    if (!session) return;
    mutexLock(&work_mutex);
    /* A descheduled worker may not have entered SendSyncRequest yet. Never
     * let its numeric handle be recycled into an unrelated service. */
    if (state == Pending && work_session == session) close_work_session = true;
    else svcCloseHandle(session);
    mutexUnlock(&work_mutex);
}

Result ncmIpcRequest(Handle session, u32 command, const void* input, size_t input_size,
                    void* output, size_t output_size, Handle* moved_handle,
                    bool send_pid, u64 timeout_ns, bool* replied) {
    if (replied) *replied = false;
    if (!session || input_size > MaxInline || output_size > 216 ||
        (input_size && !input) || (output_size && !output))
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (output_size) memset(output, 0, output_size);
    if (moved_handle) *moved_handle = INVALID_HANDLE;
    if (!ncmIpcIdle()) return timeoutError();
    Result result = startWorker();
    if (R_FAILED(result)) return result;
    mutexLock(&work_mutex);
    memset(page, 0, PageSize);
    CmifRequest request = cmifMakeRequest(page, (CmifRequestFormat){
        .request_id = command, .data_size = input_size, .send_pid = send_pid,
    });
    if (input_size) memcpy(request.data, input, input_size);
    work_session = session;
    state = Pending;
    condvarWakeOne(&request_ready);
    const u64 started = armGetSystemTick();
    while (state == Pending) {
        const u64 elapsed = armTicksToNs(armGetSystemTick() - started);
        if (elapsed >= timeout_ns) { result = timeoutError(); break; }
        result = condvarWaitTimeout(&reply_ready, &work_mutex, timeout_ns - elapsed);
        if (R_FAILED(result) && result != KERNELRESULT(Cancelled) &&
            result != KERNELRESULT(TimedOut)) break;
    }
    if (state == Done) {
        result = response(output, output_size, moved_handle, replied);
        state = Idle;
    }
    mutexUnlock(&work_mutex);
    return result;
}

void ncmIpcShutdown(void) {
    // A hung worker must never survive an NRO reload into recycled memory.
    if (!ncmIpcIdle()) svcExitProcess();
    if (!worker_started) return;
    mutexLock(&work_mutex);
    worker_stop = true;
    condvarWakeOne(&request_ready);
    mutexUnlock(&work_mutex);
    if (R_FAILED(threadWaitForExit(&worker))) svcExitProcess();
    threadClose(&worker);
    worker_started = false;
}
