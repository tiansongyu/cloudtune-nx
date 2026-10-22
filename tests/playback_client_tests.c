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
static bool ready, stall;
static Mutex fake_mutex = PTHREAD_MUTEX_INITIALIZER;
static CondVar fake_ready = PTHREAD_COND_INITIALIZER;
Result testThreadCreateResult;
static Result kernel_result;
static unsigned sends, closes[256], named_port_attempts;
static u32 last_command, last_argument;
static bool corrupt_reply_type;
static bool service_present = true;
static Result process_result = MAKERESULT(15, 1), launch_result;
static unsigned launches, pm_acquired, pm_released;
bool testAtmosphereHint = true;
static Result kernel_probe_result;
static u64 kernel_version = 0x140400;
Result svcGetInfo(u64* out, InfoType type, Handle handle, u64 sub) {
    if (type != 65000 || handle || sub) abort();
    *out = kernel_version;
    return kernel_probe_result;
}
Result testHasService(u8* present) { *present = service_present; return 0; }
Result pmshellInitialize(void) { ++pm_acquired; return 0; }
void pmshellExit(void) { ++pm_released; }
Result pmdmntInitialize(void) { ++pm_acquired; return 0; }
void pmdmntExit(void) { ++pm_released; }
Result pmdmntGetProcessId(u64* pid, u64 program) {
    if (program != NCM_PROGRAM_ID) abort();
    *pid = process_result ? 0 : 99;
    return process_result;
}
Result pmshellLaunchProgram(u32 flags, const NcmProgramLocation* location, u64* pid) {
    if (flags || location->program_id != NCM_PROGRAM_ID) abort();
    ++launches; *pid = 99; return launch_result;
}
static _Thread_local u8 server_tls[256] __attribute__((aligned(16)));
static _Thread_local u8 client_tls[256] __attribute__((aligned(16)));
static _Thread_local bool server_side;
static void check(bool value, const char* reason) {
    if (!value) { fprintf(stderr, "FAIL: %s\n", reason); exit(1); }
}
SmServiceName smEncodeName(const char* name) { (void)name; return 1; }
u64 armGetSystemTick(void) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now);
    return (u64)now.tv_sec * 1000000000 + now.tv_nsec;
}
Result svcConnectToNamedPort(Handle* handle, const char* name) {
    check(strcmp(name, "sm:") == 0, "wrong named port");
    // Atmosphere application mode has one session-creation slot, already
    // occupied by libnx's shared SM session. More free RAM cannot fix this.
    ++named_port_attempts; *handle = INVALID_HANDLE; return 0x10801;
}
Result svcCloseHandle(Handle handle) {
    check(handle < 256 && handle != 0, "invalid handle closed"); ++closes[handle]; return 0;
}
_Noreturn void svcExitProcess(void) { _exit(70); }
void* armGetTls(void) { return server_side ? server_tls : client_tls; }
Result svcAcceptSession(Handle* out, Handle port) { (void)out; (void)port; return 1; }
Result svcReplyAndReceive(s32* index, const Handle* handles, s32 count, Handle reply_to, u64 timeout) {
    (void)index; (void)handles; (void)count; (void)reply_to; (void)timeout; return 1;
}
Result smRegisterServiceCmif(Handle* out, SmServiceName name, bool light, s32 sessions) {
    (void)out; (void)name; (void)light; (void)sessions; return 1;
}
Result smUnregisterServiceCmif(SmServiceName name) { (void)name; return 0; }
#include "../background/source/ipc_server.c"
static void serverRespond(const void* bytes, size_t size, Result result) {
    // Exercise the real service parser and reply encoder, not a second
    // handwritten approximation of CloudTune's CMIF response.
    memcpy(server_tls, wire, sizeof(server_tls));
    server_side = true;
    NcmIpcServerRequest request;
    check(parseRequest(&request) == 0 && request.command_id == last_command,
          "production server rejected client request");
    if (request.data_size) {
        check(request.data_size == sizeof(last_argument), "control argument size mismatch");
        check(!memcmp(request.data, &last_argument, sizeof(last_argument)), "control argument mismatch");
    }
    prepareResponse(result, bytes, size);
    server_side = false;
    memset(wire, 0, 256);
    memcpy(wire, server_tls, sizeof(server_tls));
    if (corrupt_reply_type) ((HipcHeader*)wire)->type = CmifCommandType_Control;
    ready = true;
}
static void respond(const void* bytes, size_t size, Handle handle, Result result) {
    memset(wire, 0, 256);
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
Result svcSendSyncRequest(Handle session) {
    void* buffer = armGetTls();
    mutexLock(&fake_mutex);
    ++sends; wire = buffer; ready = false;
    const HipcParsedRequest parsed = hipcParseRequest(buffer);
    const u32* cmif = cmifGetAlignedDataStart(parsed.data.data_words, buffer);
    check(cmif[0] == CMIF_IN_HEADER_MAGIC, "missing CMIF request magic");
    last_command = cmif[2]; last_argument = cmif[4];
    if (stall) {
        while (!ready) condvarWait(&fake_ready, &fake_mutex);
        mutexUnlock(&fake_mutex);
        return kernel_result;
    }
    if (session == 1) {
        if (!last_command) {
            check(parsed.meta.send_pid, "SM registration omitted PID");
            respond(NULL, 0, 0, 0);
        } else { check(last_command == 1, "wrong SM command"); respond(NULL, 0, 42, 0); }
    } else if (last_command == NcmCommand_Version) {
        const u32 version = NCM_PROTOCOL_VERSION; serverRespond(&version, sizeof(version), 0);
    } else if (last_command == NcmCommand_Status) serverRespond(&reply, sizeof(reply), reply_result);
    else if (last_command == NCM_COMMAND_AIRPLAY_STATUS)
        serverRespond(&airplay_reply, sizeof(airplay_reply), reply_result);
    else serverRespond(NULL, 0, reply_result);
    mutexUnlock(&fake_mutex);
    return 0;
}
Result svcWaitSynchronization(s32* index, const Handle* handles, s32 count, u64 timeout) {
    (void)index; (void)handles; (void)count; (void)timeout; return 1;
}
#include "../background/client/timed_ipc.c"
static const char* startup_fixture;
static FILE* client_log_fixture;
static FILE* diagnosticOpen(const char* path, const char* mode) {
    if (!strcmp(path, NCM_CONFIG_DIRECTORY "/ipc-client-app.log")) {
        if (!client_log_fixture) client_log_fixture = tmpfile();
        check(client_log_fixture != NULL, "client log fixture failed");
        return fdopen(dup(fileno(client_log_fixture)), mode);
    }
    check(!strcmp(path,NCM_CONFIG_DIRECTORY "/background-startup.log"), "unexpected diagnostic file");
    if (!startup_fixture) return NULL;
    FILE* file=tmpfile(); check(file!=NULL,"diagnostic fixture failed");
    fputs(startup_fixture,file); rewind(file); return file;
}
#define fopen diagnosticOpen
#include "../background/client/playback_client.c"
#undef fopen
static void awaitIdle(void) {
    for(unsigned i=0;i<1000 && !ncmIpcIdle();++i) usleep(1000);
    check(ncmIpcIdle(), "worker never completed late reply");
}

int main(void) {
    testAtmosphereHint = false;
    check(ncmPlaybackEnvironmentSupported(), "legacy loader's missing hint rejected Atmosphere kernel");
    kernel_probe_result = 0xF001;
    check(!ncmPlaybackEnvironmentSupported() && R_FAILED(ncmPlaybackInitialize()),
          "stock kernel treated as Atmosphere");
    kernel_probe_result = 0;
    kernel_version = 0;
    check(!ncmPlaybackEnvironmentSupported(), "empty kernel version accepted");
    kernel_version = 0x140400;
    // Continue the complete handshake/control suite without the HBABI hint.
    // PM presence and service presence are deliberately independent. A booting
    // process must never be launched a second time just because IPC is absent.
    process_result = 0;
    service_present = false;
    check(ncmPlaybackLaunchBackground() == 0 && launches == 0, "duplicate process launch");
    check(R_FAILED(ncmPlaybackInitialize()), "missing IPC treated as connected");
    char startup_message[256];
    startup_fixture = "resource memory: used=328032256 limit=328204288 free=172032\n"
        "boot-resource-gate: 0x00006359\nstartup-exit: 0x00006359\n";
    ncmPlaybackConnectionMessage(startup_message,sizeof(startup_message));
    check(strstr(startup_message,"168 KiB") && strstr(startup_message,"上次"),
          "physical low-memory evidence not explained as historical");
    startup_fixture = "resource memory: used=328032256 limit=328204288 free=172032\n";
    ncmPlaybackConnectionMessage(startup_message,sizeof(startup_message));
    check(!strstr(startup_message,"内存不足"), "in-progress startup misreported as failed");
    startup_fixture = NULL;
    process_result = 0x1234;
    check(ncmPlaybackLaunchBackground() == process_result && launches == 0,
          "unknown process state launched a duplicate");
    process_result = MAKERESULT(15, 1);
    check(ncmPlaybackLaunchBackground() == 0 && launches == 1, "missing process not launched");
    check(pm_acquired == pm_released, "PM lookup/launch leaked sessions");
    service_present = true;
    reply.version = NCM_PROTOCOL_VERSION;
    airplay_reply.version = NCM_AIRPLAY_VERSION;
    check(ncmPlaybackInitialize() == 0, "service initialization failed");
    check(named_port_attempts == 0 && closes[1] == 0 && ncmPlaybackIsActive(),
          "client must borrow the shared SM without creating/closing another session");
    NcmPlaybackStatus status, zero = {0};
    memset(&reply, 'x', sizeof(reply)); reply.version = NCM_PROTOCOL_VERSION;
    check(ncmPlaybackGetStatus(&status) == 0, "valid status rejected");
    check(!status.title[sizeof(status.title)-1] && !status.artist[sizeof(status.artist)-1] &&
          !status.message[sizeof(status.message)-1], "unterminated status fields");
    reply.version++;
    check(R_FAILED(ncmPlaybackGetStatus(&status)) && !memcmp(&status,&zero,sizeof(status)),
          "incompatible status exposed");
    reply.version = NCM_PROTOCOL_VERSION;
    check(ncmPlaybackInitialize() == 0, "reconnect after incompatible status failed");
    reply_result = 1;
    check(R_FAILED(ncmPlaybackGetStatus(&status)) && !memcmp(&status,&zero,sizeof(status)),
          "failed status exposed");
    check(ncmPlaybackIsActive(), "valid service error discarded healthy connection");
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
    airplay_reply.version = NCM_AIRPLAY_VERSION;
    reply_result = MAKERESULT(Module_Libnx, LibnxError_BadInput);
    check(ncmAirplayGetStatus(&airplay) == reply_result && ncmPlaybackIsActive(),
          "unsupported tool discarded playback connection");
    reply_result = 0;
    check(ncmPlaybackGetStatus(&status) == 0, "music status unavailable after tool rejection");
    corrupt_reply_type = true;
    check(R_FAILED(ncmPlaybackGetStatus(&status)) && !ncmPlaybackIsActive() &&
          !memcmp(&status, &zero, sizeof(status)), "invalid reply type accepted");
    corrupt_reply_type = false;
    check(ncmPlaybackInitialize() == 0, "reconnect after invalid reply failed");

    // HasService/GetService can race with service exit. The shared SM is
    // borrowed, even when GetService completes after the caller's deadline.
    ncmPlaybackExit();
    stall = true;
    check(R_FAILED(ncmPlaybackInitialize()) && !ncmPlaybackIsActive(),
          "deferred SM acquisition did not time out");
    const unsigned pm_before_stall = pm_acquired;
    u64 process_id = 55;
    check(R_FAILED(ncmPlaybackGetBackgroundProcessId(&process_id)) && !process_id &&
          R_FAILED(ncmPlaybackLaunchBackground()) && pm_acquired == pm_before_stall,
          "pending shared SM entered an unbounded caller-thread PM lookup");
    check(closes[1] == 0, "timeout closed libnx's borrowed SM session");
    mutexLock(&fake_mutex);
    respond(NULL, 0, 76, 0);
    condvarWakeAll(&fake_ready);
    mutexUnlock(&fake_mutex);
    for (unsigned i=0; i<1000 && ncmIpcPending(); ++i) usleep(1000);
    check(!ncmIpcPending() && state == Done, "late SM reply did not finish");
    ncmPlaybackGetBackgroundProcessId(&process_id);
    check(state == Done && closes[76] == 0,
          "PM guard reaped a reply owned by the serialized client");
    awaitIdle();
    stall = false;
    check(closes[76] == 1 && closes[1] == 0 && ncmPlaybackInitialize() == 0,
          "late SM reply leaked a handle or prevented recovery");

    const long log_before_polls = ftell(client_log_fixture);
    for (unsigned i=0; i<100; ++i)
        check(ncmPlaybackGetStatus(&status) == 0, "repeated status poll failed");
    check(ftell(client_log_fixture) == log_before_polls, "successful polling wrote client log");

    stall = true;
    const unsigned closed_before_timeout = closes[42];
    const u64 started = armGetSystemTick();
    check(R_FAILED(ncmPlaybackGetStatus(&status)) && armGetSystemTick()-started >= NormalTimeout && armGetSystemTick()-started < NormalTimeout + 200000000ull,
          "stalled peer did not respect deadline");
    check(!memcmp(&status,&zero,sizeof(status)), "timeout exposed stale data");
    check(closes[42] == closed_before_timeout, "pending worker's session closed too early");
    unsigned submitted = sends;
    unsigned char pending_bytes[256]; memcpy(pending_bytes,wire,sizeof(pending_bytes));
    for (int i=0;i<100;++i) check(R_FAILED(ncmPlaybackInitialize()),"pending request reused");
    check(sends == submitted && !memcmp(pending_bytes,wire,sizeof(pending_bytes)),
          "pending worker buffer overwritten or retry allocated another request");
    ncmPlaybackExit();
    check(!memcmp(pending_bytes,wire,sizeof(pending_bytes)), "Exit touched pending buffer");
    // Final shutdown cannot safely return to an NRO loader with a live buffer.
    const pid_t child = fork(); check(child >= 0, "fork failed");
    if (!child) { ncmPlaybackShutdown(); _exit(99); }
    int exit_status = 0; waitpid(child,&exit_status,0);
    check(WIFEXITED(exit_status) && WEXITSTATUS(exit_status)==70,"unsafe pending shutdown");
    mutexLock(&fake_mutex);
    respond(NULL,0,77,0); // Late moved handle must not leak or be adopted.
    condvarWakeAll(&fake_ready);
    mutexUnlock(&fake_mutex);
    awaitIdle();
    check(closes[42] == closed_before_timeout + 1, "deferred close leaked session");
    stall = false;
    check(ncmPlaybackInitialize() == 0 && closes[77] == 1, "late reply handle leaked");

    // Malformed response length and unexpected handles are rejected and closed.
    stall = true;
    check(R_FAILED(ncmPlaybackGetStatus(&status)),"second stall failed");
    mutexLock(&fake_mutex);
    respond(&reply,sizeof(reply),78,0);
    ((HipcHeader*)wire)->num_data_words = 1023;
    condvarWakeAll(&fake_ready); mutexUnlock(&fake_mutex); awaitIdle();
    check(ncmIpcIdle() && closes[78]==1,"malformed late reply leaked handle");
    stall = false;
    check(ncmPlaybackInitialize() == 0, "recovery after malformed reply failed");
    stall = true;
    check(R_FAILED(ncmPlaybackGetStatus(&status)),"kernel-error fixture failed");
    mutexLock(&fake_mutex);
    kernel_result=KERNELRESULT(ConnectionClosed); ready=true;
    condvarWakeAll(&fake_ready); mutexUnlock(&fake_mutex); awaitIdle();
    check(ncmIpcIdle(), "worker transport error not reaped");
    stall=false;
    ncmPlaybackShutdown();
    testThreadCreateResult=0x10801;
    check(ncmPlaybackInitialize()==testThreadCreateResult && !worker_started, "worker creation failure not propagated");
    char diagnostic[128];
    ncmPlaybackConnectionMessage(diagnostic, sizeof(diagnostic));
    check(strstr(diagnostic, "获取后台会话") && strstr(diagnostic, "0x00010801"), "handshake error lacks stage/result");
    const size_t bytes_before_retries = g_trace_bytes;
    for (unsigned i=0; i<100; ++i) ncmPlaybackInitialize();
    check(g_trace_bytes == bytes_before_retries, "identical connection failures grew the log");
    ncmPlaybackShutdown();
    check(named_port_attempts == 0 && closes[1] == 0,
          "client created an extra named-port session or closed shared SM");
    rewind(client_log_fixture);
    char logged[4096] = {0};
    fread(logged, 1, sizeof(logged)-1, client_log_fixture);
    check(strstr(logged,"已连接") && strstr(logged,"获取后台会话: 0x00010801") &&
          strstr(logged,"命令已接受"), "missing actionable client diagnostics");
    fclose(client_log_fixture);
    puts("PASS: production client/server framing, service errors, bounded IPC, late replies, handles and shutdown");
}
