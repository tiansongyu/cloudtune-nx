// SPDX-License-Identifier: GPL-3.0-or-later
// Excluded probe, never shipped: the actual service dispatcher/Runtime and
// actual IPC client run on separate threads. Fixtures are generated tones.
// This does not model separate process memory pools or a physical console.
#include "ipc_server.h"
#include "netease/playback_client.h"
#include <cstdio>

namespace {
FILE *log;
void check(bool ok, const char *stage) {
    std::fprintf(log, "%s: %s\n", ok ? "PASS" : "FAIL", stage);
    std::fflush(log);
    if (!ok) {
        char message[256];
        ncmPlaybackConnectionMessage(message, sizeof(message));
        std::fprintf(log, "%s\nRESULT: FAIL\n", message);
        std::fclose(log);
        svcExitProcess();
    }
}
NcmPlaybackStatus waitFor(NcmPlaybackState state, unsigned index, unsigned position = 0) {
    NcmPlaybackStatus status{};
    const u64 deadline = armGetSystemTick() + armNsToTicks(60000000000ull);
    while (armGetSystemTick() < deadline) {
        check(R_SUCCEEDED(ncmPlaybackGetStatus(&status)), "IPC status");
        if (status.state == NcmPlayback_Error) {
            std::fprintf(log, "engine error=%08x message=%s\n", status.error_code, status.message);
            check(false, "real engine playback");
        }
        if (status.state == state && status.queue_index == index && status.position_ms >= position)
            return status;
        svcSleepThread(100000000);
    }
    check(false, "playback deadline");
    return status;
}
void client(void *) {
    log = std::fopen(NCM_CONFIG_DIRECTORY "/handoff-probe.log", "w");
    if (!log)
        svcExitProcess();
    u64 value = 0;
    check(R_SUCCEEDED(svcGetInfo(&value, InfoType_ResourceLimit, INVALID_HANDLE, 0)),
          "quota handle");
    const Handle quota = static_cast<Handle>(value);
    s64 used = 0, limit = 0;
    check(R_SUCCEEDED(svcGetResourceLimitCurrentValue(&used, quota, LimitableResource_Sessions)) &&
              R_SUCCEEDED(svcGetResourceLimitLimitValue(&limit, quota, LimitableResource_Sessions)),
          "session creation quota");
    check(R_SUCCEEDED(svcSetResourceLimitLimitValue(quota, LimitableResource_Sessions, used)),
          "no additional client session slot");
    check(R_SUCCEEDED(ncmPlaybackInitialize()),
          "production handshake with exhausted creation quota");
    // The service and client share a process only in this probe. Restore its
    // quota so the real background work guard can retain eight free sessions.
    check(R_SUCCEEDED(svcSetResourceLimitLimitValue(quota, LimitableResource_Sessions, limit)),
          "restore service quota before real playback");
    svcCloseHandle(quota);
    check(R_SUCCEEDED(ncmPlaybackLoadQueue(0)), "handoff generated queue through IPC");
    waitFor(NcmPlayback_Playing, 0, 1000);
    check(true, "MP3 decoded and emitted after IPC handoff");
    Service overlay;
    check(R_SUCCEEDED(smGetService(&overlay, NCM_SERVICE_NAME)), "overlay client acquisition");
    check(R_SUCCEEDED(serviceDispatch(&overlay, NcmCommand_Pause)), "overlay pause command");
    const auto paused = waitFor(NcmPlayback_Paused, 0);
    svcSleepThread(300000000);
    NcmPlaybackStatus status{};
    check(R_SUCCEEDED(ncmPlaybackGetStatus(&status)) && status.position_ms == paused.position_ms,
          "frontend observes paused position");
    serviceClose(&overlay);
    check(R_SUCCEEDED(ncmPlaybackPlay()), "frontend resume after overlay closes");
    waitFor(NcmPlayback_Playing, 0, paused.position_ms + 300);
    for (unsigned index = 1; index <= 2; ++index) {
        check(R_SUCCEEDED(ncmPlaybackNext()), "IPC next track");
        waitFor(NcmPlayback_Playing, index, 1000);
        check(true, index == 1 ? "AAC decoded and emitted" : "FLAC decoded and emitted");
    }
    check(R_SUCCEEDED(ncmPlaybackClearQueue()), "acknowledged account-clear barrier");
    check(R_SUCCEEDED(ncmPlaybackGetStatus(&status)) && status.queue_count == 0,
          "queue cleared in authoritative service state");
    ncmPlaybackShutdown();
    check(R_SUCCEEDED(ncmPlaybackInitialize()), "reconnect after frontend shutdown");
    ncmPlaybackShutdown();
    std::fputs("RESULT: PASS\n", log);
    std::fclose(log);
    svcExitProcess();
}
} // namespace

extern "C" Result __real_ncmIpcServerProcess(NcmIpcServer *, NcmIpcServerHandler, void *);
extern "C" Result __wrap_ncmIpcServerProcess(NcmIpcServer *server, NcmIpcServerHandler handler,
                                             void *context) {
    static bool started = false;
    static Thread probe;
    if (!started) {
        if (R_FAILED(threadCreate(&probe, client, nullptr, nullptr, 0x10000, 44, -2)) ||
            R_FAILED(threadStart(&probe)))
            svcExitProcess();
        started = true;
    }
    return __real_ncmIpcServerProcess(server, handler, context);
}
