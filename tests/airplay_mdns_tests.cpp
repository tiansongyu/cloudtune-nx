/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../airplay/mdns.hpp"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
using Bytes = std::vector<uint8_t>;
void require(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "mDNS test failed: %s\n", message);
        std::exit(1);
    }
}
void put16(Bytes &bytes, uint16_t value) {
    bytes.push_back(static_cast<uint8_t>(value >> 8));
    bytes.push_back(static_cast<uint8_t>(value));
}
uint16_t get16(const uint8_t *bytes) { return (uint16_t(bytes[0]) << 8) | bytes[1]; }
std::string readName(const Bytes &bytes, size_t &position) {
    std::string result;
    while (position < bytes.size() && bytes[position]) {
        size_t size = bytes[position++];
        require(size <= 63 && position + size <= bytes.size(), "valid response name");
        if (!result.empty())
            result += '.';
        result.append(reinterpret_cast<const char *>(bytes.data() + position), size);
        position += size;
    }
    require(position < bytes.size(), "response name terminated");
    ++position;
    return result;
}
Bytes query() {
    Bytes bytes{0, 42, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0};
    for (const char *label : {"_raop", "_tcp", "local"}) {
        bytes.push_back(static_cast<uint8_t>(std::strlen(label)));
        bytes.insert(bytes.end(), label, label + std::strlen(label));
    }
    bytes.push_back(0);
    put16(bytes, 12);
    put16(bytes, 1);
    return bytes;
}
} // namespace
int main() {
    cloudtune::airplay::MdnsAdvertiser advertiser;
    require(!advertiser.start("Test", 7000, "not-a-device-id", htonl(INADDR_LOOPBACK)),
            "invalid device id rejected");
    require(advertiser.start("CloudTune Test", 7000, "020000000001", htonl(INADDR_LOOPBACK)),
            "loopback mDNS start");
    int client = socket(AF_INET, SOCK_DGRAM, 0);
    require(client >= 0, "client socket");
    sockaddr_in local{}, destination{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(client, reinterpret_cast<sockaddr *>(&local), sizeof(local)) == 0, "client bind");
    destination = local;
    destination.sin_port = htons(5353);
    const auto send = [&](const Bytes &bytes) {
        require(sendto(client, bytes.data(), bytes.size(), 0,
                       reinterpret_cast<sockaddr *>(&destination), sizeof(destination)) ==
                    static_cast<ssize_t>(bytes.size()), "send query");
        advertiser.tick();
    };
    Bytes malformed{0, 42, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0xC0, 12, 0, 12, 0, 1};
    send(malformed); // compression pointer cycle
    pollfd readable{client, POLLIN, 0};
    require(poll(&readable, 1, 30) == 0, "compression cycle receives no response");
    send(Bytes(1501, 0xFF));
    require(poll(&readable, 1, 30) == 0, "oversized datagram receives no response");
    send(query());
    require(poll(&readable, 1, 500) == 1, "PTR query gets response");
    Bytes response(1500);
    ssize_t received = recv(client, response.data(), response.size(), 0);
    require(received > 12, "DNS response length");
    response.resize(static_cast<size_t>(received));
    require(get16(response.data()) == 42 && get16(response.data() + 2) == 0x8400,
            "unicast response ID and authoritative flags");
    require(get16(response.data() + 6) == 4, "PTR/SRV/TXT/A record count");
    require(get16(response.data() + 4) == 1, "legacy unicast repeats the query");
    size_t position = 12;
    for (unsigned q = 0; q < get16(response.data() + 4); ++q) {
        require(readName(response, position) == "_raop._tcp.local" &&
                    get16(response.data() + position) == 12 &&
                    get16(response.data() + position + 2) == 1,
                "legacy question values");
        position += 4;
    }
    bool ptr = false, srv = false, txt = false, address = false;
    for (unsigned i = 0; i < 4; ++i) {
        auto owner = readName(response, position);
        require(position + 10 <= response.size(), "record header bounds");
        uint16_t type = get16(response.data() + position);
        uint16_t length = get16(response.data() + position + 8);
        require(get16(response.data() + position + 2) == 1,
                "legacy records omit cache-flush flag");
        require(get16(response.data() + position + 4) == 0 &&
                    get16(response.data() + position + 6) == 10,
                "legacy record TTL is ten seconds");
        position += 10;
        require(position + length <= response.size(), "record payload bounds");
        if (type == 12) {
            size_t cursor = position;
            ptr = owner == "_raop._tcp.local" &&
                  readName(response, cursor) == "020000000001@CloudTune Test._raop._tcp.local";
        } else if (type == 33) {
            srv = length >= 6 && get16(response.data() + position + 4) == 7000;
        } else if (type == 16) {
            std::string attributes(reinterpret_cast<const char *>(response.data() + position), length);
            txt = attributes.find("cn=0,1") != std::string::npos &&
                  attributes.find("sr=44100") != std::string::npos &&
                  attributes.find("md=0,2") != std::string::npos &&
                  attributes.find("vn=65537") != std::string::npos;
        } else if (type == 1) {
            const uint8_t loopback[]{127, 0, 0, 1};
            address = length == 4 && !std::memcmp(response.data() + position, loopback, 4);
        }
        position += length;
    }
    require(ptr && srv && txt && address && position == response.size(), "all DNS-SD values");
    advertiser.stop();
    require(advertiser.fd() == -1, "stop closes socket");
    close(client);
    std::puts("mDNS loopback PTR/SRV/TXT/A, bounded malformed input and lifecycle passed.");
}
