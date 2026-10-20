/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "netease/playback_client.h"
#include <string.h>
#include "timed_ipc.h"
#include <stdatomic.h>
static Handle g_service;
static atomic_flag g_busy = ATOMIC_FLAG_INIT;
static const u64 NormalTimeout = 150000000ull;
static Result busy(void) { return MAKERESULT(Module_Libnx, LibnxError_Timeout); }
static bool enter(void) { return !atomic_flag_test_and_set_explicit(&g_busy, memory_order_acquire); }
static void leave(void) { atomic_flag_clear_explicit(&g_busy, memory_order_release); }
static void closeService(void) {
    // serviceClose sends a synchronous CMIF Close. A hung server cannot be
    // trusted to acknowledge it; releasing our kernel handle is sufficient.
    if (g_service) svcCloseHandle(g_service);
    g_service = INVALID_HANDLE;
}
Result ncmPlaybackInitialize(void) {
    if (!enter()) return busy();
    Result rc = 0;
    Handle sm = INVALID_HANDLE;
    if (g_service) goto done;
    if (!hosversionIsAtmosphere()) {
        rc = MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer); goto done;
    }
    if (!ncmIpcIdle()) { rc = busy(); goto done; }
    const SmServiceName name = smEncodeName(NCM_SERVICE_NAME);
    u8 present = 0;
    rc = tipcDispatchInOut(smGetServiceSessionTipc(), 65100, name, present);
    if (R_FAILED(rc)) goto done;
    if (!present) { rc = MAKERESULT(Module_Libnx, LibnxError_NotFound); goto done; }
    // Use a private SM connection. Missing-service deferral and the race
    // between HasService/GetService now have the same bounded deadline.
    rc = svcConnectToNamedPort(&sm, "sm:");
    if (R_FAILED(rc)) goto done;
    const u64 pid = 0;
    rc = ncmIpcRequest(sm, 0, &pid, sizeof(pid), NULL, 0, NULL, true, NormalTimeout, NULL);
    if (R_FAILED(rc)) goto done;
    rc = ncmIpcRequest(sm, 1, &name, sizeof(name), NULL, 0, &g_service, false, NormalTimeout, NULL);
    if (R_FAILED(rc)) goto done;
    u32 version = 0;
    rc = ncmIpcRequest(g_service, NcmCommand_Version, NULL, 0, &version,
                       sizeof(version), NULL, false, NormalTimeout, NULL);
    if (R_SUCCEEDED(rc) && version != NCM_PROTOCOL_VERSION) rc = MAKERESULT(421, 1);
done:
    if (sm) svcCloseHandle(sm);
    if (R_FAILED(rc)) closeService();
    leave();
    return rc;
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
    leave();
    return result;
}
Result ncmPlaybackGetStatus(NcmPlaybackStatus* status) {
    if (!status) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    Result result = exchange(NcmCommand_Status, NULL, 0, status, sizeof(*status));
    if (R_FAILED(result) || status->version != NCM_PROTOCOL_VERSION) {
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
    if (!hosversionIsAtmosphere())
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);
    Result rc = pmshellInitialize();
    if (R_FAILED(rc)) return rc;
    u64 pid = 0;
    const NcmProgramLocation location = { .program_id = NCM_PROGRAM_ID, .storageID = NcmStorageId_None };
    rc = pmshellLaunchProgram(0, &location, &pid);
    pmshellExit();
    return rc;
}
