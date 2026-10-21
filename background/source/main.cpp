/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../runtime.hpp"
#include "ipc_server.h"
#include "startup.h"
#include "heap.h"
#include <switch.h>
#include <curl/curl.h>
#include <cstdio>
#include <cstring>
#include <memory>

namespace {
static_assert(sizeof(NcmPlaybackStatus) <= NCM_IPC_RESPONSE_MAX_DATA_SIZE,
              "Playback status exceeds CMIF inline reply");
static_assert(sizeof(NcmAirplayStatus) <= NCM_IPC_RESPONSE_MAX_DATA_SIZE,
              "AirPlay status exceeds CMIF inline reply");
Result dispatchRequest(void* context, const NcmIpcServerRequest* request, uint8_t* output, size_t* size) {
    auto* engine = static_cast<netease::playback::Runtime*>(context);
    if (request->command_id == NcmCommand_Version) {
        const uint32_t version=NCM_PROTOCOL_VERSION;
        std::memcpy(output,&version,sizeof(version)); *size=sizeof(version); return 0;
    }
    if (request->command_id == NcmCommand_Ping) return 0;
    if (request->command_id == NcmCommand_Status) {
        auto snapshot=engine->status();
        std::memcpy(output,&snapshot,sizeof(snapshot)); *size=sizeof(snapshot); return 0;
    }
    if (request->command_id == NCM_COMMAND_AIRPLAY_STATUS) {
        const auto snapshot = engine->airplay_status();
        std::memcpy(output, &snapshot, sizeof(snapshot)); *size=sizeof(snapshot); return 0;
    }
    if (request->command_id == NCM_COMMAND_AIRPLAY_ENABLED) {
        uint32_t enabled=0;
        if (request->data_size<sizeof(enabled))
            return MAKERESULT(Module_Libnx,LibnxError_BadInput);
        std::memcpy(&enabled,request->data,sizeof(enabled));
        if (enabled>1) return MAKERESULT(Module_Libnx,LibnxError_BadInput);
        try {
            return engine->set_airplay_enabled(enabled!=0) ? 0
                : MAKERESULT(Module_Libnx,LibnxError_IoError);
        } catch (...) {
            return MAKERESULT(Module_Libnx,LibnxError_IoError);
        }
    }
    if (request->command_id == NCM_COMMAND_AIRPLAY_DISCONNECT) {
        try {
            return engine->disconnect_airplay() ? 0
                : MAKERESULT(Module_Libnx,LibnxError_Timeout);
        } catch (...) {
            return MAKERESULT(Module_Libnx,LibnxError_IoError);
        }
    }
    if (request->command_id < NcmCommand_LoadQueue ||
        request->command_id > NcmCommand_ClearQueue)
        return MAKERESULT(Module_Libnx,LibnxError_BadInput);
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
Result dispatch(void* context, const NcmIpcServerRequest* request, uint8_t* output, size_t* size) noexcept {
    // Exceptions on the service thread must become replies, never escape the
    // C IPC loop and terminate the resident player.
    try { return dispatchRequest(context, request, output, size); }
    catch (const std::bad_alloc&) {
        *size=0;
        return MAKERESULT(Module_Libnx,LibnxError_OutOfMemory);
    } catch (...) {
        *size=0;
        return MAKERESULT(Module_Libnx,LibnxError_IoError);
    }
}
}
namespace {
struct CurlLifetime {
    CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT);
    ~CurlLifetime() { if (result == CURLE_OK) curl_global_cleanup(); }
};
int run() {
    if (R_FAILED(ncmStartupResult())) return 1;
    CurlLifetime curl;
    ncmStartupRecord("curl", static_cast<Result>(curl.result));
    if (curl.result != CURLE_OK) return 1;
    auto engine = std::make_unique<netease::playback::Runtime>(
        netease::playback::EngineConfig{NCM_CONFIG_DIRECTORY, NCM_CONFIG_DIRECTORY "/cacert.pem"});
    const bool started = engine->start();
    ncmStartupRecord("heap-last-result", ncmHeapLastResult());
    ncmStartupRecord("heap-committed-bytes", static_cast<Result>(ncmHeapCommittedSize()));
    ncmStartupRecord("runtime", started ? 0 : MAKERESULT(Module_Libnx, LibnxError_IoError));
    if (!started) return 1;
    NcmIpcServer server{};
    Result result = ncmIpcServerInitialize(&server, NCM_SERVICE_NAME, 8);
    ncmStartupRecord("ipc-register", result);
    if (R_FAILED(result)) return 1;
    FILE* ready = std::fopen(NCM_CONFIG_DIRECTORY "/background-ready.log", "w");
    if (ready) { std::fputs("CloudTune NX background ready; protocol 1\n", ready); std::fclose(ready); }
    ncmStartupRecord("ready", 0);
    while (true) {
        Result rc = ncmIpcServerProcess(&server, dispatch, engine.get());
        if (R_FAILED(rc) && rc != KERNELRESULT(ConnectionClosed)) svcSleepThread(10000000);
    }
}
}
int main(int, char**) {
    try { return run(); }
    catch (const std::bad_alloc&) {
        ncmStartupRecord("heap-last-result", ncmHeapLastResult());
        ncmStartupRecord("runtime-allocation", MAKERESULT(Module_Libnx, LibnxError_OutOfMemory));
    } catch (...) {
        ncmStartupRecord("runtime-exception", MAKERESULT(Module_Libnx, LibnxError_IoError));
    }
    return 1;
}
