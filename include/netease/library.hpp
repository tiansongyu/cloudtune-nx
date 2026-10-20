// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace netease {
enum class MediaKind { Song, Playlist, Album, Artist, Radio, Action };
struct Media {
    uint64_t id = 0;
    MediaKind kind = MediaKind::Song;
    std::string title, artist, album, cover, url;
    uint32_t duration_ms = 0;
    uint32_t count = 0;
};
struct LyricLine {
    int64_t time_ms;
    std::string text;
};
std::vector<LyricLine> parse_lrc(const std::string &source);
int lyric_index(const std::vector<LyricLine> &lines, uint64_t position);
Media parse_song(const nlohmann::json &song);
Media parse_collection(const nlohmann::json &item, MediaKind kind);
std::vector<Media> parse_songs(const nlohmann::json &array);
std::vector<Media> parse_collections(const nlohmann::json &array, MediaKind kind);
std::string duration_text(uint64_t ms);
std::string json_text(const nlohmann::json &j, const char *key, std::string fallback = {});
uint64_t json_id(const nlohmann::json &j, const char *key);
} // namespace netease
