/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../engine.hpp"
#include "ipc_server.h"
#include <switch.h>
#include <curl/curl.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sys/stat.h>

namespace {
constexpr SocketInitConfig Sockets = {
    .tcp_tx_buf_size=0x800, .tcp_rx_buf_size=0x1000,
    .tcp_tx_buf_max_size=0x4000, .tcp_rx_buf_max_size=0x8000,
    .udp_tx_buf_size=0, .udp_rx_buf_size=0, .sb_efficiency=2,
    .num_bsd_sessions=2, .bsd_service_type=BsdServiceType_User,
};
Result socketResult;
void check(Result rc) { if (R_FAILED(rc)) diagAbortWithResult(rc); }
Result dispatch(void* context, const NcmIpcServerRequest* request, uint8_t* output, size_t* size) {
    auto* engine = static_cast<netease::playback::Engine*>(context);
    if (request->command_id == NcmCommand_Version) {
        const uint32_t version=NCM_PROTOCOL_VERSION;
        std::memcpy(output,&version,sizeof(version)); *size=sizeof(version); return 0;
    }
    if (request->command_id == NcmCommand_Ping) return 0;
    if (request->command_id == NcmCommand_Status) {
        auto snapshot=engine->status();
        std::memcpy(output,&snapshot,sizeof(snapshot)); *size=sizeof(snapshot); return 0;
    }
    uint32_t argument=0;
    const auto command=static_cast<NcmCommand>(request->command_id);
    bool hasArgument=command==NcmCommand_LoadQueue || command==NcmCommand_Volume ||
        command==NcmCommand_Seek || command==NcmCommand_Repeat || command==NcmCommand_Shuffle;
    if (hasArgument) {
        if (request->data_size<sizeof(argument)) return MAKERESULT(Module_Libnx,LibnxError_BadInput);
        std::memcpy(&argument,request->data,sizeof(argument));
    }
    return engine->command(command,argument) ? 0 : MAKERESULT(Module_Libnx,LibnxError_BadInput);
}
}
extern "C" {
u32 __nx_applet_type=AppletType_None;
u32 __nx_fs_num_sessions=2;
void __libnx_init_time(void);
void __libnx_initheap(void) {
    /* API JSON, decoder, TLS and both worker stacks share this fixed budget.
     * Full audio is cached on SD, never retained in process memory. */
    static char heap[12*1024*1024];
    extern char* fake_heap_start;
    extern char* fake_heap_end;
    fake_heap_start=heap; fake_heap_end=heap+sizeof(heap);
}
void __appInit(void) {
    check(smInitialize());
    if (!hosversionGet()) {
        check(setsysInitialize());
        SetSysFirmwareVersion firmware{};
        check(setsysGetFirmwareVersion(&firmware));
        hosversionSet(MAKEHOSVERSION(firmware.major,firmware.minor,firmware.micro));
        setsysExit();
    }
    check(timeInitialize());
    // This process overrides the default libnx startup. Initialize realtime
    // explicitly so TLS certificate dates and signed API timestamps are valid.
    __libnx_init_time();
    check(fsInitialize()); check(fsdevMountSdmc());
    socketResult=socketInitialize(&Sockets);
}
void __appExit(void) {
    curl_global_cleanup();
    if (R_SUCCEEDED(socketResult)) socketExit();
    fsdevUnmountDevice("sdmc"); fsExit(); timeExit(); smExit();
}
}
int main(int,char**) {
    mkdir("sdmc:/config",0755); mkdir(NCM_CONFIG_DIRECTORY,0755);
    check(socketResult);
    if (curl_global_init(CURL_GLOBAL_DEFAULT)) return 1;
    auto engine=std::make_unique<netease::playback::Engine>(
        netease::playback::EngineConfig{NCM_CONFIG_DIRECTORY,NCM_CONFIG_DIRECTORY "/cacert.pem"});
    if (!engine->start()) return 1;
    NcmIpcServer server{};
    check(ncmIpcServerInitialize(&server,NCM_SERVICE_NAME,8));
    FILE* ready=std::fopen(NCM_CONFIG_DIRECTORY "/background-ready.log","w");
    if (ready) { std::fputs("NetEase Switch background ready; protocol 1\n",ready); std::fclose(ready); }
    while (true) {
        Result rc=ncmIpcServerProcess(&server,dispatch,engine.get());
        if (R_FAILED(rc) && rc!=KERNELRESULT(ConnectionClosed)) svcSleepThread(10000000);
    }
}
