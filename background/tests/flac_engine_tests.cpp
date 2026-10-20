/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "engine.hpp"
#include "netease/crypto.hpp"
#include <curl/curl.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <vector>
using netease::playback::Engine;
using namespace std::chrono_literals;
static void check(bool value, const char* reason) {
    if (!value) throw std::runtime_error(reason);
}
template<class Predicate> static void waitFor(Engine& engine, Predicate predicate,
                                             const char* reason, int milliseconds=5000) {
    for (int i=0; i<milliseconds/10; ++i) {
        if (predicate(engine.status())) return;
        std::this_thread::sleep_for(10ms);
    }
    const auto s=engine.status();
    std::fprintf(stderr,"status=%u track=%llu position=%u message=%s\n",s.state,
        static_cast<unsigned long long>(s.track_id),s.position_ms,s.message);
    throw std::runtime_error(reason);
}
static void queue(const std::string& directory, const std::string& url, uint64_t id=700) {
    NcmQueueEntry track{};
    track.id=id; track.duration_ms=8000;
    std::snprintf(track.title,sizeof(track.title),"Generated FLAC");
    std::snprintf(track.url,sizeof(track.url),"%s",url.c_str());
    check(ncmWriteQueueFile((directory+"/queue.nsq").c_str(),&track,1)==0,"cannot write queue");
}
static void emptyQueue(const std::string& directory) {
    NcmQueueHeader header{{'N','C','M','Q','U','E','U','E'},NCM_PROTOCOL_VERSION,0};
    std::ofstream out(directory+"/queue.nsq",std::ios::binary);
    out.write(reinterpret_cast<const char*>(&header),sizeof(header));
    check(out.good(),"cannot empty queue");
}
int main(int argc,char** argv) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    try {
        check(argc==6,"usage: flac-engine-tests DIRECTORY LOCAL_AUDIO TRUNCATED_FLAC HTTPS_AUDIO CA");
        const std::string directory=argv[1];
        std::filesystem::create_directories(directory);
        std::ofstream(directory+"/settings.json")<<"{\"quality\":\"lossless\",\"diagnostics\":true}\n";
        queue(directory,argv[2]);
        Engine engine({directory,argv[5]});
        check(engine.start() && engine.command(NcmCommand_LoadQueue),"engine start failed");
        waitFor(engine,[](auto s){return s.position_ms>150;},"FLAC clock did not advance");
        emptyQueue(directory);
        check(engine.command(NcmCommand_ClearQueue),"synchronous account queue clear failed");
        auto cleared=engine.status();
        check(cleared.track_id==0 && cleared.queue_count==0 && cleared.state==NcmPlayback_Stopped,
              "clear acknowledged before old playback stopped");
        engine.command(NcmCommand_Play);
        std::this_thread::sleep_for(150ms);
        check(engine.status().track_id==0 && engine.status().state==NcmPlayback_Stopped,
              "empty queue restarted old track");

        // Exercise an enqueued LoadQueue racing with the account barrier.
        queue(directory,argv[2]);
        engine.command(NcmCommand_LoadQueue);
        emptyQueue(directory);
        check(engine.command(NcmCommand_ClearQueue),"pending LoadQueue clear failed");
        std::this_thread::sleep_for(100ms);
        check(engine.status().track_id==0 && engine.status().state==NcmPlayback_Stopped,
              "pending LoadQueue restarted after account barrier");

        queue(directory,argv[2]);
        engine.command(NcmCommand_LoadQueue);
        waitFor(engine,[](auto s){return s.position_ms>150;},"music reload failed");
        check(engine.begin_external(44100),"AirPlay handoff failed");
        std::vector<int16_t> external(44100*2,1000);
        check(engine.write_external(external.data(),44100),"AirPlay PCM rejected");
        waitFor(engine,[](auto s){return s.track_id==0 && s.position_ms>100;},"AirPlay not playing");
        emptyQueue(directory);
        check(engine.command(NcmCommand_ClearQueue),"clear during AirPlay failed");
        check(engine.external_active() && engine.status().state==NcmPlayback_Playing &&
              engine.status().queue_count==0,"account clear interrupted AirPlay");
        engine.end_external();
        waitFor(engine,[](auto s){return s.state==NcmPlayback_Stopped;},"AirPlay end restored cleared music");
        check(engine.status().track_id==0,"AirPlay end restored previous account metadata");

        queue(directory,argv[3]);
        engine.command(NcmCommand_LoadQueue);
        waitFor(engine,[](auto s){return s.state==NcmPlayback_Error;},
                "truncated FLAC silently finished",7000);
        check(std::strstr(engine.status().message,"truncated")!=nullptr,"missing damaged FLAC error");

        // A completed marker can survive an SD removal/file loss without its
        // audio. It must be discarded so a fresh progressive download starts.
        const auto orphan=directory+"/cache/700-"+
            netease::crypto::md5("anonymous\nlossless")+".audio.complete";
        std::ofstream(orphan).close();
        queue(directory,argv[4]);
        engine.command(NcmCommand_LoadQueue);
        waitFor(engine,[](auto s){return s.state==NcmPlayback_Playing && s.position_ms>100;},
                "progressive FLAC did not start",12000);
        std::filesystem::path cache;
        for (const auto& entry:std::filesystem::directory_iterator(directory+"/cache"))
            if (entry.path().extension()==".audio") cache=entry.path();
        check(!cache.empty(),"audio was not cached with a neutral extension");
        check(!std::filesystem::exists(cache.string()+".complete"),"audio waited for the full download");
        engine.command(NcmCommand_Pause);
        waitFor(engine,[](auto s){return s.state==NcmPlayback_Paused;},"progressive FLAC pause failed");
        engine.command(NcmCommand_Seek,2000);
        waitFor(engine,[](auto s){return s.position_ms==2000;},"progressive FLAC seek failed",12000);
        engine.command(NcmCommand_Play);
        waitFor(engine,[](auto s){return s.position_ms>2100;},"progressive FLAC resume failed");
        waitFor(engine,[&](auto){return std::filesystem::exists(cache.string()+".complete");},
                "FLAC cache not completed",12000);
        engine.command(NcmCommand_Stop);
        waitFor(engine,[](auto s){return s.state==NcmPlayback_Stopped;},"network stop failed");
        // Old .mp3 cache names must still work even when their bytes are FLAC.
        const auto legacy=cache.parent_path()/(cache.stem().string()+".mp3");
        std::filesystem::rename(cache,legacy);
        std::filesystem::rename(cache.string()+".complete",legacy.string()+".complete");
        queue(directory,"https://127.0.0.1:1/unavailable.flac");
        engine.command(NcmCommand_LoadQueue);
        waitFor(engine,[](auto s){return s.state==NcmPlayback_Playing && s.position_ms>100;},
                "complete legacy cache required a network connection");
        // Cancel a different, still downloading track. ClearQueue must wait
        // for teardown and must never publish its partial response as complete.
        queue(directory,argv[4],701);
        engine.command(NcmCommand_LoadQueue);
        waitFor(engine,[](auto s){return s.track_id==701 && s.state==NcmPlayback_Buffering;},
                "cancellation fixture did not start buffering");
        emptyQueue(directory);
        const auto cancelStarted=std::chrono::steady_clock::now();
        check(engine.command(NcmCommand_ClearQueue),"buffered download did not cancel");
        check(std::chrono::steady_clock::now()-cancelStarted<2s,"download cancellation was not prompt");
        check(engine.status().state==NcmPlayback_Stopped && engine.status().track_id==0,
              "cancelled download retained playback");
        for (const auto& entry:std::filesystem::directory_iterator(directory+"/cache"))
            check(entry.path().filename().string().rfind("701-",0)!=0 ||
                  entry.path().extension()!=".complete","cancelled partial download was marked complete");
        engine.stop();
        std::puts("PASS audio engine: progressive HTTPS, seek/pause, legacy cache, corruption, account clear barrier, AirPlay preservation");
    } catch (const std::exception& error) {
        std::fprintf(stderr,"FAIL FLAC engine: %s\n",error.what());
        curl_global_cleanup(); return 1;
    }
    curl_global_cleanup();
}
