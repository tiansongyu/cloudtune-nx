// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only diagnostic. Does not print cookie values or account credentials.
#include "netease/api.hpp"
#include <iostream>

int main(int argc, char** argv) {
    netease::ApiConfig config;
    config.timeout_seconds = 20;
    netease::ApiClient api(config);
    if (argc > 1) {
        try {
            const auto args = argc > 2 ? netease::Json::parse(argv[2]) : netease::Json::object();
            std::cout << api.call(argv[1], args).dump(2) << '\n';
            return 0;
        } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    }
    bool failed = false;
    for (const auto& item : std::vector<std::pair<std::string, netease::Json>>{
        {"banners", {}}, {"toplist", {}}, {"search_song", {{"keywords", "网易云"}, {"limit", 2}}},
        {"songs_detail", {{"ids", {347230}}}}, {"songs_url", {{"ids", {347230}}, {"br", 128000}}},
        {"song_lyric", {{"music_id", 347230}}}
    }) {
        try {
            const auto response = api.call(item.first, item.second.is_null() ? netease::Json::object() : item.second);
            std::cout << item.first << ": OK code=" << response.value("code", 0)
                      << " JSON bytes=" << response.dump().size() << std::endl;
        } catch (const std::exception& error) {
            std::cout << item.first << ": FAIL " << error.what() << std::endl;
            failed = true;
        }
    }
    return failed ? 1 : 0;
}
