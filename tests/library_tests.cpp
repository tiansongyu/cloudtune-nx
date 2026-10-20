#include "netease/library.hpp"
#include <cassert>
#include <iostream>
using namespace netease;
int main() {
    auto s = parse_song(nlohmann::json::parse(
        R"({"id":9876543210123,"name":"中文音乐","ar":[{"name":"歌手甲"},{"name":"歌手乙"}],"al":{"name":"专辑","picUrl":null},"dt":196123})"));
    assert(s.id == 9876543210123ULL && s.title == "中文音乐" && s.artist == "歌手甲 / 歌手乙" &&
           s.cover.empty());
    assert(duration_text(s.duration_ms) == "3:16");
    auto cloud = parse_song(nlohmann::json::parse(
        R"({"songId":42,"songName":"云盘","simpleSong":{"id":42,"name":"云盘","al":null,"ar":null}})"));
    assert(cloud.id == 42);
    auto lyrics =
        parse_lrc("[offset:-100]\n[00:01.50][00:03.2]重复\n[00:02.000]第二句\n[ti:标题]\n");
    assert(lyrics.size() == 3 && lyrics[0].time_ms == 1400 && lyrics[1].time_ms == 1900 &&
           lyrics[2].time_ms == 3100);
    assert(lyric_index(lyrics, 0) == -1 && lyric_index(lyrics, 1900) == 1 &&
           lyric_index(lyrics, 99999) == 2);
    auto malformed = parse_lrc("[99999999999999999999999:01]bad\n[ar:名字]\n[00:00.00]开始");
    assert(malformed.size() == 1 && malformed[0].text == "开始");
    assert(parse_songs(nlohmann::json()).empty());
    std::cout << "library: parsing, 64-bit IDs, nulls, LRC offsets and synchronization passed\n";
}
