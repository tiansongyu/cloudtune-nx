/* SPDX-License-Identifier: GPL-3.0-or-later */
// RAOP transformations follow shairport-sync common.c / rtsp.c / player.c
// at cf6998daa1876c5a8f97128a960c8afc0e731841 (MIT; vendor/SHAIRPORT-LICENSE).
#include "crypto.hpp"
#include "vendor/shairport_rsa_key.h"
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/version.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#ifdef __SWITCH__
extern "C" {
#include <switch/kernel/random.h>
}
#else
#include <sys/random.h>
#endif

namespace cloudtune::airplay {
namespace {
int randomBytes(void*, unsigned char* bytes, size_t size) {
#ifdef __SWITCH__
    randomGet(bytes, size);
    return 0;
#else
    size_t offset = 0;
    while (offset < size) {
        const auto got = getrandom(bytes + offset, size - offset, 0);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return -1;
        offset += static_cast<size_t>(got);
    }
    return 0;
#endif
}
struct PrivateKey {
    mbedtls_pk_context value;
    PrivateKey() { mbedtls_pk_init(&value); }
    ~PrivateKey() { mbedtls_pk_free(&value); }
    bool load() {
#if MBEDTLS_VERSION_MAJOR >= 3
        return mbedtls_pk_parse_key(&value,
            reinterpret_cast<const unsigned char*>(airplay_compatibility_rsa_key),
            sizeof(airplay_compatibility_rsa_key), nullptr, 0, randomBytes, nullptr) == 0;
#else
        return mbedtls_pk_parse_key(&value,
            reinterpret_cast<const unsigned char*>(airplay_compatibility_rsa_key),
            sizeof(airplay_compatibility_rsa_key), nullptr, 0) == 0;
#endif
    }
};
}

bool decodeBase64(const std::string& text, std::vector<uint8_t>& result) {
    result.clear();
    if (text.empty() || text.size() > 1024) return false;
    for (unsigned char c : text)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=')) return false;
    std::string padded = text;
    while (padded.size() % 4) padded += '=';
    result.resize(padded.size() * 3 / 4 + 4);
    size_t length = 0;
    if (mbedtls_base64_decode(result.data(), result.size(), &length,
            reinterpret_cast<const unsigned char*>(padded.data()), padded.size()) != 0) {
        result.clear();
        return false;
    }
    result.resize(length);
    return true;
}

bool unwrapSessionKey(const std::string& text, std::array<uint8_t, 16>& key) {
    key.fill(0);
    std::vector<uint8_t> encrypted;
    PrivateKey rsa;
    if (!decodeBase64(text, encrypted) || !rsa.load() ||
        encrypted.size() != mbedtls_pk_get_len(&rsa.value)) return false;
    auto* context = mbedtls_pk_rsa(rsa.value);
    mbedtls_rsa_set_padding(context, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA1);
    size_t length = 0;
#if MBEDTLS_VERSION_MAJOR >= 3
    const int result = mbedtls_rsa_rsaes_oaep_decrypt(context, randomBytes, nullptr,
        nullptr, 0, &length, encrypted.data(), key.data(), key.size());
#else
    const int result = mbedtls_rsa_rsaes_oaep_decrypt(context, randomBytes, nullptr,
        MBEDTLS_RSA_PRIVATE, nullptr, 0, &length, encrypted.data(), key.data(), key.size());
#endif
    if (result != 0 || length != key.size()) { key.fill(0); return false; }
    return true;
}

std::string challengeResponse(const std::string& challenge, uint32_t address,
                              const std::array<uint8_t, 6>& deviceId) {
    std::vector<uint8_t> decoded;
    if (!decodeBase64(challenge, decoded) || decoded.empty() || decoded.size() > 16) return {};
    std::array<uint8_t, 32> payload{};
    std::memcpy(payload.data(), decoded.data(), decoded.size());
    std::memcpy(payload.data() + decoded.size(), &address, sizeof(address));
    std::memcpy(payload.data() + decoded.size() + sizeof(address), deviceId.data(), deviceId.size());
    PrivateKey rsa;
    if (!rsa.load()) return {};
    auto* context = mbedtls_pk_rsa(rsa.value);
    mbedtls_rsa_set_padding(context, MBEDTLS_RSA_PKCS_V15, MBEDTLS_MD_NONE);
    std::vector<uint8_t> signature(mbedtls_pk_get_len(&rsa.value));
#if MBEDTLS_VERSION_MAJOR >= 3
    const int result = mbedtls_rsa_rsassa_pkcs1_v15_sign(context, randomBytes, nullptr,
        MBEDTLS_MD_NONE, payload.size(), payload.data(), signature.data());
#else
    const int result = mbedtls_rsa_rsassa_pkcs1_v15_sign(context, randomBytes, nullptr,
        MBEDTLS_RSA_PRIVATE, MBEDTLS_MD_NONE, payload.size(), payload.data(), signature.data());
#endif
    if (result != 0) return {};
    std::string encoded((signature.size() + 2) / 3 * 4 + 1, '\0');
    size_t length = 0;
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(encoded.data()), encoded.size(),
                            &length, signature.data(), signature.size()) != 0) return {};
    encoded.resize(length);
    while (!encoded.empty() && encoded.back() == '=') encoded.pop_back();
    return encoded;
}

bool decryptAudio(const uint8_t* bytes, size_t length, const std::array<uint8_t, 16>& key,
                  const std::array<uint8_t, 16>& originalIv, std::vector<uint8_t>& output) {
    if (!bytes || !length || length > 65536) return false;
    output.assign(bytes, bytes + length);
    std::array<uint8_t, 16> iv = originalIv;
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    int result = mbedtls_aes_setkey_dec(&aes, key.data(), 128);
    const size_t encryptedLength = length & ~size_t(15);
    if (!result && encryptedLength)
        result = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, encryptedLength,
                                     iv.data(), bytes, output.data());
    mbedtls_aes_free(&aes);
    if (result) { output.clear(); return false; }
    return true; // The incomplete final block is deliberately cleartext.
}
} // namespace cloudtune::airplay
