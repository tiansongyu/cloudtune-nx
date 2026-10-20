// SPDX-License-Identifier: GPL-3.0-or-later
#include "netease/api.hpp"
#include "netease/crypto.hpp"
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
int main(int argc, char**) {
    try {
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
        require(all.size() == 49, "API catalog coverage changed");
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
        require(plan_request("login", args).params["password"] == crypto::md5("test"), "Password hashing mismatch");
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
        rejects([] { plan_request("songs_url_v1", {{"ids", {1}}, {"level", "invalid"}}); });

        if (argc > 1) {
            // Fixed RFC 7748 Alice public key; the Python independent verifier
            // decrypts S using its private scalar and validates B/R.
            auto key = Json{{"version", "test-1"}, {"sk", "fixture-sk"},
                {"publicKey", crypto::base64(crypto::unhex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"))}};
            std::cout << Json{{"xeapi", crypto::xeapi({{"trackId", "347230"}, {"like", "true"}}, key)},
                {"signature", crypto::xeapi_signature("1700000000000", "1234567890123456")}}.dump() << '\n';
        } else std::cout << "API planning and independent cryptographic vectors passed (49 methods)\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
