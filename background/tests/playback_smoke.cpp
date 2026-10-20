/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "engine.hpp"
#include <array>
#include <curl/curl.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>
using namespace std::chrono_literals;
static void require(bool condition,const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Test> static void waitFor(Test test,const char* failure,int timeout=3000) {
    for (int i=0;i<timeout/20;++i) { if(test())return; std::this_thread::sleep_for(20ms); }
    throw std::runtime_error(failure);
}
int main(int argc,char** argv) {
    if(argc<3) { std::fprintf(stderr,"usage: ncm-playback-smoke CONFIG_DIR LOCAL_MP3_OR_HTTPS_URL [CA_BUNDLE] [SECOND_MP3]\n");return 2; }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    try {
        std::string directory=argv[1];
        std::filesystem::create_directories(directory);
        NcmQueueEntry entries[2]{};
        for(unsigned i=0;i<2;++i) {
            entries[i].id=100+i; entries[i].duration_ms=8000;
            std::snprintf(entries[i].url,sizeof(entries[i].url),"%s",i==1 && argc>4?argv[4]:argv[2]);
            std::snprintf(entries[i].title,sizeof(entries[i].title),"Audio integration test %u",i);
            std::snprintf(entries[i].artist,sizeof(entries[i].artist),"Generated sine fixture");
        }
        require(ncmWriteQueueFile((directory+"/queue.nsq").c_str(),entries,2)==0,"queue write failed");
        require(ncmWriteQueueFile((directory+"/queue.nsq").c_str(),entries,2)==0,"existing queue replacement failed");
        require(ncmWriteQueueFile((directory+"/invalid.nsq").c_str(),entries,NCM_MAX_QUEUE+1)==-1,"oversized queue accepted");
        netease::playback::Engine engine({directory,argc>3?argv[3]:""});
        require(engine.start(),"engine start failed");
        require(!engine.command(NcmCommand_Volume,1001),"invalid volume accepted");
        require(!engine.command(NcmCommand_Repeat,3),"invalid repeat accepted");
        require(engine.command(NcmCommand_LoadQueue,0),"queue command rejected");
        waitFor([&]{return engine.status().state==NcmPlayback_Playing;},"never started playing",15000);
        waitFor([&]{return engine.status().position_ms>=200;},"playback clock did not advance");
        require(engine.status().track_id==100,"wrong initial track");
        engine.command(NcmCommand_Pause);
        waitFor([&]{return engine.status().state==NcmPlayback_Paused;},"pause failed");
        uint32_t paused=engine.status().position_ms;
        engine.command(NcmCommand_Pause);
        std::this_thread::sleep_for(200ms);
        require(engine.status().position_ms==paused,"paused clock advanced");
        engine.command(NcmCommand_Seek,2000);
        waitFor([&]{return engine.status().position_ms==2000;},"seek failed");
        engine.command(NcmCommand_Volume,350);
        engine.command(NcmCommand_Repeat,NcmRepeat_All);
        engine.command(NcmCommand_Shuffle,1);
        engine.command(NcmCommand_Play);
        waitFor([&]{auto s=engine.status();return s.state==NcmPlayback_Playing && s.volume_permille==350 && s.shuffle && s.repeat==NcmRepeat_All;},"control state failed");
        engine.command(NcmCommand_Play);
        waitFor([&]{return engine.status().position_ms>2100;},"repeated play failed to advance after pause");
        engine.command(NcmCommand_Next);
        waitFor([&]{return engine.status().track_id==101 && engine.status().state==NcmPlayback_Playing;},"next failed");
        waitFor([&]{return engine.status().position_ms>=200;},"second track clock did not advance");
        engine.command(NcmCommand_Previous);
        waitFor([&]{return engine.status().track_id==100 && engine.status().state==NcmPlayback_Playing;},"previous failed");
        engine.command(NcmCommand_Stop);
        waitFor([&]{return engine.status().state==NcmPlayback_Stopped;},"stop failed");
        std::filesystem::rename(directory+"/queue.nsq",directory+"/queue.nsq.bak");
        engine.command(NcmCommand_Repeat,NcmRepeat_Off);
        engine.command(NcmCommand_Shuffle,0);
        engine.command(NcmCommand_LoadQueue,0);
        waitFor([&]{return engine.status().state==NcmPlayback_Playing;},"backup queue recovery failed",15000);
        engine.command(NcmCommand_Seek,engine.status().duration_ms-400);
        waitFor([&]{return engine.status().track_id==101 && engine.status().state==NcmPlayback_Playing;},"natural queue advancement failed",12000);
        waitFor([&]{return engine.status().position_ms>=200;},"automatic next track clock did not advance");
        engine.command(NcmCommand_Seek,engine.status().duration_ms-400);
        waitFor([&]{return engine.status().state==NcmPlayback_Stopped;},"queue did not stop at its end",12000);
        if (std::string(argv[2]).rfind("https://",0)==0) {
            const auto completed=[&] {
                size_t count=0;
                for (const auto& file:std::filesystem::directory_iterator(directory+"/cache"))
                    if(file.path().extension()==".complete")++count;
                return count;
            };
            auto count=completed();
            std::ofstream(directory+"/settings.json")<<"{\"quality\":\"higher\"}\n";
            engine.command(NcmCommand_LoadQueue,0);
            waitFor([&]{return completed()>count;},"quality change reused old cached audio",15000);
            engine.command(NcmCommand_Stop);
            waitFor([&]{return engine.status().state==NcmPlayback_Stopped;},"quality test stop failed");
            count=completed();
            std::ofstream(directory+"/cookies.txt")
                <<"# Netscape HTTP Cookie File\n.music.163.com\tTRUE\t/\tTRUE\t0\tMUSIC_U\tfixture-not-a-real-session\n";
            engine.command(NcmCommand_LoadQueue,0);
            waitFor([&]{return completed()>count;},"login change reused anonymous cached preview",15000);
            engine.command(NcmCommand_Stop);
            waitFor([&]{return engine.status().state==NcmPlayback_Stopped;},"session test stop failed");
        }
        auto badFirst=std::array<NcmQueueEntry,2>{entries[0],entries[1]};
        std::snprintf(badFirst[0].url,sizeof(badFirst[0].url),"%s/missing-audio.mp3",directory.c_str());
        require(ncmWriteQueueFile((directory+"/queue.nsq").c_str(),badFirst.data(),badFirst.size())==0,"error queue write failed");
        engine.command(NcmCommand_Repeat,NcmRepeat_One);
        engine.command(NcmCommand_LoadQueue,0);
        waitFor([&]{return engine.status().track_id==101 && engine.status().state==NcmPlayback_Playing;},"repeat-one kept retrying unavailable track",15000);
        engine.command(NcmCommand_Stop);
        waitFor([&]{return engine.status().state==NcmPlayback_Stopped;},"error queue stop failed");
        FILE* invalid=std::fopen((directory+"/queue.nsq").c_str(),"wb");
        require(invalid!=nullptr,"cannot write malformed fixture");
        std::fputs("bad",invalid);std::fclose(invalid);
        engine.command(NcmCommand_LoadQueue,0);
        waitFor([&]{return engine.status().state==NcmPlayback_Error && engine.status().queue_count==0;},"malformed queue accepted");
        engine.stop();
        std::puts("PASS queue / progressive MP3 decode / SDL output / clock / pause / seek / volume / repeat / shuffle / next / previous / natural advance / end / stop / malformed queue (HTTPS also verifies quality/session cache isolation)");
    } catch(const std::exception& e) {
        std::fprintf(stderr,"FAIL %s\n",e.what());curl_global_cleanup();return 1;
    }
    curl_global_cleanup();return 0;
}
