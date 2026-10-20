// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace netease {
using Json = nlohmann::json;

struct ApiConfig {
    std::string cookie_file;
    std::string proxy;
    std::string ca_bundle;
    long timeout_seconds = 30;
    // Background services must read the GUI-owned session without overwriting it.
    bool persist_cookies = true;
};

class ApiError : public std::runtime_error {
public:
    ApiError(std::string message, int status = 0)
        : std::runtime_error(std::move(message)), code(status) {}
    int code;
};

enum class CryptoMode { Weapi, Eapi, Xeapi };

// Pure request construction is exposed for deterministic tests and API tooling.
struct RequestSpec {
    std::string path;
    Json params = Json::object();
    CryptoMode crypto = CryptoMode::Weapi;
    bool append_csrf = true;
};
RequestSpec plan_request(const std::string& method, Json arguments = Json::object());
std::vector<std::string> api_methods();

// Calls are synchronous and serialized. Run them on a worker, never the UI thread.
// Responses preserve the service JSON schema. Errors include HTTP / service codes.
class ApiClient {
public:
    explicit ApiClient(ApiConfig config = {});
    ~ApiClient();
    ApiClient(const ApiClient&) = delete;
    ApiClient& operator=(const ApiClient&) = delete;

    Json call(const std::string& method, Json arguments = Json::object());
    Json search(std::string keywords, int type = 1, int offset = 0, int limit = 30);
    Json song_urls(const std::vector<std::uint64_t>& ids, std::string level = "standard");
    Json song_details(const std::vector<std::uint64_t>& ids);
    Json playlist(std::uint64_t id);
    Json login_status();
    // Adds qr_url alongside the service unikey.
    Json qr_create();
    // Codes 800 expired, 801 waiting, 802 scanned, 803 authorized are returned.
    Json qr_check(const std::string& key);

    void download(const std::string& url, const std::string& path);
    void set_proxy(const std::string& proxy);
    void flush_cookies();
    void clear_session();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace netease
