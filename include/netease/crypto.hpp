// SPDX-License-Identifier: GPL-3.0-or-later
// Protocol compatibility based on gmg137/netease-cloud-music-api 2.2.0 (GPLv3).
#pragma once
#include "api.hpp"

namespace netease::crypto {
std::string md5(const std::string& input);
std::string base64(const std::string& input);
std::string unbase64(const std::string& input);
std::string hex(const std::string& input, bool upper = false);
std::string unhex(const std::string& input);
std::string random_bytes(std::size_t count);
std::string form_encode(const Json& params);
std::string aes(const std::string& data, const std::string& key,
                const std::string& iv = {}, bool decrypt = false);
std::string weapi(const std::string& json, const std::string& test_secret = {});
std::string eapi(const std::string& path, const std::string& json);
std::string xeapi_signature(const std::string& timestamp, const std::string& nonce);
Json xeapi_public_key(const std::string& encrypted);
std::string xeapi(const Json& params, const Json& key_state);
std::string xeapi_response(const std::string& bytes);
} // namespace netease::crypto
