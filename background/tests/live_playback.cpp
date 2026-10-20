/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "engine.hpp"
#include <curl/curl.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>
int main(int argc,char** argv) {
    if (argc<3) { std::fprintf(stderr,"usage: ncm-live-playback CONFIG_DIR SONG_ID [CA_BUNDLE]\n");return 2; }
    const std::string directory=argv[1];
    std::filesystem::create_directories(directory);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    NcmQueueEntry track{};
    track.id=std::stoull(argv[2]);
    track.duration_ms=300000;
    std::snprintf(track.title,sizeof(track.title),"Authorized API stream test");
    if (ncmWriteQueueFile((directory+"/queue.nsq").c_str(),&track,1)) return 2;
    netease::playback::Engine player({directory,argc>3?argv[3]:""});
    if (!player.start()) return 2;
    player.command(NcmCommand_LoadQueue);
    bool playing=false, passed=false;
    for (unsigned i=0;i<120;++i) {
        auto s=player.status();
        if (i%4==0) std::printf("state=%u position=%u duration=%u message=%s\n",
            s.state,s.position_ms,s.duration_ms,s.message);
        if (s.state==NcmPlayback_Playing) playing=true;
        if (playing && s.position_ms>=5000) { passed=true;break; }
        if (s.state==NcmPlayback_Error) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    player.stop();
    curl_global_cleanup();
    std::puts(passed?"PASS live song-id resolution / TLS MP3 stream / decoder / advancing audio clock":"FAIL live stream");
    return passed?0:1;
}
