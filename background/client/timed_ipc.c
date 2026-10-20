/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "timed_ipc.h"
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

enum { PageSize = 4096, MaxInline = 512 };
static void* page;
static Handle completion;

static Result protocolError(void) { return MAKERESULT(421, 1); }
static Result timeoutError(void) { return MAKERESULT(Module_Libnx, LibnxError_Timeout); }

/* The kernel validates handle descriptors, but data counts and CMIF contents
 * still come from the peer. Validate bounds before exposing any output. */
static Result response(void* output, size_t size, Handle* moved) {
    const u32* words = page;
    if (!words[0] && !words[1])
        return words[2] ? words[2] : protocolError(); /* Kernel async failure. */
    HipcHeader header;
    memcpy(&header, page, sizeof(header));
    HipcParsedRequest parsed = hipcParseRequest(page);
    Result result = protocolError();
    const uintptr_t start = (uintptr_t)parsed.data.data_words;
    const uintptr_t end = (uintptr_t)page + PageSize;
    const uintptr_t aligned = (start + 15) & ~(uintptr_t)15;
    const size_t bytes = (size_t)header.num_data_words * sizeof(u32);
    const size_t padding = aligned - start;
    if (!header.type && !header.num_send_statics && !header.num_send_buffers &&
        !header.num_recv_buffers && !header.num_exch_buffers &&
        !header.recv_static_mode && !parsed.meta.send_pid &&
        start >= (uintptr_t)page && start <= end && bytes <= end - start &&
        bytes >= padding + sizeof(CmifOutHeader)) {
        CmifOutHeader cmif;
        memcpy(&cmif, (void*)aligned, sizeof(cmif));
        if (cmif.magic == CMIF_OUT_HEADER_MAGIC && cmif.version == 0) {
            result = cmif.result;
            if (R_SUCCEEDED(result)) {
                if (bytes < padding + sizeof(cmif) + size ||
                    parsed.meta.num_copy_handles ||
                    parsed.meta.num_move_handles != (moved ? 1u : 0u)) {
                    result = protocolError();
                } else {
                    if (size) memcpy(output, (void*)(aligned + sizeof(cmif)), size);
                    if (moved) {
                        *moved = parsed.data.move_handles[0];
                        parsed.data.move_handles[0] = INVALID_HANDLE;
                        if (!*moved) result = protocolError();
                    }
                }
            }
        }
    }
    for (u32 i = 0; i < parsed.meta.num_copy_handles; ++i)
        if (parsed.data.copy_handles[i]) svcCloseHandle(parsed.data.copy_handles[i]);
    for (u32 i = 0; i < parsed.meta.num_move_handles; ++i)
        if (parsed.data.move_handles[i]) svcCloseHandle(parsed.data.move_handles[i]);
    return result;
}

bool ncmIpcIdle(void) {
    if (!completion) return true;
    s32 index = -1;
    if (R_FAILED(svcWaitSynchronization(&index, &completion, 1, 0))) return false;
    svcCloseHandle(completion);
    completion = INVALID_HANDLE;
    /* A late successful GetService can transfer a handle even though the
     * caller already timed out. Discard it rather than replaying the command. */
    response(NULL, 0, NULL);
    return true;
}

Result ncmIpcRequest(Handle session, u32 command, const void* input, size_t input_size,
                    void* output, size_t output_size, Handle* moved_handle,
                    bool send_pid, u64 timeout_ns) {
    if (!session || input_size > MaxInline || output_size > MaxInline ||
        (input_size && !input) || (output_size && !output))
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (output_size) memset(output, 0, output_size);
    if (moved_handle) *moved_handle = INVALID_HANDLE;
    if (!ncmIpcIdle()) return timeoutError();
    if (!envIsSyscallHinted(0x23))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);
    if (!page) page = memalign(PageSize, PageSize);
    if (!page) return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    memset(page, 0, PageSize);
    CmifRequest request = cmifMakeRequest(page, (CmifRequestFormat){
        .request_id = command, .data_size = input_size, .send_pid = send_pid,
    });
    if (input_size) memcpy(request.data, input, input_size);
    Result result = svcSendAsyncRequestWithUserBuffer(&completion, page, PageSize, session);
    if (R_FAILED(result)) { completion = INVALID_HANDLE; return result; }
    const u64 started = armGetSystemTick();
    for (;;) {
        const u64 elapsed = armTicksToNs(armGetSystemTick() - started);
        s32 index = -1;
        result = svcWaitSynchronization(&index, &completion, 1,
                                       elapsed < timeout_ns ? timeout_ns - elapsed : 0);
        if (result != KERNELRESULT(Cancelled) || elapsed >= timeout_ns) break;
    }
    if (R_FAILED(result)) {
        /* Do not free, overwrite, or allocate another page while the kernel
         * owns this request. One hung peer therefore consumes at most 4 KiB. */
        return result == KERNELRESULT(TimedOut) ? timeoutError() : result;
    }
    svcCloseHandle(completion);
    completion = INVALID_HANDLE;
    return response(output, output_size, moved_handle);
}

void ncmIpcShutdown(void) {
    if (!ncmIpcIdle()) {
        /* Closing a client handle does not cancel a request already received
         * by a hung server. Process exit is the only safe bounded teardown;
         * returning to hbloader would let it recycle still-owned memory. */
        svcExitProcess();
    }
    free(page);
    page = NULL;
}
