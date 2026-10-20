// SPDX-License-Identifier: GPL-3.0-or-later
// Endpoint compatibility based on gmg137/netease-cloud-music-api 2.2.0,
// Copyright (C) 2019-2026 gmg137. See THIRD_PARTY_NOTICES.md.
#include "netease/api.hpp"
#include "netease/crypto.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <sstream>
#ifndef __SWITCH__
#include <sys/stat.h>
#endif

namespace netease {
namespace {
const std::vector<std::string> methods = {
    "fetch_xeapi_public_key", "login", "login_cellphone", "captcha", "login_qr_create",
    "login_qr_check", "login_status", "logout", "daily_task", "user_song_id_list",
    "user_song_list", "album_sublist", "user_cloud_disk", "song_list_detail",
    "songs_detail", "songs_url", "songs_url_v1", "recommend_resource", "recommend_songs",
    "personal_fm", "like", "like_v1", "fm_trash", "search", "search_song",
    "search_singer", "search_album", "search_songlist", "search_lyrics", "singer_songs",
    "singer_all_songs", "new_albums", "album", "songlist_detail_dynamic",
    "album_detail_dynamic", "top_song_list", "top_song_list_highquality", "toplist",
    "top_songs", "song_lyric", "song_list_like", "album_like", "homepage", "banners",
    "download_img", "download_song", "user_radio_sublist", "radio_program",
    "playmode_intelligence_list"
};
std::string scalar(const Json& value) {
    if (value.is_null()) throw ApiError("An API argument was null");
    return value.is_string() ? value.get<std::string>() : value.dump();
}
std::string required(const Json& args, const std::string& key, const std::string& alias = {}) {
    auto it = args.find(key);
    if (it == args.end() && !alias.empty()) it = args.find(alias);
    if (it == args.end() || it->is_null()) throw ApiError("Missing API argument: " + key);
    return scalar(*it);
}
std::string opt(const Json& args, const std::string& key, std::string fallback) {
    auto it = args.find(key);
    return it == args.end() || it->is_null() ? fallback : scalar(*it);
}
bool liked(const Json& args) {
    const auto v = opt(args, "like", "true");
    return v == "true" || v == "1";
}
Json ids_argument(const Json& args) {
    auto it = args.find("ids");
    if (it == args.end() || !it->is_array() || it->empty()) throw ApiError("ids must be a non-empty array");
    if (it->size() > 1000) throw ApiError("At most 1000 song ids per request");
    for (const auto& id : *it)
        if ((!id.is_number_unsigned() && !id.is_number_integer()) || id.get<std::int64_t>() <= 0)
            throw ApiError("Song ids must be positive integers");
    return *it;
}
std::string milliseconds() {
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
std::string service_message(const Json& json) {
    for (const auto* key : {"message", "msg"}) {
        if (json.contains(key) && json[key].is_string()) return json[key].get<std::string>();
    }
    return "Netease service rejected the request";
}
int service_code(const Json& json) {
    if (!json.is_object() || !json.contains("code")) return 200;
    if (json["code"].is_number_integer()) return json["code"].get<int>();
    if (json["code"].is_string()) {
        try { return std::stoi(json["code"].get<std::string>()); } catch (...) {}
    }
    return 200;
}
void check_url(const std::string& url) {
    if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0)
        throw ApiError("Only HTTP(S) download URLs are accepted");
}
} // namespace

std::vector<std::string> api_methods() { return methods; }

RequestSpec plan_request(const std::string& method, Json args) {
    if (!args.is_object()) throw ApiError("API arguments must be an object");
    RequestSpec r;
    auto& p = r.params;
    auto paged = [&] {
        p["offset"] = opt(args, "offset", "0");
        p["limit"] = opt(args, "limit", "30");
        try {
            const auto offset = std::stoll(p["offset"].get<std::string>());
            const auto limit = std::stoll(p["limit"].get<std::string>());
            if (offset < 0 || limit < 1 || limit > 10000) throw ApiError("Invalid pagination");
        } catch (const std::invalid_argument&) { throw ApiError("Pagination must be numeric"); }
          catch (const std::out_of_range&) { throw ApiError("Pagination is too large"); }
    };
    if (method == "login") {
        const auto username = required(args, "username");
        const bool phone = username.size() == 11 &&
            std::all_of(username.begin(), username.end(), [](char c){ return c >= '0' && c <= '9'; });
        r.path = phone ? "/weapi/login/cellphone" : "/weapi/login";
        p[phone ? "phone" : "username"] = username;
        // Public C++ interface accepts a password, not a pre-hashed password.
        p["password"] = args.contains("password_md5") ? required(args, "password_md5") :
            crypto::md5(required(args, "password"));
        p["rememberLogin"] = "true";
        if (!phone) p["clientToken"] = "1_jVUMqWEPke0/1/Vu56xCmJpo5vP1grjn_SOVVDzOc78w8OKLVZ2JH7IfkjSXqgfmh";
    } else if (method == "login_cellphone") {
        r.path = "/weapi/w/login/cellphone";
        p = {{"phone", required(args, "phone")}, {"countrycode", opt(args, "ctcode", "86")},
             {"type", "1"}, {"https", "true"}, {"remember", "true"}, {"captcha", required(args, "captcha")}};
    } else if (method == "captcha") {
        r.path = "/weapi/sms/captcha/sent";
        p = {{"cellphone", required(args, "phone")}, {"ctcode", opt(args, "ctcode", "86")},
             {"secrete", "music_middleuser_pclogin"}};
    } else if (method == "login_qr_create") {
        r.path = "/weapi/login/qrcode/unikey"; p["type"] = "1";
    } else if (method == "login_qr_check") {
        r.path = "/weapi/login/qrcode/client/login";
        p = {{"type", "1"}, {"key", required(args, "key")}};
    } else if (method == "login_status") r.path = "/weapi/nuser/account/get";
    else if (method == "logout") r.path = "/weapi/logout";
    else if (method == "daily_task") { r.path = "/weapi/point/dailyTask"; p["type"] = "0"; }
    else if (method == "user_song_id_list") {
        r.path = "/weapi/song/like/get"; p["uid"] = required(args, "uid");
    } else if (method == "user_song_list") {
        r.path = "/weapi/user/playlist"; p["uid"] = required(args, "uid"); paged();
    } else if (method == "album_sublist" || method == "user_radio_sublist") {
        r.path = method == "album_sublist" ? "/weapi/album/sublist" : "/weapi/djradio/get/subed";
        p["total"] = "true"; paged();
    } else if (method == "user_cloud_disk") {
        r.path = "/weapi/v1/cloud/get"; paged();
    } else if (method == "song_list_detail" || method == "top_songs") {
        r.path = "/weapi/v6/playlist/detail";
        p = {{"id", required(args, method == "top_songs" ? "list_id" : "songlist_id", "id")},
             {"total", "true"}, {"n", opt(args, "n", "1000")}}; paged();
        p["limit"] = opt(args, "limit", "1000");
    } else if (method == "songs_detail") {
        r.path = "/weapi/v3/song/detail";
        auto c = Json::array();
        for (const auto& id : ids_argument(args)) c.push_back({{"id", id}});
        p["c"] = c.dump();
    } else if (method == "songs_url" || method == "songs_url_v1") {
        r.path = method == "songs_url" ? "/api/song/enhance/player/url" : "/api/song/enhance/player/url/v1";
        r.crypto = CryptoMode::Eapi;
        p["ids"] = ids_argument(args).dump();
        if (method == "songs_url") p["br"] = opt(args, "br", "320000");
        else {
            const auto level = opt(args, "level", "standard");
            static const std::vector<std::string> levels = {
                "standard", "higher", "exhigh", "lossless", "hires", "jyeffect", "sky", "jymaster"};
            if (std::find(levels.begin(), levels.end(), level) == levels.end()) throw ApiError("Unknown audio quality");
            p["level"] = level;
            // MP3 at lossy levels is directly decodable by the background player.
            p["encodeType"] = opt(args, "encodeType",
                level == "standard" || level == "higher" || level == "exhigh" ? "mp3" : "flac");
            if (level == "sky") p["immerseType"] = "c51";
        }
    } else if (method == "recommend_resource") r.path = "/weapi/v1/discovery/recommend/resource";
    else if (method == "recommend_songs") {
        r.path = "/weapi/v3/discovery/recommend/songs"; p["afresh"] = "false";
    } else if (method == "personal_fm") r.path = "/weapi/v1/radio/get";
    else if (method == "like" || method == "like_v1") {
        r.path = method == "like" ? "/weapi/radio/like" : "/api/v1/radio/like";
        p = {{"alg", "itembased"}, {"trackId", required(args, "songid", "id")},
             {"like", liked(args) ? "true" : "false"}, {"time", method == "like" ? "25" : "3"}};
        if (method == "like_v1") { r.crypto = CryptoMode::Xeapi; r.append_csrf = false; }
    } else if (method == "fm_trash") {
        r.path = "/weapi/radio/trash/add";
        p = {{"alg", "RT"}, {"songId", required(args, "songid", "id")}, {"time", "25"}};
    } else if (method == "search" || method == "search_song" || method == "search_singer" ||
               method == "search_album" || method == "search_songlist" || method == "search_lyrics") {
        r.path = "/weapi/search/get";
        std::string type = method == "search_singer" ? "100" : method == "search_album" ? "10" :
            method == "search_songlist" ? "1000" : method == "search_lyrics" ? "1006" : "1";
        if (method == "search") type = opt(args, "types", opt(args, "type", "1"));
        p = {{"s", required(args, "keywords", "s")}, {"type", type}}; paged();
    } else if (method == "singer_songs") {
        r.path = "/weapi/v1/artist/" + required(args, "id"); r.append_csrf = false;
    } else if (method == "singer_all_songs") {
        r.path = "/weapi/v1/artist/songs"; r.append_csrf = false;
        p = {{"id", required(args, "id")}, {"private_cloud", "true"}, {"work_type", "1"},
             {"order", opt(args, "order", "hot")}}; paged();
    } else if (method == "new_albums") {
        r.path = "/weapi/album/new"; p = {{"area", opt(args, "area", "ALL")}, {"total", "true"}}; paged();
    } else if (method == "album") r.path = "/weapi/v1/album/" + required(args, "album_id", "id");
    else if (method == "songlist_detail_dynamic" || method == "album_detail_dynamic") {
        r.path = method == "songlist_detail_dynamic" ? "/weapi/playlist/detail/dynamic" : "/weapi/album/detail/dynamic";
        p["id"] = required(args, method == "songlist_detail_dynamic" ? "songlist_id" : "album_id", "id");
    } else if (method == "top_song_list") {
        r.path = "/weapi/playlist/list";
        p = {{"cat", opt(args, "cat", "全部")}, {"order", opt(args, "order", "hot")}, {"total", "true"}}; paged();
    } else if (method == "top_song_list_highquality") {
        r.path = "/weapi/playlist/highquality/list";
        p = {{"cat", opt(args, "cat", "全部")}, {"total", "true"},
             {"lasttime", opt(args, "lasttime", "0")}, {"limit", opt(args, "limit", "30")}};
    } else if (method == "toplist") r.path = "/weapi/toplist";
    else if (method == "song_lyric") {
        r.path = "/weapi/song/lyric";
        p = {{"id", required(args, "music_id", "id")}, {"lv", "-1"}, {"tv", "-1"}, {"rv", "-1"}};
    } else if (method == "song_list_like") {
        r.path = liked(args) ? "/weapi/playlist/subscribe" : "/weapi/playlist/unsubscribe";
        p["id"] = required(args, "id");
    } else if (method == "album_like") {
        r.path = liked(args) ? "/weapi/album/sub" : "/weapi/album/unsub";
        p["id"] = required(args, "id");
    } else if (method == "homepage") {
        r.path = "/weapi/homepage/block/page";
        p = {{"refresh", "false"}, {"cursor", opt(args, "cursor", "null")}};
    } else if (method == "banners") {
        r.path = "/weapi/v2/banner/get"; p["clientType"] = opt(args, "clientType", "pc");
    } else if (method == "radio_program") {
        r.path = "/weapi/dj/program/byradio";
        p = {{"radioId", required(args, "rid", "id")}, {"asc", "false"}}; paged();
    } else if (method == "playmode_intelligence_list") {
        r.path = "/weapi/playmode/intelligence/list";
        const auto id = required(args, "sid", "song_id");
        p = {{"songId", id}, {"type", "fromPlayOne"}, {"playlistId", required(args, "pid", "playlist_id")},
             {"startMusicId", id}, {"count", opt(args, "count", "1")}};
    } else throw ApiError("Unknown request method: " + method);
    return r;
}

struct ApiClient::Impl {
    ApiConfig config;
    CURL* curl = nullptr;
    std::mutex mutex;
    Json xeapi_key;
    std::string device_id;

    explicit Impl(ApiConfig c) : config(std::move(c)) {
        static std::once_flag initialized;
        std::call_once(initialized, [] {
            if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) throw ApiError("curl initialization failed");
        });
        curl = curl_easy_init();
        if (!curl) throw ApiError("curl allocation failed");
        if (!config.cookie_file.empty()) {
            auto parent = std::filesystem::path(config.cookie_file).parent_path();
            if (!parent.empty()) std::filesystem::create_directories(parent);
        }
        curl_easy_setopt(curl, CURLOPT_COOKIEFILE, config.cookie_file.c_str());
        if (!config.cookie_file.empty() && config.persist_cookies)
            curl_easy_setopt(curl, CURLOPT_COOKIEJAR, config.cookie_file.c_str());
        curl_easy_setopt(curl, CURLOPT_COOKIELIST, "RELOAD");
        device_id = crypto::hex(crypto::random_bytes(16));
    }
    ~Impl() {
        if (curl) {
            flush();
            curl_easy_cleanup(curl);
        }
    }
    void flush() {
        if (!config.persist_cookies) return;
        curl_easy_setopt(curl, CURLOPT_COOKIELIST, "FLUSH");
#ifndef __SWITCH__
        if (!config.cookie_file.empty()) chmod(config.cookie_file.c_str(), 0600);
#endif
    }
    std::string cookie(const std::string& name) {
        curl_slist* list = nullptr;
        curl_easy_getinfo(curl, CURLINFO_COOKIELIST, &list);
        std::string result;
        for (auto* node = list; node; node = node->next) {
            std::istringstream line(node->data);
            std::vector<std::string> parts;
            std::string part;
            while (std::getline(line, part, '\t')) parts.push_back(part);
            if (parts.size() >= 7 && parts[5] == name) result = parts[6];
        }
        curl_slist_free_all(list);
        return result;
    }
    static std::size_t write_string(char* ptr, std::size_t size, std::size_t count, void* data) {
        auto* text = static_cast<std::string*>(data);
        const auto length = size * count;
        if (text->size() + length > 32 * 1024 * 1024) return 0;
        try { text->append(ptr, length); } catch (...) { return 0; }
        return length;
    }
    static std::size_t write_file(char* ptr, std::size_t size, std::size_t count, void* data) {
        return std::fwrite(ptr, 1, size * count, static_cast<FILE*>(data));
    }
    std::string http(const std::string& url, const std::string* post,
                     const std::vector<std::string>& extra = {}, FILE* file = nullptr) {
        check_url(url);
        std::string output;
        std::string extra_cookies;
        char error[CURL_ERROR_SIZE]{};
        curl_slist* raw = nullptr;
        for (const auto& header : extra) {
            // CURLOPT_COOKIE merges protocol metadata with the cookie engine.
            // A literal Cookie header would replace the logged-in session.
            if (header.rfind("Cookie: ", 0) == 0) extra_cookies = header.substr(8);
            else raw = curl_slist_append(raw, header.c_str());
        }
        std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(raw, curl_slist_free_all);
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_PROXY, config.proxy.empty() ? nullptr : config.proxy.c_str());
        curl_easy_setopt(curl, CURLOPT_CAINFO, config.ca_bundle.empty() ? nullptr : config.ca_bundle.c_str());
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, config.timeout_seconds);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, std::min(15L, config.timeout_seconds));
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 32L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 20L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
        curl_easy_setopt(curl, CURLOPT_REFERER, "https://music.163.com/");
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/124.0.0.0 Safari/537.36");
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(curl, CURLOPT_COOKIE, extra_cookies.empty() ? nullptr : extra_cookies.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers.get());
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, file ? write_file : write_string);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, file ? static_cast<void*>(file) : static_cast<void*>(&output));
        if (post) {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(post->size()));
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post->c_str());
        } else {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, nullptr);
            curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        }
        const auto status = curl_easy_perform(curl);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);
        if (status != CURLE_OK) throw ApiError(std::string("Network: ") + (error[0] ? error : curl_easy_strerror(status)));
        long http_status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
        if (http_status < 200 || http_status >= 300) throw ApiError("HTTP " + std::to_string(http_status), static_cast<int>(http_status));
        flush();
        return output;
    }
    Json fetch_key() {
        if (xeapi_key.is_object()) return xeapi_key;
        const auto timestamp = milliseconds();
        std::string nonce;
        for (unsigned char c : crypto::random_bytes(16)) nonce += static_cast<char>('0' + c % 10);
        const auto body = crypto::form_encode({
            {"appVersion", "9.5.61"}, {"currentKeyVersion", ""}, {"deviceId", device_id}, {"nonce", nonce},
            {"os", "android"}, {"requestType", "active"}, {"signature", crypto::xeapi_signature(timestamp, nonce)},
            {"t1", ""}, {"t2", ""}, {"timestamp", timestamp}, {"uid", ""}
        });
        const auto text = http("https://interface.music.163.com/api/gorilla/anti/crawler/security/key/get", &body,
            {"Content-Type: application/x-www-form-urlencoded;charset=utf-8",
             "User-Agent: NeteaseMusic/9.5.61.260802021928(9005061);Dalvik/2.1.0 (Linux; U; Android 12; HBN-AL00 Build/cd737a2.0)",
             "Cookie: os=android; appver=9.5.61; osver=16"});
        auto response = Json::parse(text);
        if (!response.contains("data") || !response["data"].is_object() ||
            !response["data"].contains("encryptedData"))
            throw ApiError(service_message(response), service_code(response));
        xeapi_key = crypto::xeapi_public_key(response["data"]["encryptedData"].get<std::string>());
        return xeapi_key;
    }
    Json request(RequestSpec spec) {
        const auto csrf = cookie("__csrf");
        std::string url, body;
        std::vector<std::string> headers{"Content-Type: application/x-www-form-urlencoded", "Accept: */*"};
        if (spec.crypto == CryptoMode::Weapi) {
            spec.params["csrf_token"] = csrf;
            url = "https://music.163.com" + spec.path;
            if (spec.append_csrf) url += "?csrf_token=" + csrf;
            body = crypto::weapi(spec.params.dump());
            headers.push_back("Cookie: os=pc; appver=2.7.1.198277; __remember_me=true");
        } else if (spec.crypto == CryptoMode::Eapi) {
            const auto now = milliseconds();
            spec.params["csrf_token"] = csrf;
            spec.params["header"] = {
                {"osver", "16.2"}, {"deviceId", device_id}, {"os", "iPhone OS"}, {"appver", "9.0.90"},
                {"versioncode", "140"}, {"mobilename", ""}, {"buildver", now.substr(0, 10)},
                {"resolution", "1920x1080"}, {"__csrf", csrf}, {"channel", ""},
                {"requestId", now + "_0001"}
            };
            // Keep logged-in entitlement on the EAPI host.
            const auto music_u = cookie("MUSIC_U");
            if (!music_u.empty()) spec.params["header"]["MUSIC_U"] = music_u;
            url = "https://interface.music.163.com/eapi" + spec.path.substr(4);
            body = crypto::eapi(spec.path, spec.params.dump());
            headers.push_back("Cookie: os=iphone; appver=9.0.90; osver=16.2");
        } else {
            fetch_key();
            url = "https://interface3.music.163.com/xeapi/" + spec.path.substr(5);
            body = crypto::xeapi(spec.params, xeapi_key);
            headers.insert(headers.end(), {
                "X-Client-Enc-State: ENCRYPTED", "x-aeapi: true", "x-deviceid: " + device_id,
                "x-sdeviceid: " + device_id, "x-os: android", "x-osver: 16",
                "x-appver: 9.5.61", "x-buildver: " + milliseconds().substr(0, 10),
                "User-Agent: NeteaseMusic/9.5.61.260802021928(9005061);Dalvik/2.1.0 (Linux; U; Android 12; HBN-AL00 Build/cd737a2.0)",
                "Cookie: os=android; appver=9.5.61; osver=16"
            });
            const auto music_u = cookie("MUSIC_U");
            if (!music_u.empty()) headers.push_back("x-music-u: " + music_u);
        }
        auto response = http(url, &body, headers);
        if (spec.crypto == CryptoMode::Xeapi && !Json::accept(response))
            response = crypto::xeapi_response(response);
        return Json::parse(response);
    }
    void download(const std::string& url, const std::string& path) {
        if (path.empty()) throw ApiError("Download path is empty");
        auto parent = std::filesystem::path(path).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
        const auto temp = path + ".part";
        std::unique_ptr<FILE, decltype(&std::fclose)> file(std::fopen(temp.c_str(), "wb"), std::fclose);
        if (!file) throw ApiError("Cannot open download destination");
        try {
            http(url, nullptr, {}, file.get());
            if (std::fflush(file.get()) != 0 || std::ferror(file.get())) throw ApiError("Download write failed");
            file.reset();
            std::filesystem::rename(temp, path);
        } catch (...) {
            file.reset();
            std::remove(temp.c_str());
            throw;
        }
    }
};

ApiClient::ApiClient(ApiConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
ApiClient::~ApiClient() = default;
Json ApiClient::call(const std::string& method, Json arguments) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    try {
        if (!impl_->config.persist_cookies && !impl_->config.cookie_file.empty()) {
            curl_easy_setopt(impl_->curl, CURLOPT_COOKIELIST, "ALL");
            curl_easy_setopt(impl_->curl, CURLOPT_COOKIELIST, "RELOAD");
        }
        if (method == "fetch_xeapi_public_key") {
            impl_->fetch_key();
            // Key material is transport state, deliberately not exposed.
            return {{"code", 200}};
        }
        if (method == "download_img" || method == "download_song") {
            auto url = required(arguments, "url");
            if (method == "download_img")
                url += (url.find('?') == std::string::npos ? "?" : "&") +
                    std::string("param=") + opt(arguments, "width", "300") + "y" + opt(arguments, "high", "300");
            impl_->download(url, required(arguments, "path"));
            return {{"code", 200}};
        }
        auto response = impl_->request(plan_request(method, std::move(arguments)));
        const int code = service_code(response);
        if (code != 200 && code != 0 &&
            !(method == "login_qr_check" && code >= 800 && code <= 803))
            throw ApiError(service_message(response), code);
        if (method == "login_qr_create") {
            auto key = response.value("unikey", "");
            if (key.empty() && response.contains("data") && response["data"].is_object())
                key = response["data"].value("unikey", "");
            if (key.empty()) throw ApiError("QR response did not contain a key");
            response["unikey"] = key;
            response["qr_url"] = "https://music.163.com/login?codekey=" + key;
        }
        if (method == "logout") {
            curl_easy_setopt(impl_->curl, CURLOPT_COOKIELIST, "ALL");
            impl_->xeapi_key = nullptr;
            impl_->flush();
        }
        return response;
    } catch (const Json::exception& error) {
        throw ApiError(std::string("Unexpected API response: ") + error.what());
    }
}
Json ApiClient::search(std::string keywords, int type, int offset, int limit) {
    return call("search", {{"keywords", std::move(keywords)}, {"types", type}, {"offset", offset}, {"limit", limit}});
}
Json ApiClient::song_urls(const std::vector<std::uint64_t>& ids, std::string level) {
    // Background decoding currently supports MP3. Legacy bitrate requests avoid
    // the service returning AAC despite a v1 encodeType hint.
    if (level == "standard" || level == "higher" || level == "exhigh") {
        return call("songs_url", {{"ids", ids},
            {"br", level == "higher" ? 192000 : level == "exhigh" ? 320000 : 128000}});
    }
    return call("songs_url_v1", {{"ids", ids}, {"level", std::move(level)}});
}
Json ApiClient::song_details(const std::vector<std::uint64_t>& ids) { return call("songs_detail", {{"ids", ids}}); }
Json ApiClient::playlist(std::uint64_t id) { return call("song_list_detail", {{"songlist_id", id}}); }
Json ApiClient::login_status() { return call("login_status"); }
Json ApiClient::qr_create() { return call("login_qr_create"); }
Json ApiClient::qr_check(const std::string& key) { return call("login_qr_check", {{"key", key}}); }
void ApiClient::download(const std::string& url, const std::string& path) {
    std::lock_guard<std::mutex> lock(impl_->mutex); impl_->download(url, path);
}
void ApiClient::set_proxy(const std::string& proxy) {
    std::lock_guard<std::mutex> lock(impl_->mutex); impl_->config.proxy = proxy;
}
void ApiClient::flush_cookies() { std::lock_guard<std::mutex> lock(impl_->mutex); impl_->flush(); }
void ApiClient::clear_session() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    curl_easy_setopt(impl_->curl, CURLOPT_COOKIELIST, "ALL"); impl_->xeapi_key = nullptr; impl_->flush();
}
} // namespace netease
