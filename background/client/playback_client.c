/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "netease/playback_client.h"
#include <string.h>
#include <stdio.h>
#include "timed_ipc.h"
#include <stdatomic.h>
static Handle g_service;
static atomic_flag g_busy = ATOMIC_FLAG_INIT;
static const u64 NormalTimeout = 150000000ull;
static Result g_connection_result;
static const char* g_connection_stage = "未连接";
#ifndef NCM_CLIENT_LOG_NAME
#define NCM_CLIENT_LOG_NAME "ipc-client-app.log"
#endif
#ifndef CLOUDTUNE_VERSION
#define CLOUDTUNE_VERSION "development"
#endif
static size_t g_trace_bytes;
static bool g_trace_started;
static Result g_trace_result;
static const char* g_trace_stage;
/* Only lifecycle changes and controls, never poll payloads, song URLs or
 * account data. Close each write so a removed SD contains complete entries. */
static void traceClient(const char* stage, Result result, u32 command) {
    if (command == UINT32_MAX && g_trace_stage &&
        !strcmp(g_trace_stage, stage) && g_trace_result == result) return;
    if (g_trace_bytes >= 32768) return;
    char line[256];
    const int length = command == UINT32_MAX
        ? snprintf(line, sizeof(line), "%s: 0x%08X\n", stage, result)
        : snprintf(line, sizeof(line), "%s command=%u: 0x%08X\n", stage, command, result);
    if (length <= 0 || (size_t)length >= sizeof(line)) return;
    FILE* log = fopen(NCM_CONFIG_DIRECTORY "/" NCM_CLIENT_LOG_NAME,
                      g_trace_started ? "a" : "w");
    if (!log) return;
    if (!g_trace_started) {
        const int count = fprintf(log, "CloudTune NX %s client\n", CLOUDTUNE_VERSION);
        if (count > 0) g_trace_bytes += (size_t)count;
        g_trace_started = true;
    }
    if ((size_t)length <= 32768 - g_trace_bytes)
        g_trace_bytes += fwrite(line, 1, (size_t)length, log);
    fclose(log);
    g_trace_stage = stage;
    g_trace_result = result;
}
static Result busy(void) { return MAKERESULT(Module_Libnx, LibnxError_Timeout); }
static bool enter(void) { return !atomic_flag_test_and_set_explicit(&g_busy, memory_order_acquire); }
static void leave(void) { atomic_flag_clear_explicit(&g_busy, memory_order_release); }
static void closeService(void) {
    // serviceClose sends a synchronous CMIF Close. A hung server cannot be
    // trusted to acknowledge it; releasing our kernel handle is sufficient.
    if (g_service) ncmIpcCloseSession(g_service);
    g_service = INVALID_HANDLE;
}
bool ncmPlaybackEnvironmentSupported(void) {
    if (hosversionIsAtmosphere()) return true;
    /* Atmosphere's read-only MesosphereMeta/KernelVersion query creates no
     * handles. Stock kernels reject the enum; unlike SM extension probing it
     * cannot defer indefinitely for a nonexistent service. Old nx-ovlloader
     * supplies HOS version but not HBABI's optional ATMOSPHR magic. */
    u64 version = 0;
    return R_SUCCEEDED(svcGetInfo(&version, (InfoType)65000, INVALID_HANDLE, 0)) && version != 0;
}
Result ncmPlaybackInitialize(void) {
    if (!enter()) return busy();
    Result rc = 0;
    if (g_service) goto done;
    g_connection_stage = "检查环境";
    if (!ncmPlaybackEnvironmentSupported()) {
        rc = MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer); goto done;
    }
    g_connection_stage = "等待上次请求结束";
    if (!ncmIpcIdle()) { rc = busy(); goto done; }
    const SmServiceName name = smEncodeName(NCM_SERVICE_NAME);
    u8 present = 0;
    g_connection_stage = "查找服务";
    rc = tipcDispatchInOut(smGetServiceSessionTipc(), 65100, name, present);
    if (R_FAILED(rc)) goto done;
    if (!present) { rc = MAKERESULT(Module_Libnx, LibnxError_NotFound); goto done; }
    // Like TriPlayer/libnx, use the already registered SM connection.
    // Application mode allows only one directly created session, consumed
    // by libnx at startup. A second ConnectToNamedPort fails with 0x10801.
    // Borrow this handle; only the returned playback session belongs to us.
    g_connection_stage = "获取后台会话";
    rc = ncmIpcRequest(smGetServiceSession()->session, 1, &name, sizeof(name),
                       NULL, 0, &g_service, false, NormalTimeout, NULL);
    if (R_FAILED(rc)) goto done;
    u32 version = 0;
    g_connection_stage = "协议握手";
    rc = ncmIpcRequest(g_service, NcmCommand_Version, NULL, 0, &version,
                       sizeof(version), NULL, false, NormalTimeout, NULL);
    if (R_SUCCEEDED(rc) && version != NCM_PROTOCOL_VERSION) rc = MAKERESULT(421, 1);
done:
    if (R_FAILED(rc)) closeService();
    g_connection_result = rc;
    if (R_SUCCEEDED(rc)) g_connection_stage = "已连接";
    traceClient(g_connection_stage, rc, UINT32_MAX);
    leave();
    return rc;
}
void ncmPlaybackConnectionMessage(char* output, size_t size) {
    if (!output || !size) return;
    if (!enter()) { snprintf(output, size, "后台通信忙，请稍后重试"); return; }
    const Result result = g_connection_result;
    const char* stage = g_connection_stage;
    leave();
    snprintf(output, size, "%s：0x%08X", stage, result);
    if (result != MAKERESULT(Module_Libnx, LibnxError_NotFound)) return;
    /* Bounded read; never inspect cookies/settings or infer a running process
     * from old log files. Only explain a completed, failed startup attempt. */
    FILE* file = fopen(NCM_CONFIG_DIRECTORY "/background-startup.log", "r");
    if (!file) return;
    char line[192];
    unsigned gate = 0, exited = 0;
    long long free_bytes = -1;
    size_t read = 0;
    while (read < 64u * 1024u && fgets(line, sizeof(line), file)) {
        read += strlen(line);
        long long used, limit, available;
        if (sscanf(line, "resource memory: used=%lld limit=%lld free=%lld", &used, &limit, &available) == 3 &&
            used >= 0 && limit >= used && available == limit - used) free_bytes = available;
        sscanf(line, "boot-resource-gate: 0x%x", &gate);
        sscanf(line, "startup-exit: 0x%x", &exited);
    }
    fclose(file);
    if (gate && exited && free_bytes >= 0 && free_bytes < 12ll * 1024 * 1024)
        snprintf(output, size, "上次后台启动内存不足（剩余 %lld KiB）；请关闭不需要的常驻模块", free_bytes / 1024);
}
void ncmPlaybackExit(void) {
    if (!enter()) return;
    closeService();
    ncmIpcIdle();
    leave();
}
void ncmPlaybackShutdown(void) {
    ncmPlaybackExit();
    ncmIpcShutdown();
}
bool ncmPlaybackIsActive(void) {
    if (!enter()) return false;
    const bool active = g_service != INVALID_HANDLE;
    leave();
    return active;
}
static Result exchange(u32 command, const void* input, size_t input_size,
                       void* output, size_t output_size) {
    if (output && output_size) memset(output, 0, output_size);
    if (!enter()) return busy();
    // Account publication must wait for the engine's five-second clear
    // barrier. Ordinary overlay controls always use the short deadline.
    const u64 timeout = command == NcmCommand_ClearQueue ? 5500000000ull : NormalTimeout;
    bool replied = false;
    Result result = ncmIpcRequest(g_service, command, input, input_size, output,
                                  output_size, NULL, false, timeout, &replied);
    // A queue-full/unsupported/tool error is a valid response. Keep its
    // connection so callers can query state or use supported music commands.
    if (R_FAILED(result) && !replied) closeService();
    if (R_FAILED(result)) {
        g_connection_result = result;
        g_connection_stage = command == NcmCommand_Status ? "读取后台状态" : "发送后台命令";
        traceClient(g_connection_stage, result, UINT32_MAX);
    }
    else if (command != NcmCommand_Status && command != NCM_COMMAND_AIRPLAY_STATUS)
        traceClient("命令已接受", result, command);
    leave();
    return result;
}
Result ncmPlaybackGetStatus(NcmPlaybackStatus* status) {
    if (!status) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    Result result = exchange(NcmCommand_Status, NULL, 0, status, sizeof(*status));
    if (R_FAILED(result) || status->version != NCM_PROTOCOL_VERSION) {
        if (R_SUCCEEDED(result) && enter()) {
            closeService();
            g_connection_result = MAKERESULT(421, 1);
            g_connection_stage = "状态协议不兼容";
            leave();
        }
        memset(status, 0, sizeof(*status));
        return R_FAILED(result) ? result : MAKERESULT(421, 1);
    }
    status->title[sizeof(status->title)-1] = 0;
    status->artist[sizeof(status->artist)-1] = 0;
    status->message[sizeof(status->message)-1] = 0;
    return 0;
}
static Result simple(NcmCommand cmd) { return exchange(cmd, NULL, 0, NULL, 0); }
static Result value(NcmCommand cmd, uint32_t v) { return exchange(cmd, &v, sizeof(v), NULL, 0); }
Result ncmPlaybackLoadQueue(uint32_t i) { return value(NcmCommand_LoadQueue, i); }
Result ncmPlaybackPlayPause(void) { return simple(NcmCommand_PlayPause); }
Result ncmPlaybackPlay(void) { return simple(NcmCommand_Play); }
Result ncmPlaybackPause(void) { return simple(NcmCommand_Pause); }
Result ncmPlaybackNext(void) { return simple(NcmCommand_Next); }
Result ncmPlaybackPrevious(void) { return simple(NcmCommand_Previous); }
Result ncmPlaybackSetVolume(uint32_t v) { return value(NcmCommand_Volume, v); }
Result ncmPlaybackSeek(uint32_t v) { return value(NcmCommand_Seek, v); }
Result ncmPlaybackSetRepeat(NcmRepeatMode v) { return value(NcmCommand_Repeat, v); }
Result ncmPlaybackSetShuffle(bool v) { return value(NcmCommand_Shuffle, v ? 1u : 0u); }
Result ncmPlaybackStop(void) { return simple(NcmCommand_Stop); }
Result ncmPlaybackClearQueue(void) { return simple(NcmCommand_ClearQueue); }
Result ncmAirplayGetStatus(NcmAirplayStatus* status) {
    if (!status) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    memset(status, 0, sizeof(*status));
    Result result = exchange(NCM_COMMAND_AIRPLAY_STATUS, NULL, 0, status, sizeof(*status));
    if (R_FAILED(result) || status->version != NCM_AIRPLAY_VERSION) {
        memset(status, 0, sizeof(*status));
        return R_FAILED(result) ? result : MAKERESULT(421, 1);
    }
    status->name[sizeof(status->name)-1] = 0;
    status->address[sizeof(status->address)-1] = 0;
    status->title[sizeof(status->title)-1] = 0;
    status->artist[sizeof(status->artist)-1] = 0;
    status->message[sizeof(status->message)-1] = 0;
    return 0;
}
Result ncmAirplaySetEnabled(bool enabled) {
    const uint32_t value = enabled ? 1u : 0u;
    return exchange(NCM_COMMAND_AIRPLAY_ENABLED, &value, sizeof(value), NULL, 0);
}
Result ncmAirplayDisconnect(void) {
    return exchange(NCM_COMMAND_AIRPLAY_DISCONNECT, NULL, 0, NULL, 0);
}
Result ncmPlaybackLaunchBackground(void) {
    if (!ncmPlaybackEnvironmentSupported())
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);
    u64 existing = 0;
    Result rc = ncmPlaybackGetBackgroundProcessId(&existing);
    if (R_SUCCEEDED(rc)) return 0;
    /* Missing IPC is not evidence that the process is absent. Do not launch
     * a second instance when PM is unavailable or the boot gate is waiting. */
    if (rc != MAKERESULT(15, 1)) return rc;
    rc = pmshellInitialize();
    if (R_FAILED(rc)) return rc;
    u64 pid = 0;
    const NcmProgramLocation location = { .program_id = NCM_PROGRAM_ID, .storageID = NcmStorageId_None };
    rc = pmshellLaunchProgram(0, &location, &pid);
    pmshellExit();
    if (R_FAILED(rc) && R_SUCCEEDED(ncmPlaybackGetBackgroundProcessId(&existing))) return 0;
    return rc;
}
Result ncmPlaybackGetBackgroundProcessId(u64* pid) {
    if (!pid) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    *pid = 0;
    // A timed-out GetService may still be using the borrowed SM connection.
    // Do not enter another synchronous SM/PM lookup on the caller thread.
    if (ncmIpcPending()) return busy();
    if (!ncmPlaybackEnvironmentSupported()) return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);
    Result rc = pmdmntInitialize();
    if (R_FAILED(rc)) return rc;
    rc = pmdmntGetProcessId(pid, NCM_PROGRAM_ID);
    pmdmntExit();
    if (R_SUCCEEDED(rc) && !*pid) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    return rc;
}
