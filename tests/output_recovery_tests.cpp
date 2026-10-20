/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "engine.hpp"
#include <SDL2/SDL.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace netease::playback;
using namespace std::chrono_literals;

static std::atomic<bool> lost{false}, unavailable{false}, blocked{false}, writeFailure{false};
static std::atomic<bool> sleepGap{false};
static std::atomic<bool> stalledTail{false};
static std::atomic<bool> failQueueAllocation{false};
static thread_local bool rejectCommandAllocations=false;
static std::atomic<bool> commandAllocationRejected{false};
static std::atomic<unsigned> openFailures{0}, opens{0};
extern "C" {
void* __real__Znwm(size_t);
void* __wrap__Znwm(size_t bytes) {
    if (rejectCommandAllocations) {
        commandAllocationRejected=true;
        throw std::bad_alloc();
    }
    if (bytes == sizeof(NcmQueueEntry)*2 && failQueueAllocation.exchange(false))
        throw std::bad_alloc();
    return __real__Znwm(bytes);
}
SDL_AudioDeviceID __real_SDL_OpenAudioDevice(const char*, int, const SDL_AudioSpec*, SDL_AudioSpec*, int);
SDL_AudioStatus __real_SDL_GetAudioDeviceStatus(SDL_AudioDeviceID);
Uint32 __real_SDL_GetQueuedAudioSize(SDL_AudioDeviceID);
int __real_SDL_QueueAudio(SDL_AudioDeviceID, const void*, Uint32);
SDL_AudioDeviceID __wrap_SDL_OpenAudioDevice(const char* name, int capture,
        const SDL_AudioSpec* wanted, SDL_AudioSpec* obtained, int changes) {
    ++opens;
    if (unavailable) return 0;
    if (openFailures.load()) { --openFailures; return 0; }
    return __real_SDL_OpenAudioDevice(name,capture,wanted,obtained,changes);
}
SDL_AudioStatus __wrap_SDL_GetAudioDeviceStatus(SDL_AudioDeviceID device) {
    if (lost.exchange(false)) return SDL_AUDIO_STOPPED;
    return __real_SDL_GetAudioDeviceStatus(device);
}
Uint32 __wrap_SDL_GetQueuedAudioSize(SDL_AudioDeviceID device) {
    if (sleepGap.exchange(false)) {
        std::this_thread::sleep_for(2100ms);
        return 32768;
    }
    return blocked ? 32768 : stalledTail ? 4096 : __real_SDL_GetQueuedAudioSize(device);
}
int __wrap_SDL_QueueAudio(SDL_AudioDeviceID device, const void* data, Uint32 bytes) {
    return writeFailure.exchange(false) ? -1 : __real_SDL_QueueAudio(device,data,bytes);
}
}
static void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class Predicate> static NcmPlaybackStatus wait(Engine& engine, Predicate done, int ms=5000) {
    for (int i=0;i<ms/10;++i) {
        const auto status=engine.status();
        if (done(status)) return status;
        std::this_thread::sleep_for(10ms);
    }
    throw std::runtime_error("Timed out waiting for recovery state");
}
int main(int argc,char** argv) try {
    check(argc==3 || argc==4,"usage: output-recovery <directory> <generated audio> [scenario]");
    std::filesystem::create_directories(argv[1]);
    NcmQueueEntry queue[2]{};
    for (unsigned i=0;i<2;++i) {
        queue[i].id=900+i; queue[i].duration_ms=30000;
        std::snprintf(queue[i].url,sizeof(queue[i].url),"%s",argv[2]);
        std::snprintf(queue[i].title,sizeof(queue[i].title),"Generated track %u",i);
    }
    check(ncmWriteQueueFile((std::string(argv[1])+"/queue.nsq").c_str(),queue,2)==0,"queue");
    Engine engine({argv[1],""});
    check(engine.start(),"start");
    const auto descriptors = [] {
        return std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                             std::filesystem::directory_iterator{});
    };
    const auto before = descriptors();
    failQueueAllocation=true;
    check(engine.command(NcmCommand_LoadQueue),"queue command");
    wait(engine,[](auto s){return s.state==NcmPlayback_Error && s.error_code==10;});
    check(!failQueueAllocation && descriptors()==before,"allocation failure leaked queue file");
    check(engine.command(NcmCommand_LoadQueue),"engine did not survive allocation failure");
    wait(engine,[](auto s){return s.position_ms>300;});
    if (argc==4) {
        const std::string scenario=argv[3];
        if (scenario=="command-oom") {
            unsigned accepted=0;
            rejectCommandAllocations=true;
            for (unsigned i=0;i<128;++i) {
                accepted+=engine.command(NcmCommand_Volume,500);
                std::this_thread::sleep_for(1ms);
            }
            const bool began=engine.begin_external(48000);
            engine.flush_external();
            for (unsigned i=0;i<128;++i) engine.end_external();
            rejectCommandAllocations=false;
            check(!commandAllocationRejected && accepted && began,"command/lifecycle path allocated");
            wait(engine,[](auto s){return s.track_id==900 && s.state==NcmPlayback_Paused;});
            check(engine.command(NcmCommand_Play),"command queue did not recover");
            check(engine.command(NcmCommand_Volume,321),"volume control did not recover");
            wait(engine,[](auto s){return s.volume_permille==321 && s.state==NcmPlayback_Playing;});
        } else if (scenario=="seek-recovery") {
            lost=true;
            wait(engine,[](auto s){return s.state==NcmPlayback_Buffering;});
            check(engine.command(NcmCommand_Seek,3000),"seek during recovery rejected");
            wait(engine,[](auto s){return s.state==NcmPlayback_Playing && s.position_ms>=3000;},2000);
        } else if (scenario=="resume-recovery") {
            check(engine.begin_external(48000),"AirPlay handoff");
            wait(engine,[](auto s){return s.track_id==0 && s.state==NcmPlayback_Playing;});
            engine.end_external();
            const auto saved=wait(engine,[](auto s){return s.track_id==900 && s.state==NcmPlayback_Paused;});
            openFailures=1;
            check(engine.command(NcmCommand_Play),"music resume rejected");
            wait(engine,[](auto s){return s.state==NcmPlayback_Buffering;});
            const auto resumed=wait(engine,[](auto s){return s.state==NcmPlayback_Playing;});
            check(resumed.track_id==900 && resumed.position_ms>=saved.position_ms,
                  "output recovery rewound resumed music");
        } else if (scenario=="tail-stall") {
            stalledTail=true;
            check(engine.command(NcmCommand_Seek,29950),"tail seek rejected");
            wait(engine,[](auto s){return s.state==NcmPlayback_Buffering;},7000);
            stalledTail=false;
            wait(engine,[](auto s){return s.track_id==901 && s.state==NcmPlayback_Playing;});
        } else if (scenario=="control-stall") {
            blocked=true;
            bool recovered=false;
            for (unsigned i=0;i<120;++i) {
                check(engine.command(NcmCommand_Volume,500+i%100),"volume command rejected");
                std::this_thread::sleep_for(50ms);
                if (engine.status().state==NcmPlayback_Buffering) { recovered=true; break; }
            }
            check(recovered,"volume commands indefinitely postponed stalled-output recovery");
            blocked=false;
            wait(engine,[](auto s){return s.state==NcmPlayback_Playing;});
        } else throw std::runtime_error("Unknown recovery scenario");
        engine.stop();
        std::printf("PASS: %s\n",argv[3]);
        return 0;
    }
    openFailures=2; lost=true;
    const auto recovering=wait(engine,[](auto s){return s.state==NcmPlayback_Buffering;});
    check(recovering.track_id==900,"device loss skipped track");
    check(engine.command(NcmCommand_Volume,500),"controls unavailable during recovery");
    wait(engine,[](auto s){return s.volume_permille==500;});
    wait(engine,[&](auto s){return s.state==NcmPlayback_Playing && s.position_ms>recovering.position_ms+100;});
    check(!openFailures && engine.status().track_id==900,"retry lost track or position");

    // A persistent failure stops retrying and never skips through the playlist.
    unavailable=true; lost=true;
    const auto held=wait(engine,[](auto s){return s.state==NcmPlayback_Paused && s.error_code==6;});
    const auto attemptCount=opens.load();
    std::this_thread::sleep_for(600ms);
    check(opens==attemptCount && engine.status().track_id==900 &&
          engine.status().position_ms==held.position_ms,"unbounded retries or lost cursor");
    unavailable=false;
    check(engine.command(NcmCommand_Play),"manual retry");
    wait(engine,[&](auto s){return s.state==NcmPlayback_Playing && s.position_ms>held.position_ms+100;});
    writeFailure=true;
    wait(engine,[](auto s){return s.state==NcmPlayback_Buffering;});
    wait(engine,[](auto s){return s.state==NcmPlayback_Playing;});

    // Stale queued output (the console's unreleased-buffer case) also recovers.
    blocked=true;
    wait(engine,[](auto s){return s.state==NcmPlayback_Buffering;},7000);
    blocked=false;
    wait(engine,[](auto s){return s.state==NcmPlayback_Playing;});
    check(engine.status().track_id==900,"stalled device skipped track");
    sleepGap=true;
    wait(engine,[](auto s){return s.state==NcmPlayback_Buffering;},3500);
    wait(engine,[](auto s){return s.state==NcmPlayback_Playing;});
    check(engine.status().track_id==900,"suspend gap lost current track");

    // Live PCM is dropped while recovering; only fresh audio advances afterward.
    check(engine.begin_external(48000),"AirPlay begin");
    wait(engine,[](auto s){return s.track_id==0 && s.state==NcmPlayback_Playing;});
    std::vector<int16_t> pcm(9600*2,1000);
    engine.write_external(pcm.data(),9600);
    wait(engine,[](auto s){return s.position_ms>50;});
    openFailures=1; lost=true;
    const auto live=wait(engine,[](auto s){return s.state==NcmPlayback_Buffering;});
    for (int i=0;i<5;++i) check(engine.write_external(pcm.data(),9600),"live discard");
    wait(engine,[](auto s){return s.state==NcmPlayback_Playing;});
    std::this_thread::sleep_for(100ms);
    check(engine.status().position_ms==live.position_ms,"recovery replayed stale AirPlay PCM");
    check(engine.write_external(pcm.data(),9600),"fresh live data");
    wait(engine,[&](auto s){return s.position_ms>live.position_ms+50;});
    lost=true;
    wait(engine,[](auto s){return s.state==NcmPlayback_Buffering;});
    engine.flush_external();
    wait(engine,[](auto s){return s.state==NcmPlayback_Playing && s.position_ms==0;});
    check(engine.write_external(pcm.data(),9600),"fresh data after recovery flush");
    wait(engine,[](auto s){return s.position_ms>50;});
    engine.end_external();
    wait(engine,[](auto s){return s.track_id==900 && s.state==NcmPlayback_Paused;});
    engine.command(NcmCommand_Play);
    wait(engine,[](auto s){return s.state==NcmPlayback_Playing;});
    unavailable=true; lost=true;
    wait(engine,[](auto s){return s.state==NcmPlayback_Buffering;});
    const auto stop=std::chrono::steady_clock::now();
    engine.stop();
    check(std::chrono::steady_clock::now()-stop<500ms,"recovery delayed shutdown");
    std::puts("PASS: lost output, failed writes, stalled buffers, bounded retries, saved position and live PCM recovery");
} catch (const std::exception& error) {
    std::fprintf(stderr,"FAIL: %s\n",error.what()); return 1;
}
