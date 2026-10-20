// SPDX-License-Identifier: GPL-3.0-or-later
#include "netease/library.hpp"
#include <iomanip>
#include <limits>
#include <regex>
#include <sstream>

namespace netease {
std::string json_text(const nlohmann::json &j, const char *key, std::string fallback) {
    auto i = j.find(key);
    return i != j.end() && i->is_string() ? i->get<std::string>() : fallback;
}
uint64_t json_id(const nlohmann::json &j, const char *key) {
    auto i = j.find(key);
    if (i == j.end() || i->is_null())
        return 0;
    if (i->is_number_unsigned())
        return i->get<uint64_t>();
    if (i->is_number_integer())
        return std::max<int64_t>(0, i->get<int64_t>());
    if (i->is_string()) {
        const auto &text = i->get_ref<const std::string &>();
        uint64_t value = 0;
        if (text.empty())
            return 0;
        for (const auto c : text) {
            if (c < '0' || c > '9' ||
                value > (std::numeric_limits<uint64_t>::max() - (c - '0')) / 10)
                return 0;
            value = value * 10 + (c - '0');
        }
        return value;
    }
    return 0;
}
static uint32_t bounded_count(uint64_t value) {
    return static_cast<uint32_t>(std::min<uint64_t>(value, std::numeric_limits<uint32_t>::max()));
}
static std::string artists(const nlohmann::json &j) {
    std::string s;
    if (j.is_array())
        for (const auto &a : j) {
            auto name = json_text(a, "name");
            if (!name.empty()) {
                if (!s.empty())
                    s += " / ";
                s += name;
            }
        }
    return s;
}
Media parse_song(const nlohmann::json &source) {
    const auto &s =
        source.contains("simpleSong") && source["simpleSong"].is_object() ? source["simpleSong"]
        : source.contains("mainSong") && source["mainSong"].is_object()   ? source["mainSong"]
                                                                          : source;
    Media m;
    m.id = json_id(s, "id");
    if (!m.id)
        m.id = json_id(source, "songId");
    m.title = json_text(s, "name", json_text(source, "songName", "未知歌曲"));
    m.duration_ms = bounded_count(json_id(s, "dt"));
    if (!m.duration_ms)
        m.duration_ms = bounded_count(json_id(s, "duration"));
    if (s.contains("ar"))
        m.artist = artists(s["ar"]);
    else if (s.contains("artists"))
        m.artist = artists(s["artists"]);
    if (m.artist.empty())
        m.artist = json_text(source, "artist", "未知艺术家");
    auto al = s.find("al");
    if (al == s.end())
        al = s.find("album");
    if (al != s.end() && al->is_object()) {
        m.album = json_text(*al, "name");
        m.cover = json_text(*al, "picUrl");
    }
    if (m.cover.empty())
        m.cover = json_text(source, "coverUrl", json_text(source, "picUrl"));
    return m;
}
Media parse_collection(const nlohmann::json &j, MediaKind kind) {
    Media m;
    m.kind = kind;
    m.id = json_id(j, "id");
    m.title = json_text(j, "name", "未命名");
    m.cover = json_text(j, "coverImgUrl", json_text(j, "picUrl"));
    m.count = bounded_count(json_id(j, "trackCount"));
    if (!m.count)
        m.count = bounded_count(json_id(j, "size"));
    if (j.contains("creator"))
        m.artist = json_text(j["creator"], "nickname");
    if (j.contains("artist"))
        m.artist = json_text(j["artist"], "name");
    if (m.artist.empty() && j.contains("artists"))
        m.artist = artists(j["artists"]);
    if (m.artist.empty())
        m.artist = json_text(j, "updateFrequency", json_text(j, "copywriter"));
    if (kind == MediaKind::Radio && j.contains("dj"))
        m.artist = json_text(j["dj"], "nickname");
    return m;
}
std::vector<Media> parse_songs(const nlohmann::json &array) {
    std::vector<Media> out;
    if (array.is_array())
        for (const auto &s : array) {
            auto m = parse_song(s);
            if (m.id)
                out.push_back(std::move(m));
        }
    return out;
}
std::vector<Media> parse_collections(const nlohmann::json &array, MediaKind kind) {
    std::vector<Media> out;
    if (array.is_array())
        for (const auto &s : array) {
            auto m = parse_collection(s, kind);
            if (m.id)
                out.push_back(std::move(m));
        }
    return out;
}
std::vector<LyricLine> parse_lrc(const std::string &source) {
    std::vector<LyricLine> result;
    static const std::regex time(R"(\[(\d+):(\d{1,2})(?:[\.:](\d{1,3}))?\])");
    static const std::regex offset_re(R"(\[offset:([+-]?\d+)\])");
    int64_t offset = 0;
    std::smatch offset_match;
    if (std::regex_search(source, offset_match, offset_re)) {
        try {
            offset = std::stoll(offset_match[1]);
        } catch (...) {
        }
    }
    std::istringstream stream(source);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        std::vector<int64_t> times;
        size_t text_start = 0;
        for (std::sregex_iterator i(line.begin(), line.end(), time), end; i != end; ++i) {
            try {
                auto frac = (*i)[3].str();
                while (frac.size() < 3)
                    frac += '0';
                const auto minutes = std::stoll((*i)[1]);
                const auto remainder = std::stoll((*i)[2]) * 1000 + std::stoll(frac);
                constexpr auto maximum = std::numeric_limits<int64_t>::max();
                if (minutes > (maximum - remainder) / 60000)
                    continue;
                const auto milliseconds = minutes * 60000 + remainder;
                if (offset > 0 && milliseconds > maximum - offset)
                    continue;
                // milliseconds is nonnegative, so a negative offset cannot
                // underflow; values before the song begins are clamped below.
                times.push_back(milliseconds + offset);
                text_start = i->position() + i->length();
            } catch (...) {
            }
        }
        for (auto t : times)
            if (text_start < line.size())
                result.push_back({std::max<int64_t>(0, t), line.substr(text_start)});
    }
    std::stable_sort(result.begin(), result.end(),
                     [](const auto &a, const auto &b) { return a.time_ms < b.time_ms; });
    return result;
}
int lyric_index(const std::vector<LyricLine> &lines, uint64_t position) {
    auto it =
        std::upper_bound(lines.begin(), lines.end(), position, [](uint64_t t, const LyricLine &l) {
            return t < static_cast<uint64_t>(l.time_ms);
        });
    return static_cast<int>(it - lines.begin()) - 1;
}
std::string duration_text(uint64_t ms) {
    std::ostringstream s;
    s << ms / 60000 << ':' << std::setw(2) << std::setfill('0') << (ms / 1000) % 60;
    return s.str();
}
} // namespace netease
