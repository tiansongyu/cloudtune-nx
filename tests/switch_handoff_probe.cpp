// SPDX-License-Identifier: GPL-3.0-or-later
// Excluded probe, never shipped: the actual service dispatcher/Runtime and
// actual IPC client run on separate threads. Fixtures are generated tones.
// This does not model separate process memory pools or a physical console.
#include "heap.h"
#include "ipc_server.h"
#include "netease/playback_client.h"
#include <atomic>
#include <cstdio>
#include <malloc.h>

namespace {
FILE *log;
unsigned assertions = 0;
std::atomic<bool> contend{false};
std::atomic<unsigned> appends{0}, outputOpens{0}, wrongPriority{0};
std::atomic<u64> longestGap{0};
std::atomic<size_t> failedAllocation{0};
std::atomic<int> failedAllocated{0}, failedFree{0};
u64 lastAppend = 0;
void check(bool ok, const char *stage) {
    ++assertions;
    if (!ok || assertions < 30) {
        std::fprintf(log, "%s: %s\n", ok ? "PASS" : "FAIL", stage);
        std::fflush(log);
    }
    if (!ok) {
        char message[256];
        ncmPlaybackConnectionMessage(message, sizeof(message));
        std::fprintf(log, "%s\nRESULT: FAIL\n", message);
        std::fclose(log);
        svcExitProcess();
    }
}
void contention(void *) {
    // Bounded synthetic system-core pressure, below the production audio and
    // receiver priorities. It cannot stand in for a physical game benchmark.
    volatile u64 value = 1;
    while (contend) {
        const auto until = armGetSystemTick() + armNsToTicks(150000000);
        while (contend && armGetSystemTick() < until)
            for (unsigned i = 0; i < 1024; ++i)
                value = value * 1664525 + 1013904223;
        svcSleepThread(20000000);
    }
}
NcmPlaybackStatus waitFor(NcmPlaybackState state, unsigned index, unsigned position = 0) {
    NcmPlaybackStatus status{};
    const u64 deadline = armGetSystemTick() + armNsToTicks(60000000000ull);
    while (armGetSystemTick() < deadline) {
        check(R_SUCCEEDED(ncmPlaybackGetStatus(&status)), "IPC status");
        if (status.state == NcmPlayback_Error) {
            std::fprintf(log, "engine error=%08x index=%u allocated=%d free=%d message=%s\n",
                         status.error_code, status.queue_index, mallinfo().uordblks,
                         mallinfo().fordblks, status.message);
            std::fprintf(log, "allocation failure bytes=%zu allocated=%d free=%d\n",
                         failedAllocation.load(), failedAllocated.load(), failedFree.load());
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
    // Let the throttled generated fixtures finish caching before measuring
    // continuous transitions. Cold download behavior is a separate probe.
    waitFor(NcmPlayback_Playing, 0, 10000);
    check(true, "MP3 decoded and cached after IPC handoff");
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
        waitFor(NcmPlayback_Playing, index, 10000);
        check(true, index == 1 ? "AAC decoded and emitted" : "FLAC decoded and emitted");
    }
    check(R_SUCCEEDED(ncmPlaybackStop()), "stop before memory baseline");
    waitFor(NcmPlayback_Stopped, 2);
    const auto memoryBefore = mallinfo().uordblks;
    check(ncmHeapCommittedSize() <= 2 * 1024 * 1024, "stress runs within compact heap");
    for (unsigned cycle = 0; cycle < 30; ++cycle) {
        const unsigned index = cycle % 3;
        check(R_SUCCEEDED(ncmPlaybackLoadQueue(index)), "stress load");
        waitFor(NcmPlayback_Playing, index, 200);
        check(R_SUCCEEDED(smGetService(&overlay, NCM_SERVICE_NAME)), "stress overlay connect");
        check(R_SUCCEEDED(serviceDispatch(&overlay, NcmCommand_Pause)), "stress overlay pause");
        const auto held = waitFor(NcmPlayback_Paused, index);
        svcSleepThread(30000000);
        check(R_SUCCEEDED(ncmPlaybackGetStatus(&status)) && status.position_ms == held.position_ms,
              "stress paused cursor");
        check(R_SUCCEEDED(ncmPlaybackPlay()), "stress resume");
        waitFor(NcmPlayback_Playing, index, held.position_ms + 100);
        check(R_SUCCEEDED(ncmPlaybackNext()), "stress next");
        waitFor(NcmPlayback_Playing, (index + 1) % 3, 100);
        check(R_SUCCEEDED(ncmPlaybackPrevious()), "stress previous");
        waitFor(NcmPlayback_Playing, index, 100);
        check(R_SUCCEEDED(ncmPlaybackSeek(1000)), "stress seek");
        waitFor(NcmPlayback_Playing, index, 1000);
        check(R_SUCCEEDED(ncmPlaybackPlayPause()), "stress toggle pause");
        waitFor(NcmPlayback_Paused, index);
        check(R_SUCCEEDED(ncmPlaybackPlayPause()), "stress toggle resume");
        waitFor(NcmPlayback_Playing, index, 1100);
        check(R_SUCCEEDED(ncmPlaybackSetRepeat(NcmRepeat_All)), "stress repeat");
        check(R_SUCCEEDED(ncmPlaybackSetShuffle(false)), "stress shuffle");
        check(R_SUCCEEDED(ncmPlaybackStop()), "stress stop");
        waitFor(NcmPlayback_Stopped, index);
        serviceClose(&overlay);
        std::fprintf(log, "PASS: transport cycle %u; allocated=%d\n", cycle + 1,
                     mallinfo().uordblks);
        std::fflush(log);
    }
    check(R_SUCCEEDED(ncmPlaybackLoadQueue(0)), "load before control flood");
    waitFor(NcmPlayback_Playing, 0, 200);
    for (unsigned i = 0; i < 3000; ++i) {
        check(R_SUCCEEDED(ncmPlaybackSetVolume(500 + i % 400)), "volume flood");
        check(R_SUCCEEDED(ncmPlaybackGetStatus(&status)) && status.state != NcmPlayback_Error,
              "status during volume flood");
        svcSleepThread(1000000);
    }
    waitFor(NcmPlayback_Playing, 0, 1000);
    check(R_SUCCEEDED(ncmPlaybackSetVolume(700)), "restore volume");
    check(R_SUCCEEDED(ncmPlaybackSetRepeat(NcmRepeat_All)), "continuous queue repeat");
    // A same-index LoadQueue is asynchronous: an old Playing snapshot must
    // not be mistaken for completion of the new load when recording counters.
    check(R_SUCCEEDED(ncmPlaybackStop()), "stop before continuous baseline");
    waitFor(NcmPlayback_Stopped, 0);
    check(R_SUCCEEDED(ncmPlaybackLoadQueue(0)), "continuous queue load");
    waitFor(NcmPlayback_Playing, 0, 500);
    const auto opensBefore = outputOpens.load();
    longestGap = 0;
    Thread load;
    contend = true;
    check(R_SUCCEEDED(threadCreate(&load, contention, nullptr, nullptr, 0x4000, 55, 3)) &&
              R_SUCCEEDED(threadStart(&load)),
          "system-core contention thread");
    for (unsigned transition = 1; transition <= 4; ++transition) {
        waitFor(NcmPlayback_Playing, transition % 3, 500);
        std::fprintf(log, "PASS: automatic queue transition %u under contention\n", transition);
        std::fflush(log);
    }
    contend = false;
    threadWaitForExit(&load);
    threadClose(&load);
    std::fprintf(log, "CONTINUOUS: opens_before=%u opens_after=%u gap_ms=%llu\n", opensBefore,
                 outputOpens.load(), (unsigned long long)(armTicksToNs(longestGap) / 1000000));
    check(outputOpens == opensBefore, "automatic transitions reuse audout");
    check(!wrongPriority && appends > 500, "audio worker priority and real PCM submission");
    const auto gapMs = armTicksToNs(longestGap) / 1000000;
    check(gapMs < 200, "cached PCM submission gap stays inside 256 ms buffer");
    check(R_SUCCEEDED(ncmPlaybackStop()), "stop after contention");
    waitFor(NcmPlayback_Stopped, 1);
    check(mallinfo().uordblks < memoryBefore + 64 * 1024,
          "bounded memory after repeated transports");
    std::fprintf(log,
                 "METRICS: assertions=%u appends=%u opens=%u longest_gap_ms=%llu allocated=%d\n",
                 assertions, appends.load(), outputOpens.load(), (unsigned long long)gapMs,
                 mallinfo().uordblks);
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

extern "C" void *__real_malloc(size_t);
extern "C" void *__wrap_malloc(size_t bytes) {
    void *value = __real_malloc(bytes);
    if (!value) {
        const auto info = mallinfo();
        failedAllocation = bytes;
        failedAllocated = info.uordblks;
        failedFree = info.fordblks;
    }
    return value;
}
extern "C" void *__real_memalign(size_t, size_t);
extern "C" void *__wrap_memalign(size_t alignment, size_t bytes) {
    void *value = __real_memalign(alignment, bytes);
    if (!value) {
        const auto info = mallinfo();
        failedAllocation = bytes;
        failedAllocated = info.uordblks;
        failedFree = info.fordblks;
    }
    return value;
}

extern "C" Result __real_audoutStartAudioOut(void);
extern "C" Result __wrap_audoutStartAudioOut(void) {
    ++outputOpens;
    lastAppend = 0;
    return __real_audoutStartAudioOut();
}
extern "C" Result __real_audoutAppendAudioOutBuffer(AudioOutBuffer *);
extern "C" Result __wrap_audoutAppendAudioOutBuffer(AudioOutBuffer *buffer) {
    s32 priority = 0;
    if (R_FAILED(svcGetThreadPriority(&priority, CUR_THREAD_HANDLE)) || priority != 49)
        ++wrongPriority;
    const auto now = armGetSystemTick();
    if (lastAppend && now - lastAppend > longestGap)
        longestGap = now - lastAppend;
    lastAppend = now;
    ++appends;
    return __real_audoutAppendAudioOutBuffer(buffer);
}

// Select the same compact policy as the supplied physical-console snapshot.
// The real allocator remains capped at 2 MiB. Do not recurse into its locking
// committed-size accessor from the resource query used by heap growth.
static size_t committedHeap = 0;
extern "C" Result __real_svcSetHeapSize(void **, u64);
extern "C" Result __wrap_svcSetHeapSize(void **out, u64 bytes) {
    const auto result = __real_svcSetHeapSize(out, bytes);
    if (R_SUCCEEDED(result))
        committedHeap = bytes;
    return result;
}
extern "C" Result __real_svcGetResourceLimitCurrentValue(s64 *, Handle, LimitableResource);
extern "C" Result __wrap_svcGetResourceLimitCurrentValue(s64 *out, Handle handle,
                                                         LimitableResource kind) {
    if (kind != LimitableResource_Memory)
        return __real_svcGetResourceLimitCurrentValue(out, handle, kind);
    *out = 322772992 + static_cast<s64>(committedHeap);
    return 0;
}
extern "C" Result __real_svcGetResourceLimitLimitValue(s64 *, Handle, LimitableResource);
extern "C" Result __wrap_svcGetResourceLimitLimitValue(s64 *out, Handle handle,
                                                       LimitableResource kind) {
    if (kind != LimitableResource_Memory)
        return __real_svcGetResourceLimitLimitValue(out, handle, kind);
    *out = 328204288;
    return 0;
}

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
