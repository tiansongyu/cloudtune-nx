/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ipc_stubs/switch.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
static unsigned char* tls;
static Handle closed[32];
static unsigned closed_count;
static unsigned char incoming[256], outgoing[256];
static Result receive_result;
static unsigned reply_count, handler_count;
static bool clean_receive_template = true;
void* armGetTls(void) { return tls; }
Result svcCloseHandle(Handle handle) { closed[closed_count++] = handle; return 0; }
Result svcAcceptSession(Handle* out, Handle in) { (void)out; (void)in; return 1; }
Result svcReplyAndReceive(s32* a, const Handle* b, s32 c, Handle d, u64 e) {
    (void)b; (void)e;
    if (c == 1 && d == 0) {
        for (unsigned i = 0; i < 256; ++i)
            if (tls[i]) clean_receive_template = false;
        if (R_FAILED(receive_result)) return receive_result;
        memcpy(tls, incoming, sizeof(incoming));
        *a = 0;
        return 0;
    }
    if (c == 0 && d != 0) {
        memcpy(outgoing, tls, sizeof(outgoing));
        ++reply_count;
        return KERNELRESULT(TimedOut);
    }
    return 1;
}
Result svcWaitSynchronization(s32* a, const Handle* b, s32 c, u64 d) {
    (void)a; (void)b; (void)c; (void)d; return 1;
}
Result smRegisterServiceCmif(Handle* a, SmServiceName b, bool c, s32 d) {
    (void)a; (void)b; (void)c; (void)d; return 1;
}
Result smUnregisterServiceCmif(SmServiceName name) { (void)name; return 0; }
SmServiceName smEncodeName(const char* name) { (void)name; return 0; }
#include "../background/source/ipc_server.c"
static void check(bool condition, const char* reason) {
    if (!condition) { fprintf(stderr, "FAIL: %s\n", reason); exit(1); }
}
static void message(unsigned words) {
    memset(tls, 0, 256);
    HipcHeader header = {0};
    header.type = CmifCommandType_Request;
    header.num_data_words = words;
    memcpy(tls, &header, sizeof(header));
    const NcmIpcRawHeader cmif = {CMIF_IN_HEADER_MAGIC, {.command_id=16}};
    memcpy(tls + 16, &cmif, sizeof(cmif));
}
static Result poll_handler(void* context, const NcmIpcServerRequest* request,
                           uint8_t* output, size_t* size) {
    (void)context;
    check(request->command_id == 16 && request->data_size == 0,
          "Synthetic polling request was parsed incorrectly");
    const uint32_t value = 42;
    memcpy(output, &value, sizeof(value));
    *size = sizeof(value);
    ++handler_count;
    return 0;
}
static void diagnostic_tests(void) {
    NcmIpcServer server = {.handles={80, 81}, .max_handles=2, .handle_count=2};
    message(8);
    memcpy(incoming, tls, sizeof(incoming));
    closed_count = 0;
    g_trace_file = tmpfile(); // Anonymous host file; never open a real SD path.
    check(g_trace_file != NULL, "Cannot create diagnostic fixture");
    for (unsigned i = 0; i < 4096; ++i)
        check(processSession(&server, poll_handler, NULL, 1) == 0,
              "Normal polling failed");
    uint32_t response = 0;
    memcpy(&response, outgoing + 32, sizeof(response));
    check(reply_count == 4096 && handler_count == 4096 && response == 42 &&
          clean_receive_template && closed_count == 0 && server.handle_count == 2,
          "Polling changed IPC replies, receive templates or session lifetime");
    check(ftell(g_trace_file) == 0, "Successful IPC polling grew the diagnostic log");

    traceResult("sm-register", 0);
    const long lifecycle_size = ftell(g_trace_file);
    check(lifecycle_size > 0, "Lifecycle diagnostic was lost");
    receive_result = 0x1234;
    check(R_FAILED(processSession(&server, poll_handler, NULL, 1)) &&
          server.handle_count == 1 && closed_count == 1 && closed[0] == 81,
          "Failed receive did not release its session");
    const long first_failure_size = ftell(g_trace_file);
    check(first_failure_size > lifecycle_size, "First receive failure was not recorded");
    for (unsigned i = 0; i < 4096; ++i) {
        server.handle_count = 2;
        closed_count = 0;
        check(R_FAILED(processSession(&server, poll_handler, NULL, 1)),
              "Repeated receive failure was accepted");
    }
    check(ftell(g_trace_file) == first_failure_size,
          "Repeated identical failures grew the diagnostic log");
    for (unsigned i = 0; i < 8192; ++i) {
        server.handle_count = 2;
        closed_count = 0;
        receive_result = 1 + (i % 2);
        check(R_FAILED(processSession(&server, poll_handler, NULL, 1)) &&
              server.handle_count == 1 && closed_count == 1 && closed[0] == 81,
              "Diagnostic limit changed failed-session cleanup");
    }
    const long final_size = ftell(g_trace_file);
    check(final_size > 60 * 1024 && final_size <= 64 * 1024,
          "IPC diagnostics exceeded their persistent size limit");
    check(handler_count == 4096 && reply_count == 4096,
          "Failed receives reached the handler or reply path");
    traceClose();
}
int main(void) {
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    void* allocation = mmap(NULL, page * 2, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(allocation != MAP_FAILED, "mmap failed");
    check(mprotect((u8*)allocation + page, page, PROT_NONE) == 0, "guard page failed");
    tls = (u8*)allocation + page - 256;
    NcmIpcServerRequest request;
    message(9);
    const u32 value = 700;
    memcpy(tls + 32, &value, sizeof(value));
    check(parseRequest(&request) == 0 && request.data_size == 4,
          "CMIF argument size includes alignment padding");
    check(*(const u32*)request.data == value, "CMIF argument was misplaced");
    message(8);
    check(parseRequest(&request) == 0 && request.data_size == 0,
          "argument-free request incorrectly exposes padding as an argument");
    message(4);
    check(R_FAILED(parseRequest(&request)), "short CMIF header accepted");
    message(1023);
    check(R_FAILED(parseRequest(&request)), "oversized TLS data accepted");
    message(8);
    ((u32*)(tls + 16))[1] = 1;
    check(R_FAILED(parseRequest(&request)), "unsupported CMIF header version accepted");
    message(8);
    ((HipcHeader*)tls)->type = CmifCommandType_Control;
    ((NcmIpcRawHeader*)(tls + 16))->command_id = 3;
    check(parseRequest(&request) == 0 && request.command_id == 3 && request.data_size == 0,
          "valid pointer-buffer control request rejected");
    message(8);
    HipcHeader* header = (HipcHeader*)tls;
    header->has_special_header = 1;
    const HipcSpecialHeader special = {.num_copy_handles=1, .num_move_handles=1};
    memcpy(tls + 8, &special, sizeof(special));
    const Handle handles[] = {71, 72};
    memcpy(tls + 12, handles, sizeof(handles));
    check(R_FAILED(parseRequest(&request)), "unsupported transferred handles accepted");
    check(closed_count == 2 && closed[0] == 71 && closed[1] == 72,
          "rejected transferred handles leaked");
    message(8);
    header->num_send_buffers = header->num_recv_buffers = header->num_exch_buffers = 15;
    header->num_send_statics = 15;
    const pid_t child = fork();
    check(child >= 0, "fork failed");
    if (!child) _exit(R_FAILED(parseRequest(&request)) ? 0 : 1);
    int result = 0;
    check(waitpid(child, &result, 0) == child && WIFEXITED(result) && WEXITSTATUS(result) == 0,
          "descriptor counts accessed beyond the TLS guard page");
    /* Exhaust the bounded HIPC bit fields against the protected TLS window. */
    for (unsigned words = 0; words < 1024; ++words) {
        message(words);
        const Result parsed = parseRequest(&request);
        const bool inline_valid = words >= 8 && words <= 62;
        check(R_SUCCEEDED(parsed) == inline_valid, "TLS raw word boundary mismatch");
        if (inline_valid && request.data_size)
            check((const u8*)request.data >= tls &&
                  (const u8*)request.data + request.data_size <= tls + 256,
                  "Parsed CMIF argument extends beyond TLS");
    }
    for (unsigned copy = 0; copy < 16; ++copy)
        for (unsigned move = 0; move < 16; ++move)
            for (unsigned pid = 0; pid < 2; ++pid) {
                message(8);
                ((HipcHeader*)tls)->has_special_header = 1;
                const HipcSpecialHeader h = {
                    .send_pid=pid, .num_copy_handles=copy, .num_move_handles=move};
                memcpy(tls + 8, &h, sizeof(h));
                const size_t start = 12 + (pid ? 8 : 0);
                for (unsigned i = 0; i < copy + move; ++i) {
                    const Handle value = 100 + i;
                    memcpy(tls + start + i * sizeof(value), &value, sizeof(value));
                }
                closed_count = 0;
                check(R_FAILED(parseRequest(&request)) && closed_count == copy + move,
                      "Rejected HIPC transfer did not release every handle");
                for (unsigned i = 0; i < closed_count; ++i)
                    check(closed[i] == 100 + i, "HIPC handle offsets were parsed incorrectly");
            }
    uint8_t response[NCM_IPC_RESPONSE_MAX_DATA_SIZE];
    memset(response, 0x5a, sizeof(response));
    prepareResponse(0, response, sizeof(response));
    check(((NcmIpcRawHeader*)(tls + 16))->magic == CMIF_OUT_HEADER_MAGIC &&
          memcmp(tls + 32, response, sizeof(response)) == 0,
          "maximum inline response failed its TLS bounds");
    diagnostic_tests();
    munmap(allocation, page * 2);
    puts("PASS: CMIF bounds, 1024 TLS lengths, 512 handle layouts, 4096 polls and bounded failure logging");
    return 0;
}
