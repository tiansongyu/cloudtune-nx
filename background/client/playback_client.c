/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "netease/playback_client.h"
#include <string.h>
static Service g_service;
Result ncmPlaybackInitialize(void) {
    if (serviceIsActive(&g_service)) return 0;
    /* GetService is deferred by Atmosphere when a custom service is absent.
     * Never request it speculatively: this would freeze app/overlay startup.
     * Also never send Atmosphere extension IPC to stock Horizon or emulators. */
    if (!hosversionIsAtmosphere())
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);
    const SmServiceName name = smEncodeName(NCM_SERVICE_NAME);
    u8 present = 0;
    Result rc = tipcDispatchInOut(smGetServiceSessionTipc(), 65100, name, present);
    if (R_FAILED(rc)) return rc;
    if (!present) return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    rc = smGetService(&g_service, NCM_SERVICE_NAME);
    if (R_FAILED(rc)) return rc;
    uint32_t version = 0;
    rc = serviceDispatchOut(&g_service, NcmCommand_Version, version);
    if (R_FAILED(rc) || version != NCM_PROTOCOL_VERSION) {
        serviceClose(&g_service);
        return R_FAILED(rc) ? rc : MAKERESULT(421, 1);
    }
    return 0;
}
void ncmPlaybackExit(void) { serviceClose(&g_service); }
bool ncmPlaybackIsActive(void) { return serviceIsActive(&g_service); }
Result ncmPlaybackGetStatus(NcmPlaybackStatus* status) {
    if (!status) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    memset(status, 0, sizeof(*status));
    return serviceDispatchOut(&g_service, NcmCommand_Status, *status);
}
static Result simple(NcmCommand cmd) { return serviceDispatch(&g_service, cmd); }
static Result value(NcmCommand cmd, uint32_t v) { return serviceDispatchIn(&g_service, cmd, v); }
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
    Result result = serviceDispatchOut(&g_service, NCM_COMMAND_AIRPLAY_STATUS, *status);
    if (R_FAILED(result)) return result;
    if (status->version != NCM_AIRPLAY_VERSION) {
        memset(status, 0, sizeof(*status));
        return MAKERESULT(421, 1);
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
    return serviceDispatchIn(&g_service, NCM_COMMAND_AIRPLAY_ENABLED, value);
}
Result ncmAirplayDisconnect(void) {
    return serviceDispatch(&g_service, NCM_COMMAND_AIRPLAY_DISCONNECT);
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
