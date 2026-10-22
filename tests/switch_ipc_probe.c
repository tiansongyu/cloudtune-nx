/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Excluded native IPC regression probe. Never install as a sysmodule.
 * Eden lacks Atmosphere HasService, so only presence/CFW detection is
 * substituted. SM registration/acquisition, threads, TLS, transport, server
 * dispatch and all reply parsing execute through the actual libnx calls. */
#include <switch.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include "netease/playback_client.h"
#include "ipc_server.h"
#undef tipcDispatchInOut
#define tipcDispatchInOut(service, command, input, output) ((output)=1, (Result)0)
#define hosversionIsAtmosphere() true
#include "../background/client/playback_client.c"
u32 __nx_applet_type = AppletType_None;
u64 __nx_heap_size = 2u * 1024u * 1024u;
static NcmIpcServer server;
static NcmPlaybackStatus snapshot = {.version=NCM_PROTOCOL_VERSION, .volume_permille=700};
static atomic_bool slow_once;
static Result dispatch(void* context, const NcmIpcServerRequest* r, u8* out, size_t* size) {
    (void)context;
    if (r->command_id == NcmCommand_Version) {
        const u32 version=NCM_PROTOCOL_VERSION; memcpy(out,&version,4); *size=4;
    } else if(r->command_id==NcmCommand_Status) {
        memcpy(out,&snapshot,sizeof(snapshot)); *size=sizeof(snapshot);
    } else if(r->command_id==NcmCommand_Pause) {
        if (atomic_exchange(&slow_once,false)) svcSleepThread(300000000);
        snapshot.state = NcmPlayback_Paused;
    } else if(r->command_id==NcmCommand_Volume) {
        u32 volume=0;
        if(r->data_size<4) return MAKERESULT(Module_Libnx,LibnxError_BadInput);
        memcpy(&volume,r->data,4);
        if(volume>1000) return MAKERESULT(Module_Libnx,LibnxError_BadInput);
        snapshot.volume_permille=volume;
    } else return MAKERESULT(Module_Libnx,LibnxError_BadInput);
    return 0;
}
static void server_main(void* unused) {
    (void)unused;
    while (1) ncmIpcServerProcess(&server,dispatch,NULL);
}
void __appInit(void) { smInitialize(); hosversionSet(MAKEHOSVERSION(20,4,0)); fsInitialize(); fsdevMountSdmc(); }
void __appExit(void) { fsdevUnmountAll(); fsExit(); smExit(); }
int main(void) {
    mkdir("sdmc:/config",0777);mkdir(NCM_CONFIG_DIRECTORY,0777);
    FILE* log=fopen(NCM_CONFIG_DIRECTORY "/native-ipc-probe.log","w");
    if (!log) return 1;
    unsigned failures=0;
    Handle resource_limit=INVALID_HANDLE;
    s64 previous_session_limit=0, initial_sessions=0;
#define CHECK(condition,stage) do { const bool ok=(condition); fprintf(log,"%s: %s\n", ok?"PASS":"FAIL",stage); fflush(log); if(!ok) {++failures;goto done;} } while(0)
    CHECK(R_SUCCEEDED(ncmIpcServerInitialize(&server,NCM_SERVICE_NAME,8)),"SM service registration");
    Thread thread;
    CHECK(R_SUCCEEDED(threadCreate(&thread,server_main,NULL,NULL,0x10000,44,-2)),"server thread creation");
    CHECK(R_SUCCEEDED(threadStart(&thread)),"server thread start");
    u64 resource_handle=0;
    CHECK(R_SUCCEEDED(svcGetInfo(&resource_handle,InfoType_ResourceLimit,INVALID_HANDLE,0)),
          "read native process resource-limit handle");
    resource_limit=(Handle)resource_handle;
    CHECK(R_SUCCEEDED(svcGetResourceLimitCurrentValue(&initial_sessions,resource_limit,LimitableResource_Sessions)) &&
          R_SUCCEEDED(svcGetResourceLimitLimitValue(&previous_session_limit,resource_limit,LimitableResource_Sessions)),
          "read native session creation quota");
    CHECK(R_SUCCEEDED(svcSetResourceLimitLimitValue(resource_limit,LimitableResource_Sessions,initial_sessions)),
          "exhaust additional session-creation slots like application mode");
    Handle extra=INVALID_HANDLE;
    const Result extra_result=svcConnectToNamedPort(&extra,"sm:");
    if (extra) svcCloseHandle(extra);
    CHECK(extra_result==MAKERESULT(Module_Kernel,132),
          "old private-SM path reproduces hardware 0x10801");
    CHECK(R_SUCCEEDED(ncmPlaybackInitialize()),"production client SM acquisition and version handshake");
    NcmPlaybackStatus status;
    CHECK(R_SUCCEEDED(ncmPlaybackGetStatus(&status)) && status.version==1 && status.volume_permille==700,
          "216-byte playback snapshot");
    CHECK(R_SUCCEEDED(ncmPlaybackPause()) && R_SUCCEEDED(ncmPlaybackGetStatus(&status)) &&
          status.state==NcmPlayback_Paused,"pause command and authoritative state");
    CHECK(R_FAILED(ncmPlaybackSetVolume(1001)) && ncmPlaybackIsActive(),"valid command rejection retains session");
    Service overlay;
    CHECK(R_SUCCEEDED(smGetService(&overlay,NCM_SERVICE_NAME)),"second overlay-like session");
    const u32 volume=420;
    CHECK(R_SUCCEEDED(serviceDispatchIn(&overlay,NcmCommand_Volume,volume)) &&
          R_SUCCEEDED(ncmPlaybackGetStatus(&status)) && status.volume_permille==420,
          "second client control is visible to first client");
    serviceClose(&overlay);
    CHECK(R_SUCCEEDED(ncmPlaybackGetStatus(&status)),"closing overlay preserves foreground session");
    atomic_store(&slow_once,true);
    const u64 start=armGetSystemTick();
    const Result timeout=ncmPlaybackPause();
    const u64 elapsed=armTicksToNs(armGetSystemTick()-start);
    CHECK(R_FAILED(timeout) && !ncmPlaybackIsActive() && elapsed<290000000,
          "slow server returns to caller before completing command");
    svcSleepThread(500000000);
    CHECK(R_SUCCEEDED(ncmPlaybackInitialize()) && R_SUCCEEDED(ncmPlaybackGetStatus(&status)),
          "late completion and reconnection");
    ncmPlaybackShutdown();
    CHECK(R_SUCCEEDED(ncmPlaybackInitialize()),"worker shutdown and recreation");
    ncmPlaybackShutdown();
    Service other;
    CHECK(R_SUCCEEDED(smGetService(&other,NCM_SERVICE_NAME)),
          "borrowed SM survives playback shutdown");
    serviceClose(&other);
done:
    if(resource_limit) {
        svcSetResourceLimitLimitValue(resource_limit,LimitableResource_Sessions,previous_session_limit);
        svcCloseHandle(resource_limit);
    }
    fprintf(log,"RESULT: %s\n",failures?"FAIL":"PASS");
    fclose(log);
    svcExitProcess();
}
