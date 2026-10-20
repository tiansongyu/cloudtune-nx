// SPDX-License-Identifier: GPL-3.0-or-later
#include "netease/api.hpp"
#include "netease/crypto.hpp"
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>

using namespace netease;
static void require(bool ok, const char* reason) {
    if (!ok) throw std::runtime_error(reason);
}
template<class F> void rejects(F f) {
    bool threw = false;
    try { f(); } catch (const ApiError&) { threw = true; }
    require(threw, "Invalid input was accepted");
}
static void transport_case(const std::string& test, const std::string& proxy,
                           const std::string& ca, const std::string& output) {
    ApiConfig config;
    config.proxy = proxy;
    config.ca_bundle = ca;
    config.timeout_seconds = 5;
    config.persist_cookies = false;
    ApiClient api(config);
    api.adopt_session("# Netscape HTTP Cookie File\n"
        ".music.163.com\tTRUE\t/\tTRUE\t0\tMUSIC_U\tsynthetic-music-session\n"
        "unrelated.invalid\tFALSE\t/\tFALSE\t0\tMUSIC_U\tsynthetic-unrelated-session\n");
    if (test == "redirect") {
        try { api.call("like_v1", {{"songid", 1}}); }
        catch (const ApiError& error) {
            require(error.code == 307, "Redirect did not report HTTP 307");
            return;
        }
        throw std::runtime_error("Authenticated API request followed an external redirect");
    }
    if (test == "cookie-scope") {
        api.call("like_v1", {{"songid", 1}});
        return; // The fixture checks the received header without printing it.
    }
    if (test == "cookie-path") {
        api.adopt_session("# Netscape HTTP Cookie File\n"
            ".music.163.com\tTRUE\t/download-only\tTRUE\t0\tMUSIC_U\tsynthetic-path-session\n");
        api.call("like_v1", {{"songid", 1}});
        return;
    }
    if (test == "cookie-expired") {
        api.adopt_session("# Netscape HTTP Cookie File\n"
            ".music.163.com\tTRUE\t/\tTRUE\t1\tMUSIC_U\tsynthetic-expired-session\n"
            ".music.163.com\tTRUE\t/\tTRUE\t1\t__csrf\tsynthetic-expired-csrf\n");
        api.song_urls({1}, "lossless");
        return;
    }
    if (test == "malformed") {
        try { api.login_status(); }
        catch (const ApiError& error) {
            require(std::string(error.what()) == "Unexpected API response",
                    "API parse error exposed response contents");
            return;
        }
        throw std::runtime_error("Malformed API response was accepted");
    }
    if (test == "code-suffix" || test == "code-overflow") {
        try { api.login_status(); }
        catch (const ApiError& error) {
            require(std::string(error.what()) == "Unexpected API status code",
                    "Malformed API status was not rejected safely");
            return;
        }
        throw std::runtime_error("Malformed API status was accepted as success");
    }
    if (test == "download") {
        api.call("like_v1", {{"songid", 1}});
        api.download("https://music.163.com/fixture-artwork", output);
        std::ifstream file(output);
        const std::string body((std::istreambuf_iterator<char>(file)), {});
        require(body == "fixture-image", "CDN download redirect stopped working");
        return;
    }
    throw std::runtime_error("Unknown transport fixture");
}
int main(int argc, char** argv) {
    try {
        if (argc == 6 && std::string(argv[1]) == "--transport") {
            transport_case(argv[2], argv[3], argv[4], argv[5]);
            return 0;
        }
        const std::string scoped_jar =
            "# Netscape HTTP Cookie File\n"
            "#HttpOnly_.MUSIC.163.COM\tTRUE\t/\tTRUE\t0\tMUSIC_U\tsynthetic-root\r\n"
            ".music.163.com\tTRUE\t/download\tTRUE\t0\tMUSIC_U\tsynthetic-path\n"
            "unrelated.invalid\tFALSE\t/\tFALSE\t0\tMUSIC_U\tsynthetic-other\n"
            ".music.163.com\tTRUE\t/\tTRUE\t1\t__csrf\tsynthetic-expired\n"
            ".music.163.com\tTRUE\t/\tTRUE\t0tail\t__csrf\tsynthetic-invalid\n";
        require(session_auth_cookie(scoped_jar, "MUSIC_U") == "synthetic-root",
                "Session identity ignored domain/path scope or HttpOnly");
        require(session_auth_cookie(scoped_jar, "__csrf").empty(),
                "Expired or malformed cookie expiry became authentication");
        require(session_auth_cookie(
                    ".music.163.com\tTRUE\t/\tTRUE\t0\tMUSIC_U\tbad\rheader\n", "MUSIC_U").empty(),
                "Cookie control characters became authentication");
        require(session_auth_cookie(
                    ".music.163.com\tTRUE\t/\tTRUE\t184467440737095516160\tMUSIC_U\tbad\n",
                    "MUSIC_U").empty(),
                "Overflowing cookie expiry became authentication");
        const std::string deleted_cookie =
            ".music.163.com\tTRUE\t/\tTRUE\t0\tMUSIC_U\tsynthetic-original\n"
            ".music.163.com\tTRUE\t/\tTRUE\t1\tMUSIC_U\tsynthetic-deleted\n";
        require(session_auth_cookie(deleted_cookie, "MUSIC_U").empty(),
                "A later expired cookie resurrected an earlier session");
        ApiConfig memory_config;
        memory_config.persist_cookies = false;
        ApiClient memory(memory_config);
        memory.adopt_session(deleted_cookie);
        require(session_auth_cookie(deleted_cookie, "MUSIC_U") ==
                    session_auth_cookie(memory.session_cookies(), "MUSIC_U"),
                "Raw and curl-normalized sessions disagree about authentication");
        // Expected vectors independently produced with Python cryptography/OpenSSL.
        require(crypto::md5("abc") == "900150983cd24fb0d6963f7d28e17f72", "MD5 vector mismatch");
        require(crypto::base64("Switch") == "U3dpdGNo", "Base64 vector mismatch");
        require(crypto::unbase64("U3dpdGNo") == "Switch", "Base64 decode mismatch");
        require(crypto::hex(crypto::aes("Hello, Switch!", "0123456789abcdef", "0102030405060708")) ==
                "058d393ea6273af327b26245f6afbf14", "AES-CBC vector mismatch");
        require(crypto::aes(crypto::unhex("058d393ea6273af327b26245f6afbf14"),
                "0123456789abcdef", "0102030405060708", true) == "Hello, Switch!", "AES decrypt mismatch");
        const auto weapi = crypto::weapi(R"({"ids":"[347230]"})", "abcdefghijklmnop");
        require(weapi.find("params=BToYSS6FhM7%2FsLgbhkqkHKPEnvpWBIYCIn2uaqDjc2F7oK%2BOoIStehec5dQazgGG") != std::string::npos,
                "WEAPI AES vector mismatch");
        require(weapi.find("encSecKey=d15a1683c992095d0c234c19966605c5c5964911268bbeda8cb8d08d834913e59d53b32358903a121b5fca784c1f5ae44951fd02524df58ecc98e52cc7cf8689b42c2e93ddf05b0592512d87f5960467e2f086c018849d76014d323500e30f13ef4cafbb0cf5a66731a3f1776c75ca35d0062dac70a3e33245afabcf47938487") != std::string::npos,
                "WEAPI RSA vector mismatch");
        require(crypto::eapi("/api/song/enhance/player/url", R"({"ids":"[347230]"})") ==
                "params=FA90B329E9614F79E79598F37DC2EDB430F8378D2A2796338F0BFDEAEF824A22975CDA9D96D79E6DC4A59218CDB8199FCEA23502D1BFF8CEBC8A92B152AA8FF475156A12019E8A3138E79883655C1FAEC98048B1D2841100692427F138D86B31DD5F2CCC394AD624F1463A28ECD88AB4",
                "EAPI vector mismatch");
        const auto secret = crypto::random_bytes(32);
        require(secret.size() == 32 && secret != crypto::random_bytes(32), "Random bytes failure");
        rejects([] { crypto::aes("bad", "0123456789abcdef", {}, true); });
        rejects([] { crypto::unhex("zz"); });
        rejects([] { crypto::unbase64("$$$$"); });

        const auto all = api_methods();
        require(all.size() == 46, "API catalog coverage changed");
        require(std::set<std::string>(all.begin(), all.end()).size() == all.size(), "Duplicate API methods");
        const Json args = {{"id", 347230}, {"songid", 347230}, {"song_id", 347230}, {"sid", 347230},
            {"pid", 3778678}, {"uid", 1}, {"rid", 1}, {"ids", {347230}}, {"key", "test"},
            {"username", "example@example.com"}, {"password", "test"}, {"phone", "13800000000"},
            {"captcha", "1234"}, {"keywords", "test"}};
        for (const auto& method : all) {
            if (method == "fetch_xeapi_public_key" || method == "download_img" || method == "download_song") continue;
            auto plan = plan_request(method, args);
            require(plan.path.rfind("/", 0) == 0 && plan.params.is_object(), "Malformed request plan");
        }
        require(plan_request("songs_detail", args).params["c"] == R"([{"id":347230}])", "Song detail JSON encoding mismatch");
        for (const auto* removed : {"login", "login_cellphone", "captcha"})
            rejects([&] { plan_request(removed, args); });
        require(plan_request("search_lyrics", args).params["type"] == "1006", "Search alias mismatch");
        require(plan_request("top_song_list_highquality", {{"lasttime", 1720000000123ULL}}).params["lasttime"] == "1720000000123",
                "Pagination timestamp was truncated");
        require(plan_request("songs_url_v1", {{"ids", {347230}}, {"level", "sky"}}).params["immerseType"] == "c51",
                "AudioVivid option missing");
        require(plan_request("like_v1", args).crypto == CryptoMode::Xeapi, "Like v1 requires XEAPI");
        rejects([] { plan_request("missing"); });
        rejects([] { plan_request("user_song_list"); });
        rejects([] { plan_request("songs_detail", {{"ids", Json::array()}}); });
        rejects([] { plan_request("search", {{"keywords", "test"}, {"offset", -1}}); });
        rejects([] { plan_request("search", {{"keywords", "test"}, {"offset", "12tail"}}); });
        rejects([] { plan_request("search", {{"keywords", "test"}, {"limit", " 20"}}); });
        rejects([] { plan_request("songs_url_v1", {{"ids", {1}}, {"level", "invalid"}}); });

        if (argc > 1) {
            // Fixed RFC 7748 Alice public key; the Python independent verifier
            // decrypts S using its private scalar and validates B/R.
            auto key = Json{{"version", "test-1"}, {"sk", "fixture-sk"},
                {"publicKey", crypto::base64(crypto::unhex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"))}};
            std::cout << Json{{"xeapi", crypto::xeapi({{"trackId", "347230"}, {"like", "true"}}, key)},
                {"signature", crypto::xeapi_signature("1700000000000", "1234567890123456")}}.dump() << '\n';
        } else std::cout << "API planning and independent cryptographic vectors passed (46 methods)\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
