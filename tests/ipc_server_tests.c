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
void* armGetTls(void) { return tls; }
Result svcCloseHandle(Handle handle) { closed[closed_count++] = handle; return 0; }
Result svcAcceptSession(Handle* out, Handle in) { (void)out; (void)in; return 1; }
Result svcReplyAndReceive(s32* a, const Handle* b, s32 c, Handle d, u64 e) {
    (void)a; (void)b; (void)c; (void)d; (void)e; return 1;
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
    uint8_t response[NCM_IPC_RESPONSE_MAX_DATA_SIZE];
    memset(response, 0x5a, sizeof(response));
    prepareResponse(0, response, sizeof(response));
    check(((NcmIpcRawHeader*)(tls + 16))->magic == CMIF_OUT_HEADER_MAGIC &&
          memcmp(tls + 32, response, sizeof(response)) == 0,
          "maximum inline response failed its TLS bounds");
    munmap(allocation, page * 2);
    puts("PASS: CMIF payload size, TLS bounds, descriptors and transferred handles");
    return 0;
}
