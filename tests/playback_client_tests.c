/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "netease/playback_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static NcmPlaybackStatus reply;
static NcmAirplayStatus airplay_reply;
static Result reply_result;
static void* wire;
static u64 ticks;
static bool ready, stall, allowed = true, cancel_once;
static unsigned sends, closes[256];
static u32 last_command, last_argument;
static void check(bool value, const char* reason) {
    if (!value) { fprintf(stderr, "FAIL: %s\n", reason); exit(1); }
}
SmServiceName smEncodeName(const char* name) { (void)name; return 1; }
bool envIsSyscallHinted(unsigned id) { check(id == 0x23, "wrong syscall hint"); return allowed; }
u64 armGetSystemTick(void) { return ticks; }
Result svcConnectToNamedPort(Handle* handle, const char* name) {
    check(strcmp(name, "sm:") == 0, "wrong named port"); *handle = 1; return 0;
}
Result svcCloseHandle(Handle handle) {
    check(handle < 256 && handle != 0, "invalid handle closed"); ++closes[handle]; return 0;
}
_Noreturn void svcExitProcess(void) { _exit(70); }
static void respond(const void* bytes, size_t size, Handle handle, Result result) {
    memset(wire, 0, 4096);
    HipcHeader* header = wire;
    size_t offset = sizeof(*header);
    if (handle) {
        header->has_special_header = 1;
        const HipcSpecialHeader special = {.num_move_handles=1};
        memcpy((u8*)wire + offset, &special, sizeof(special)); offset += sizeof(special);
        memcpy((u8*)wire + offset, &handle, sizeof(handle)); offset += sizeof(handle);
    }
    const size_t aligned = (offset + 15) & ~(size_t)15;
    header->num_data_words = (aligned - offset + sizeof(CmifOutHeader) + size + 3) / 4;
    const CmifOutHeader cmif = {CMIF_OUT_HEADER_MAGIC, 0, result, 0};
    memcpy((u8*)wire + aligned, &cmif, sizeof(cmif));
    if (size) memcpy((u8*)wire + aligned + sizeof(cmif), bytes, size);
    ready = true;
}
Result svcSendAsyncRequestWithUserBuffer(Handle* event, void* buffer, u64 size, Handle session) {
    check(size == 4096 && ((uintptr_t)buffer & 4095) == 0, "unaligned/non-page IPC buffer");
    ++sends; wire = buffer; ready = false; *event = 100;
    const HipcParsedRequest parsed = hipcParseRequest(buffer);
    const u32* cmif = cmifGetAlignedDataStart(parsed.data.data_words, buffer);
    check(cmif[0] == CMIF_IN_HEADER_MAGIC, "missing CMIF request magic");
    last_command = cmif[2]; last_argument = cmif[4];
    if (stall) return 0;
    if (session == 1) {
        if (!last_command) {
            check(parsed.meta.send_pid, "SM registration omitted PID");
            respond(NULL, 0, 0, 0);
        } else { check(last_command == 1, "wrong SM command"); respond(NULL, 0, 42, 0); }
    } else if (last_command == NcmCommand_Version) {
        const u32 version = NCM_PROTOCOL_VERSION; respond(&version, sizeof(version), 0, 0);
    } else if (last_command == NcmCommand_Status) respond(&reply, sizeof(reply), 0, reply_result);
    else if (last_command == NCM_COMMAND_AIRPLAY_STATUS)
        respond(&airplay_reply, sizeof(airplay_reply), 0, reply_result);
    else respond(NULL, 0, 0, reply_result);
    return 0;
}
Result svcWaitSynchronization(s32* index, const Handle* handles, s32 count, u64 timeout) {
    check(count == 1 && *handles == 100, "wrong completion event");
    if (cancel_once) { cancel_once = false; ticks += 1000000; return KERNELRESULT(Cancelled); }
    if (!ready) { ticks += timeout; return KERNELRESULT(TimedOut); }
    *index = 0; return 0;
}
#include "../background/client/timed_ipc.c"
#include "../background/client/playback_client.c"

int main(void) {
    reply.version = NCM_PROTOCOL_VERSION;
    airplay_reply.version = NCM_AIRPLAY_VERSION;
    check(ncmPlaybackInitialize() == 0, "service initialization failed");
    check(closes[1] == 1 && ncmPlaybackIsActive(), "private SM leaked");
    NcmPlaybackStatus status, zero = {0};
    memset(&reply, 'x', sizeof(reply)); reply.version = NCM_PROTOCOL_VERSION;
    check(ncmPlaybackGetStatus(&status) == 0, "valid status rejected");
    check(!status.title[sizeof(status.title)-1] && !status.artist[sizeof(status.artist)-1] &&
          !status.message[sizeof(status.message)-1], "unterminated status fields");
    reply.version++;
    check(R_FAILED(ncmPlaybackGetStatus(&status)) && !memcmp(&status,&zero,sizeof(status)),
          "incompatible status exposed");
    reply.version = NCM_PROTOCOL_VERSION;
    reply_result = 1;
    check(R_FAILED(ncmPlaybackGetStatus(&status)) && !memcmp(&status,&zero,sizeof(status)),
          "failed status exposed");
    check(!ncmPlaybackIsActive(), "failed connection retained");
    reply_result = 0;
    check(ncmPlaybackInitialize() == 0, "reconnect failed");
    check(R_FAILED(ncmPlaybackGetStatus(NULL)), "NULL output accepted");
    check(ncmPlaybackSetVolume(600) == 0 && last_command == NcmCommand_Volume &&
          last_argument == 600, "control argument corrupted");
    NcmAirplayStatus airplay, empty_airplay = {0};
    memset(&airplay_reply,'y',sizeof(airplay_reply)); airplay_reply.version=NCM_AIRPLAY_VERSION;
    check(ncmAirplayGetStatus(&airplay) == 0 && !airplay.name[sizeof(airplay.name)-1] &&
          !airplay.message[sizeof(airplay.message)-1], "AirPlay text not bounded");
    airplay_reply.version++;
    check(R_FAILED(ncmAirplayGetStatus(&airplay)) && !memcmp(&airplay,&empty_airplay,sizeof(airplay)),
          "incompatible AirPlay status exposed");
    check(R_FAILED(ncmAirplayGetStatus(NULL)), "NULL AirPlay output accepted");
    cancel_once = true;
    check(ncmPlaybackGetStatus(&status) == 0, "cancelled wait was not retried");

    stall = true;
    const u64 started = ticks;
    check(R_FAILED(ncmPlaybackGetStatus(&status)) && ticks-started == NormalTimeout,
          "stalled peer did not respect deadline");
    check(!memcmp(&status,&zero,sizeof(status)), "timeout exposed stale data");
    unsigned submitted = sends;
    unsigned char pending_bytes[4096]; memcpy(pending_bytes,wire,sizeof(pending_bytes));
    for (int i=0;i<100;++i) check(R_FAILED(ncmPlaybackInitialize()),"pending request reused");
    check(sends == submitted && !memcmp(pending_bytes,wire,sizeof(pending_bytes)),
          "pending kernel buffer overwritten or retry allocated another request");
    ncmPlaybackExit();
    check(!memcmp(pending_bytes,wire,sizeof(pending_bytes)), "Exit touched pending buffer");
    // Final shutdown cannot safely return to an NRO loader with a live buffer.
    const pid_t child = fork(); check(child >= 0, "fork failed");
    if (!child) { ncmPlaybackShutdown(); _exit(99); }
    int exit_status = 0; waitpid(child,&exit_status,0);
    check(WIFEXITED(exit_status) && WEXITSTATUS(exit_status)==70,"unsafe pending shutdown");
    respond(NULL,0,77,0); // Late moved handle must not leak or be adopted.
    stall = false;
    check(ncmPlaybackInitialize() == 0 && closes[77] == 1, "late reply handle leaked");

    // Malformed response length and unexpected handles are rejected and closed.
    stall = true;
    check(R_FAILED(ncmPlaybackGetStatus(&status)),"second stall failed");
    respond(&reply,sizeof(reply),78,0);
    ((HipcHeader*)wire)->num_data_words = 1023;
    check(ncmIpcIdle() && closes[78]==1,"malformed late reply leaked handle");
    stall = false;
    check(ncmPlaybackInitialize() == 0, "recovery after malformed reply failed");
    stall = true;
    check(R_FAILED(ncmPlaybackGetStatus(&status)),"kernel-error fixture failed");
    memset(wire,0,4096); ((u32*)wire)[2]=KERNELRESULT(ConnectionClosed); ready=true;
    check(ncmIpcIdle(), "kernel async error not reaped");
    stall=false;
    ncmPlaybackShutdown();
    allowed=false;
    check(R_FAILED(ncmPlaybackInitialize()) && !completion, "unsupported SVC used");
    ncmPlaybackShutdown();
    puts("PASS: bounded Switch IPC, private SM, late replies, handle cleanup, shutdown and status validation");
}
