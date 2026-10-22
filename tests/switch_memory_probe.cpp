// SPDX-License-Identifier: GPL-3.0-or-later
// Excluded emulator probe: real runtime, decoders, SD and audout, hard 2 MiB
// heap ceiling. Fixtures contain only generated tones, never account data.
#include "heap.h"
#include "ipc_server.h"
#include "runtime.hpp"
#include "startup.h"
#include <cstdio>
#include <cstring>
#include <malloc.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

extern "C" Result __real_ncmCheckMemoryGrowth(size_t);
extern "C" Result __real_svcSetHeapSize(void **, u64);
static size_t committed;
extern "C" int __real_avformat_find_stream_info(AVFormatContext *, AVDictionary **);
extern "C" int __real_avcodec_open2(AVCodecContext *, const AVCodec *, AVDictionary **);
static void allocationCheckpoint(const char *stage, int result) {
    if (FILE *log = std::fopen(NCM_CONFIG_DIRECTORY "/memory-codec.log", "a")) {
        const auto info = mallinfo();
        std::fprintf(log, "%s result=%d allocated=%d free=%d\n", stage, result, info.uordblks,
                     info.fordblks);
        std::fclose(log);
    }
}
extern "C" int __wrap_avformat_find_stream_info(AVFormatContext *input, AVDictionary **options) {
    allocationCheckpoint("before-probe", 0);
    int result = __real_avformat_find_stream_info(input, options);
    allocationCheckpoint("after-probe", result);
    return result;
}
extern "C" int __wrap_avcodec_open2(AVCodecContext *codec, const AVCodec *decoder, AVDictionary **options) {
    allocationCheckpoint("before-codec", 0);
    int result = __real_avcodec_open2(codec, decoder, options);
    allocationCheckpoint("after-codec", result);
    return result;
}
extern "C" Result __wrap_svcSetHeapSize(void **out, u64 size) {
    const Result rc = __real_svcSetHeapSize(out, size);
    if (R_SUCCEEDED(rc))
        committed = size;
    return rc;
}
extern "C" Result __wrap_ncmCheckMemoryGrowth(size_t added) {
    // Called under heap_mutex: never call the locking committed-size getter.
    if (committed + added > 2u * 1024u * 1024u)
        return MAKERESULT(Module_Kernel, 132);
    return __real_ncmCheckMemoryGrowth(added);
}
extern "C" Result __wrap_ncmIpcServerProcess(NcmIpcServer *, NcmIpcServerHandler, void *context) {
    auto *runtime = static_cast<netease::playback::Runtime *>(context);
    static FILE *log = std::fopen(NCM_CONFIG_DIRECTORY "/memory-probe.log", "w");
    static unsigned stage = 0;
    static u64 deadline = 0;
    if (!log)
        svcExitProcess();
    auto finish = [&](bool ok, const char *reason) {
        const auto info = mallinfo();
        std::fprintf(log, "%s: %s; heap=%zu allocated=%d free=%d last=%08x\n", ok ? "PASS" : "FAIL",
                     reason, ncmHeapCommittedSize(), info.uordblks, info.fordblks,
                     ncmHeapLastResult());
        std::fflush(log);
        if (!ok) {
            runtime->stop();
            std::fclose(log);
            svcExitProcess();
        }
    };
    if (!stage) {
        if (!deadline) {
            deadline = armGetSystemTick() + armNsToTicks(60000000000ull);
            std::remove(NCM_CONFIG_DIRECTORY "/memory-codec.log");
        }
        const auto airplay = runtime->airplay_status();
        if (airplay.state == NcmAirplay_Starting && armGetSystemTick() < deadline) {
            svcSleepThread(100000000);
            return 0;
        }
        finish(airplay.enabled && airplay.state == NcmAirplay_Ready &&
                   ncmHeapCommittedSize() <= 2u * 1024u * 1024u,
               "runtime and enabled AirPlay start within 2 MiB");
        finish(runtime->command(NcmCommand_LoadQueue, 0), "load 512-entry generated queue");
        stage = 1;
    }
    const auto status = runtime->status();
    if (status.state == NcmPlayback_Error) {
        std::fprintf(log, "error=%08x message=%s\n", status.error_code, status.message);
        finish(false, "runtime playback error");
    }
    if (armGetSystemTick() >= deadline)
        finish(false, "deadline");
    if (status.state == NcmPlayback_Playing && status.position_ms >= 1000 &&
        status.queue_index == stage - 1) {
        finish(true, stage == 1   ? "MP3 decoded and emitted"
                     : stage == 2 ? "AAC decoded and emitted"
                                  : "FLAC decoded and emitted");
        if (stage < 3) {
            finish(runtime->command(NcmCommand_Next), "next track accepted");
            ++stage;
        } else {
            runtime->stop();
            finish(true, "RESULT: all formats and orderly stop");
            std::fclose(log);
            svcExitProcess();
        }
    }
    svcSleepThread(100000000);
    return 0;
}
