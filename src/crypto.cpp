// SPDX-License-Identifier: GPL-3.0-or-later
// Protocol compatibility based on gmg137/netease-cloud-music-api 2.2.0,
// Copyright (C) 2019-2026 gmg137. See THIRD_PARTY_NOTICES.md.
#include "netease/crypto.hpp"
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/md5.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <zlib.h>
#include <algorithm>
#include <array>
#include <climits>
#include <cerrno>
#ifdef __SWITCH__
#include <switch.h>
#else
#include <sys/random.h>
#endif

namespace netease::crypto {
namespace {
template <typename T, void (*Init)(T*), void (*Free)(T*)> struct Context {
    T value;
    Context() { Init(&value); }
    ~Context() { Free(&value); }
    T* get() { return &value; }
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
};
constexpr char ekey[] = "e82ckenh8dichen8";
const std::string& xkey() {
    static const std::string key = unhex("ab1d5a430f6bb04a3f01e81ddd72bd916d5ce591248ac128714806d7f8fb1b84");
    return key;
}
void check(bool ok, const char* reason) {
    if (!ok) throw ApiError(std::string("Crypto: ") + reason);
}
const unsigned char* u(const std::string& s) {
    return reinterpret_cast<const unsigned char*>(s.data());
}
unsigned char* u(std::string& s) { return reinterpret_cast<unsigned char*>(s.data()); }
int rng(void*, unsigned char* bytes, std::size_t size) {
#ifdef __SWITCH__
    randomGet(bytes, size);
    return 0;
#else
    std::size_t offset = 0;
    while (offset < size) {
        const auto result = getrandom(bytes + offset, size - offset, 0);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) return -1;
        offset += static_cast<std::size_t>(result);
    }
    return 0;
#endif
}
std::string hmac(const std::string& key, const std::string& data) {
    std::string output(32, '\0');
    check(mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                         u(key), key.size(), u(data), data.size(), u(output)) == 0, "HMAC-SHA256 failed");
    return output;
}
std::string percent(const std::string& s) {
    const char* digits = "0123456789ABCDEF";
    std::string output;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            output += static_cast<char>(c);
        else { output += '%'; output += digits[c >> 4]; output += digits[c & 15]; }
    }
    return output;
}
std::string encrypt_s(const std::string& dynamic_key, const Json& state) {
    auto peer_bytes = unbase64(state.at("publicKey").get<std::string>());
    check(peer_bytes.size() == 32, "X25519 public key must be 32 bytes");
    Context<mbedtls_ecp_group, mbedtls_ecp_group_init, mbedtls_ecp_group_free> group;
    Context<mbedtls_ecp_point, mbedtls_ecp_point_init, mbedtls_ecp_point_free> peer, pub_point;
    Context<mbedtls_mpi, mbedtls_mpi_init, mbedtls_mpi_free> private_key, secret;
    check(mbedtls_ecp_group_load(group.get(), MBEDTLS_ECP_DP_CURVE25519) == 0 &&
          mbedtls_ecdh_gen_public(group.get(), private_key.get(), pub_point.get(), rng, nullptr) == 0,
          "X25519 keygen failed");
    std::string pub(32, '\0');
    check(mbedtls_mpi_write_binary_le(&pub_point.value.X, u(pub), pub.size()) == 0 &&
          mbedtls_mpi_read_binary_le(&peer.value.X, u(peer_bytes), peer_bytes.size()) == 0 &&
          mbedtls_mpi_lset(&peer.value.Z, 1) == 0, "X25519 public key import failed");
    check(mbedtls_ecdh_compute_shared(group.get(), secret.get(), peer.get(), private_key.get(), rng, nullptr) == 0,
          "X25519 derive failed");
    std::string shared(32, '\0');
    check(mbedtls_mpi_write_binary_le(secret.get(), u(shared), shared.size()) == 0, "X25519 export failed");
    auto key = hmac(hmac(std::string(32, '\0'), shared), pub + '\1').substr(0, 16);
    auto iv = random_bytes(12);
    auto text = base64(dynamic_key) + "|android|" + state.at("sk").get<std::string>();
    Context<mbedtls_gcm_context, mbedtls_gcm_init, mbedtls_gcm_free> ctx;
    check(mbedtls_gcm_setkey(ctx.get(), MBEDTLS_CIPHER_ID_AES, u(key), 128) == 0, "AES-GCM init failed");
    std::string encrypted(text.size(), '\0'), tag(16, '\0');
    check(mbedtls_gcm_crypt_and_tag(ctx.get(), MBEDTLS_GCM_ENCRYPT, text.size(),
          u(iv), iv.size(), nullptr, 0, u(text), u(encrypted), tag.size(), u(tag)) == 0,
          "AES-GCM encrypt failed");
    return pub + iv + encrypted + tag;
}
} // namespace

std::string random_bytes(std::size_t count) {
    check(count <= INT_MAX, "random request too large");
    std::string result(count, '\0');
    check(rng(nullptr, u(result), count) == 0, "secure random generator unavailable");
    return result;
}
std::string hex(const std::string& input, bool upper) {
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    std::string result;
    result.reserve(input.size() * 2);
    for (unsigned char c : input) { result += digits[c >> 4]; result += digits[c & 15]; }
    return result;
}
std::string unhex(const std::string& input) {
    check(input.size() % 2 == 0, "invalid hex length");
    auto nibble = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        throw ApiError("Crypto: invalid hex digit");
    };
    std::string result;
    result.reserve(input.size() / 2);
    for (std::size_t i = 0; i < input.size(); i += 2)
        result += static_cast<char>((nibble(input[i]) << 4) | nibble(input[i + 1]));
    return result;
}
std::string md5(const std::string& input) {
    std::string result(16, '\0');
    check(mbedtls_md5_ret(u(input), input.size(), u(result)) == 0, "MD5 failed");
    return hex(result);
}
std::string base64(const std::string& input) {
    check(input.size() <= INT_MAX, "base64 input too large");
    std::string output(4 * ((input.size() + 2) / 3) + 1, '\0');
    std::size_t length = 0;
    check(mbedtls_base64_encode(u(output), output.size(), &length, u(input), input.size()) == 0, "base64 encode failed");
    output.resize(length);
    return output;
}
std::string unbase64(const std::string& input) {
    if (input.empty()) return {};
    check(input.size() % 4 == 0 && input.size() <= INT_MAX, "invalid base64 length");
    std::string output(input.size() / 4 * 3 + 1, '\0');
    std::size_t length = 0;
    check(mbedtls_base64_decode(u(output), output.size(), &length, u(input), input.size()) == 0, "invalid base64 input");
    output.resize(length);
    return output;
}
std::string form_encode(const Json& params) {
    std::string result;
    for (auto it = params.begin(); it != params.end(); ++it) {
        if (!result.empty()) result += '&';
        result += percent(it.key()) + "=" + percent(it.value().is_string() ? it.value().get<std::string>() : it.value().dump());
    }
    return result;
}
std::string aes(const std::string& data, const std::string& key, const std::string& iv, bool decrypt) {
    check(data.size() <= INT_MAX, "AES input too large");
    check(key.size() == 16 || key.size() == 32, "AES key must be 16 or 32 bytes");
    check(iv.empty() || iv.size() == 16, "AES IV must be 16 bytes");
    Context<mbedtls_aes_context, mbedtls_aes_init, mbedtls_aes_free> ctx;
    check((decrypt ? mbedtls_aes_setkey_dec(ctx.get(), u(key), key.size() * 8) :
                     mbedtls_aes_setkey_enc(ctx.get(), u(key), key.size() * 8)) == 0, "AES init failed");
    std::string input = data;
    if (!decrypt) {
        const auto padding = static_cast<char>(16 - input.size() % 16);
        input.append(static_cast<unsigned char>(padding), padding);
    }
    check(!input.empty() && input.size() % 16 == 0, "Invalid AES ciphertext length");
    std::string output(input.size(), '\0');
    const auto mode = decrypt ? MBEDTLS_AES_DECRYPT : MBEDTLS_AES_ENCRYPT;
    if (iv.empty()) {
        for (std::size_t offset = 0; offset < input.size(); offset += 16)
            check(mbedtls_aes_crypt_ecb(ctx.get(), mode, u(input) + offset, u(output) + offset) == 0, "AES-ECB failed");
    } else {
        auto mutable_iv = iv;
        check(mbedtls_aes_crypt_cbc(ctx.get(), mode, input.size(), u(mutable_iv), u(input), u(output)) == 0, "AES-CBC failed");
    }
    if (decrypt) {
        const auto padding = static_cast<unsigned char>(output.back());
        check(padding > 0 && padding <= 16 && padding <= output.size(), "Invalid AES padding");
        check(std::all_of(output.end() - padding, output.end(), [padding](unsigned char c) { return c == padding; }),
              "Invalid AES padding");
        output.resize(output.size() - padding);
    }
    return output;
}
std::string weapi(const std::string& json, const std::string& test_secret) {
    std::string secret = test_secret;
    if (secret.empty()) {
        constexpr char base62[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        for (unsigned char c : random_bytes(16)) secret += base62[c % 62];
    }
    check(secret.size() == 16, "WEAPI key must be 16 bytes");
    const auto first = base64(aes(json, "0CoJUm6Qyw8W8jud", "0102030405060708"));
    const auto params = base64(aes(first, secret, "0102030405060708"));
    constexpr char pem[] =
        "-----BEGIN PUBLIC KEY-----\n"
        "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDgtQn2JZ34ZC28NWYpAUd98iZ37BUr"
        "X/aKzmFbt7clFSs6sXqHauqKWqdtLkF2KexO40H1YTX8z2lSgBBOAxLsvaklV8k4cBFK"
        "9snQXE9/DDaFt6Rr7iVZMldczhC0JNgTz+SHXT6CBHuX3e9SdB1Ua44oncaTWz7OBGLbCiK45wIDAQAB\n"
        "-----END PUBLIC KEY-----";
    Context<mbedtls_pk_context, mbedtls_pk_init, mbedtls_pk_free> key;
    check(mbedtls_pk_parse_public_key(key.get(), reinterpret_cast<const unsigned char*>(pem), sizeof(pem)) == 0,
          "RSA public key import failed");
    std::reverse(secret.begin(), secret.end());
    std::string input(128 - secret.size(), '\0'); input += secret;
    std::string output(128, '\0');
    check(mbedtls_rsa_public(mbedtls_pk_rsa(key.value), u(input), u(output)) == 0, "RSA encrypt failed");
    return form_encode({{"params", params}, {"encSecKey", hex(output)}});
}
std::string eapi(const std::string& path, const std::string& json) {
    const auto digest = md5("nobody" + path + "use" + json + "md5forencrypt");
    return form_encode({{"params", hex(aes(path + "-36cd479b6b5-" + json + "-36cd479b6b5-" + digest, ekey), true)}});
}
std::string xeapi_signature(const std::string& timestamp, const std::string& nonce) {
    return base64(hmac("mUHCwVNWJbunMqAHf5MImuirT6plvs6VSFW62MGHstFQxhBGdEoIhLItH3djc4+FB/OKty3+lL2rGeoFBpVe5g==",
                       timestamp + nonce));
}
Json xeapi_public_key(const std::string& encrypted) {
    return Json::parse(aes(unbase64(encrypted), xkey(), {}, true));
}
std::string xeapi(const Json& params, const Json& state) {
    auto fields = params; fields.erase("e_r");
    auto plain = Json({{"body", base64(form_encode(fields))}, {"queryString", "e_r=true"}}).dump();
    auto encrypted = aes(plain, xkey());
    auto random = random_bytes(16);
    for (std::size_t i = 0; i < encrypted.size(); ++i) encrypted[i] ^= random[i & 15];
    auto b64 = base64(encrypted);
    const auto rotation = (static_cast<unsigned char>(random[0]) & 15) % b64.size();
    auto mid = random + b64.substr(rotation) + b64.substr(0, rotation);
    auto key = random_bytes(16);
    auto version = state.at("version").is_string() ? state.at("version").get<std::string>() : state.at("version").dump();
    return form_encode({{"B", base64(aes(mid, key))},
                        {"S", base64(encrypt_s(key, state))},
                        {"R", base64(aes(version + "|", xkey()))}});
}
std::string xeapi_response(const std::string& bytes) {
    auto decoded = aes(bytes, ekey, {}, true);
    if (decoded.size() < 2 || static_cast<unsigned char>(decoded[0]) != 0x1f ||
        static_cast<unsigned char>(decoded[1]) != 0x8b) return decoded;
    z_stream stream{};
    check(inflateInit2(&stream, 16 + MAX_WBITS) == Z_OK, "gzip init failed");
    stream.next_in = reinterpret_cast<Bytef*>(decoded.data());
    stream.avail_in = static_cast<uInt>(decoded.size());
    std::array<char, 16384> block{};
    std::string output;
    int status = Z_OK;
    do {
        stream.next_out = reinterpret_cast<Bytef*>(block.data());
        stream.avail_out = block.size();
        status = inflate(&stream, Z_NO_FLUSH);
        output.append(block.data(), block.size() - stream.avail_out);
        if (output.size() > 32 * 1024 * 1024) { inflateEnd(&stream); throw ApiError("Response exceeds limit"); }
    } while (status == Z_OK);
    inflateEnd(&stream);
    check(status == Z_STREAM_END, "gzip response corrupt");
    return output;
}
} // namespace netease::crypto
