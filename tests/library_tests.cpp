#include "netease/library.hpp"
#include <cassert>
#include <iostream>
#include <limits>
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
    for (const auto *bad : {"-1", "+12", " 12", "12tail", "", "18446744073709551616"})
        assert(json_id(nlohmann::json{{"id", bad}}, "id") == 0);
    assert(json_id(nlohmann::json{{"id", "18446744073709551615"}}, "id") ==
           std::numeric_limits<uint64_t>::max());
    assert(parse_song(nlohmann::json{{"id", 1}, {"dt", 4294967296ULL}}).duration_ms ==
           std::numeric_limits<uint32_t>::max());
    assert(parse_collection(nlohmann::json{{"id", 1}, {"size", 4294967296ULL}},
                            MediaKind::Playlist).count == std::numeric_limits<uint32_t>::max());
    assert(parse_lrc("[9223372036854775:01]overflow").empty());
    assert(parse_lrc("[153722867280912:59.999]addition overflow").empty());
    assert(parse_lrc("[offset:9223372036854775807]\n[00:01]offset overflow").empty());
    const auto negative = parse_lrc("[offset:-9223372036854775808]\n[00:01]before start");
    assert(negative.size() == 1 && negative[0].time_ms == 0);
    assert(parse_songs(nlohmann::json()).empty());
    std::cout << "library: parsing, 64-bit IDs, nulls, LRC offsets and synchronization passed\n";
}
