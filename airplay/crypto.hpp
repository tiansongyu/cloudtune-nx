#pragma once
/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cloudtune::airplay {
// The protocol's published compatibility RSA key is not a user credential.
bool unwrapSessionKey(const std::string& base64, std::array<uint8_t, 16>& key);
std::string challengeResponse(const std::string& challenge, uint32_t ipv4NetworkOrder,
                              const std::array<uint8_t, 6>& deviceId);
bool decodeBase64(const std::string& text, std::vector<uint8_t>& result);
bool decryptAudio(const uint8_t* bytes, size_t length, const std::array<uint8_t, 16>& key,
                  const std::array<uint8_t, 16>& iv, std::vector<uint8_t>& result);
} // namespace cloudtune::airplay
