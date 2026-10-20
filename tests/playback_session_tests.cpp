// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine.hpp"
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

using namespace netease::playback;
using namespace std::chrono_literals;
static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static void playing(Engine& engine, uint64_t track) {
    const auto deadline=std::chrono::steady_clock::now()+5s;
    while (std::chrono::steady_clock::now()<deadline) {
        const auto status=engine.status();
        check(status.state!=NcmPlayback_Error,"Playback failed during session isolation test");
        if (status.track_id==track && status.state==NcmPlayback_Playing &&
            status.position_ms>=150) return;
        std::this_thread::sleep_for(10ms);
    }
    throw std::runtime_error("Playback session test timed out");
}
int main(int argc,char** argv) {
    try {
        check(argc==3,"usage: playback-session-tests DIRECTORY TEST_CA");
        NcmQueueEntry tracks[2]{};
        for (size_t i=0;i<2;++i) {
            tracks[i].id=710+i;
            tracks[i].duration_ms=3000;
            std::snprintf(tracks[i].title,sizeof(tracks[i].title),"Synthetic session track");
        }
        const std::string directory=argv[1];
        check(ncmWriteQueueFile((directory+"/queue.nsq").c_str(),tracks,2)==0,
              "Cannot save synthetic queue");
        Engine engine({directory,argv[2]});
        check(engine.start() && engine.command(NcmCommand_LoadQueue),"Cannot start engine");
        playing(engine,710);
        // The fixture changes the registry/mirror after the first request.
        // The loaded queue must retain one identity through track advancement.
        check(engine.command(NcmCommand_Next),"Cannot advance queue");
        playing(engine,711);
        check(engine.command(NcmCommand_ClearQueue),"Cannot clear old account queue");
        check(engine.status().queue_count==0,"Account barrier did not clear the queue");
        check(engine.command(NcmCommand_LoadQueue),"Cannot reload queue for new account");
        playing(engine,710);
        engine.stop();
        std::puts("PASS: playback progression and synchronous queue-clear barrier");
    } catch (const std::exception& error) {
        std::fprintf(stderr,"FAIL: %s\n",error.what());
        return 1;
    }
}
