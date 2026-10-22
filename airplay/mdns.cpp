/* SPDX-License-Identifier: GPL-3.0-or-later */
#if defined(__SWITCH__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#endif
#include "mdns.hpp"
#include "udp.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace cloudtune::airplay {
namespace {
using Bytes = std::vector<uint8_t>;
using Clock = std::chrono::steady_clock;
constexpr size_t kPacketLimit = 1500, kQuestionLimit = 16;
constexpr const char *kService = "_raop._tcp.local";
constexpr const char *kEnumeration = "_services._dns-sd._udp.local";
uint16_t read16(const uint8_t *bytes) { return (uint16_t(bytes[0]) << 8) | bytes[1]; }
void write16(Bytes &out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}
void write32(Bytes &out, uint32_t value) {
    write16(out, static_cast<uint16_t>(value >> 16));
    write16(out, static_cast<uint16_t>(value));
}
std::string lower(std::string value) {
    for (char &ch : value)
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    return value;
}
bool name(Bytes &out, const std::string &value) {
    if (value.empty() || value.size() > 253)
        return false;
    size_t position = 0;
    while (position < value.size()) {
        size_t end = value.find('.', position);
        if (end == std::string::npos)
            end = value.size();
        size_t length = end - position;
        if (!length || length > 63)
            return false;
        out.push_back(static_cast<uint8_t>(length));
        out.insert(out.end(), value.begin() + position, value.begin() + end);
        position = end + 1;
    }
    out.push_back(0);
    return true;
}
// Compression pointers must point inside this datagram. The hop and expanded
// length limits reject cycles and excessive expansion before allocating.
bool readName(const uint8_t *packet, size_t length, size_t &position, std::string &out) {
    out.clear();
    size_t cursor = position, continuation = 0;
    unsigned jumps = 0, labels = 0;
    while (cursor < length) {
        uint8_t size = packet[cursor++];
        if (!size) {
            position = continuation ? continuation : cursor;
            return true;
        }
        if ((size & 0xC0) == 0xC0) {
            if (cursor >= length || ++jumps > 16)
                return false;
            size_t target = (size_t(size & 63) << 8) | packet[cursor++];
            if (target >= length)
                return false;
            if (!continuation)
                continuation = cursor;
            cursor = target;
            continue;
        }
        if ((size & 0xC0) || size > 63 || cursor + size > length || ++labels > 127)
            return false;
        if (!out.empty())
            out.push_back('.');
        if (out.size() + size > 253)
            return false;
        for (size_t i = 0; i < size; ++i) {
            char ch = static_cast<char>(packet[cursor++]);
            if (ch == '\0' || ch == '.')
                return false;
            out.push_back(ch);
        }
    }
    return false;
}
void record(Bytes &out, const std::string &owner, uint16_t type, bool unique, uint32_t ttl,
            const Bytes &data) {
    name(out, owner);
    write16(out, type);
    write16(out, unique ? 0x8001 : 1);
    write32(out, ttl);
    write16(out, static_cast<uint16_t>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
}
} // namespace

struct MdnsAdvertiser::Impl {
    int socket = -1;
    uint16_t port = 0;
    uint32_t address = 0;
    std::string instance, host;
    Clock::time_point nextAnnouncement{}, lastReply{};
    unsigned announcements = 0;

    Bytes response(uint32_t ttl, uint16_t id, bool enumerate,
                   const Bytes &questions, uint16_t questionCount, bool legacy) const {
        Bytes packet;
        packet.reserve(1200);
        write16(packet, id);
        write16(packet, 0x8400);
        write16(packet, questionCount);
        write16(packet, enumerate ? 5 : 4);
        write16(packet, 0);
        write16(packet, 0);
        packet.insert(packet.end(), questions.begin(), questions.end());
        Bytes data;
        name(data, instance);
        record(packet, kService, 12, false, ttl, data);
        data.clear();
        write16(data, 0);
        write16(data, 0);
        write16(data, port);
        name(data, host);
        record(packet, instance, 33, !legacy, ttl, data);
        data.clear();
        for (const char *entry : {"txtvers=1", "ch=2", "cn=0,1", "et=0,1", "ek=1",
                                 "sr=44100", "ss=16", "tp=UDP", "vn=65537", "vs=105.1",
                                 "md=0,2", "da=true", "sv=false", "sf=0x4",
                                 "pw=false", "am=CloudTuneNX"}) {
            size_t length = std::strlen(entry);
            data.push_back(static_cast<uint8_t>(length));
            data.insert(data.end(), entry, entry + length);
        }
        record(packet, instance, 16, !legacy, ttl, data);
        const auto *bytes = reinterpret_cast<const uint8_t *>(&address);
        data.assign(bytes, bytes + sizeof(address));
        record(packet, host, 1, !legacy, ttl, data);
        if (enumerate) {
            data.clear();
            name(data, kService);
            record(packet, kEnumeration, 12, false, ttl, data);
        }
        return packet;
    }
    void send(uint32_t ttl, const sockaddr_in *destination = nullptr, uint16_t id = 0,
              bool enumerate = false, const Bytes &questions = {}, uint16_t questionCount = 0,
              bool legacy = false) {
        sockaddr_in multicast{};
        multicast.sin_family = AF_INET;
        multicast.sin_port = htons(5353);
        inet_pton(AF_INET, "224.0.0.251", &multicast.sin_addr);
        const Bytes packet = response(ttl, id, enumerate, questions, questionCount, legacy);
        if (packet.size() <= kPacketLimit)
            ::sendto(socket, packet.data(), packet.size(), 0,
                     reinterpret_cast<const sockaddr *>(destination ? destination : &multicast),
                     sizeof(multicast));
    }
};
MdnsAdvertiser::MdnsAdvertiser() : impl(std::make_unique<Impl>()) {}
MdnsAdvertiser::~MdnsAdvertiser() { stop(); }
bool MdnsAdvertiser::start(const std::string &friendly, uint16_t port,
                           const std::string &deviceId, uint32_t address) {
    stop();
    std::string identifier;
    for (char ch : deviceId) {
        if (ch == ':' || ch == '-')
            continue;
        if (!std::isxdigit(static_cast<unsigned char>(ch)))
            return false;
        identifier.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }
    if (identifier.size() != 12 || !port || !address || address == INADDR_NONE)
        return false;
    std::string label = friendly.empty() ? "CloudTune NX" : friendly;
    for (char &ch : label)
        if (ch == '.' || static_cast<unsigned char>(ch) < 32 || ch == 127)
            ch = '-';
    if (label.size() > 50) {
        size_t end = 50;
        while (end && (static_cast<unsigned char>(label[end]) & 0xC0) == 0x80)
            --end;
        label.resize(end);
    }
    impl->instance = identifier + "@" + label + "." + kService;
    impl->host = "cloudtune-" + lower(identifier) + ".local";
    impl->address = address;
    impl->port = port;
    impl->socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (impl->socket < 0)
        return false;
    int receive = UdpDiscoveryBytes;
    if (configureUdpBuffers(impl->socket, receive, UdpDiscoveryBytes)) {
        const int error = errno;
        ::close(impl->socket);
        impl->socket = -1;
        errno = error;
        return false;
    }
    const int yes = 1;
    const int unicastTtl = 255;
    const unsigned char ttl = 255;
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(5353);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    ip_mreq membership{};
    inet_pton(AF_INET, "224.0.0.251", &membership.imr_multiaddr);
    membership.imr_interface.s_addr = address;
    if (::setsockopt(impl->socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0 ||
        ::bind(impl->socket, reinterpret_cast<sockaddr *>(&local), sizeof(local)) < 0 ||
        ::setsockopt(impl->socket, IPPROTO_IP, IP_TTL, &unicastTtl, sizeof(unicastTtl)) < 0 ||
        ::setsockopt(impl->socket, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0 ||
        ::setsockopt(impl->socket, IPPROTO_IP, IP_MULTICAST_IF, &membership.imr_interface,
                     sizeof(membership.imr_interface)) < 0 ||
        ::setsockopt(impl->socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership,
                     sizeof(membership)) < 0 ||
        ::fcntl(impl->socket, F_SETFL, O_NONBLOCK) < 0) {
        const int error = errno;
        ::close(impl->socket);
        impl->socket = -1;
        errno = error;
        return false;
    }
    impl->announcements = 0;
    impl->nextAnnouncement = Clock::now();
    impl->lastReply = {};
    tick();
    return true;
}
void MdnsAdvertiser::tick() {
    if (impl->socket < 0)
        return;
    const auto now = Clock::now();
    if (now >= impl->nextAnnouncement) {
        impl->send(120);
        impl->nextAnnouncement = now + std::chrono::seconds(++impl->announcements < 3 ? 1 : 60);
    }
    std::array<uint8_t, kPacketLimit + 1> packet{};
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        sockaddr_in sender{};
        socklen_t size = sizeof(sender);
        const ssize_t received = ::recvfrom(impl->socket, packet.data(), packet.size(), 0,
                                            reinterpret_cast<sockaddr *>(&sender), &size);
        if (received < 0)
            break;
        if (received < 12 || received > static_cast<ssize_t>(kPacketLimit) ||
            (packet[2] & 0xF8) || (packet[3] & 0x0F))
            continue;
        const uint16_t questions = read16(packet.data() + 4);
        if (!questions || questions > kQuestionLimit)
            continue;
        const bool legacy = ntohs(sender.sin_port) != 5353;
        bool relevant = false, unicast = legacy, enumerate = false,
             valid = true;
        Bytes questionData;
        size_t position = 12;
        for (unsigned i = 0; i < questions; ++i) {
            std::string queried;
            if (!readName(packet.data(), static_cast<size_t>(received), position, queried) ||
                position + 4 > static_cast<size_t>(received)) {
                valid = false;
                break;
            }
            uint16_t type = read16(packet.data() + position);
            uint16_t cls = read16(packet.data() + position + 2);
            position += 4;
            if (legacy) {
                // RFC 6762 §6.7: legacy unicast repeats the question. Expand
                // names before copying so input compression offsets cannot
                // point into unrelated response records.
                name(questionData, queried);
                write16(questionData, type);
                write16(questionData, cls);
                if (questionData.size() > kPacketLimit) {
                    valid = false;
                    break;
                }
            }
            queried = lower(std::move(queried));
            if ((cls & 0x7FFF) != 1 && (cls & 0x7FFF) != 255)
                continue;
            bool match = (queried == kService && (type == 12 || type == 255)) ||
                         (queried == lower(impl->instance) &&
                          (type == 16 || type == 33 || type == 255)) ||
                         (queried == impl->host && (type == 1 || type == 255));
            if (queried == kEnumeration && (type == 12 || type == 255)) {
                match = true;
                enumerate = true;
            }
            if (match) {
                relevant = true;
                unicast |= (cls & 0x8000) != 0;
            }
        }
        if (valid && relevant && now - impl->lastReply >= std::chrono::milliseconds(100)) {
            // Modern QU responses keep mDNS semantics; only legacy resolvers
            // receive echoed IDs/questions, short TTLs and no cache-flush bit.
            const uint16_t id = legacy ? read16(packet.data()) : 0;
            impl->send(legacy ? 10 : 120, unicast ? &sender : nullptr, id, enumerate,
                       questionData, legacy ? questions : 0, legacy);
            impl->lastReply = now;
        }
    }
}
void MdnsAdvertiser::stop() noexcept {
    if (impl->socket >= 0) {
        // A goodbye is optional. It must never prevent closing the socket or
        // escape a destructor/worker cleanup when memory is exhausted.
        try {
            impl->send(0);
        } catch (...) {
        }
        ::close(impl->socket);
        impl->socket = -1;
    }
}
int MdnsAdvertiser::fd() const { return impl->socket; }
} // namespace cloudtune::airplay
