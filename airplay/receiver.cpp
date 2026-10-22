/* SPDX-License-Identifier: GPL-3.0-or-later */
// Classic RAOP wire formats/latency conventions are based on shairport-sync
// cf6998daa1876c5a8f97128a960c8afc0e731841 (MIT; vendor/SHAIRPORT-LICENSE).
// This receiver owns its sockets/decoder on one bounded, nonblocking thread.
#include "receiver.hpp"
#include "codec.hpp"
#include "crypto.hpp"
#include "diagnostics.hpp"
#include "mdns.hpp"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __SWITCH__
extern "C" {
#include <switch/kernel/svc.h>
#include <switch/services/nifm.h>
}
#else
#include <ifaddrs.h>
#include <net/if.h>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace cloudtune::airplay {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr size_t MaxHeader = 16 * 1024;
constexpr size_t MaxBody = 512 * 1024;
constexpr size_t MaxMetadata = 16 * 1024;
constexpr size_t MaxPacket = 65536;
constexpr size_t Slots = 512;
constexpr size_t MaxPcmBytes = 1024 * 1024;
constexpr uint32_t DefaultLatency = 88200;
#if defined(MSG_NOSIGNAL) && !defined(__SWITCH__)
constexpr int SendFlags = MSG_NOSIGNAL;
#else
// Horizon BSD does not need the host's SIGPIPE suppression flag.
constexpr int SendFlags = 0;
#endif
int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}
uint16_t read16(const uint8_t* p) { return uint16_t(p[0]) << 8 | p[1]; }
uint32_t read32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void write16(uint8_t* p, uint16_t value) { p[0] = value >> 8; p[1] = value; }
void write32(uint8_t* p, uint32_t value) {
    p[0] = value >> 24; p[1] = value >> 16; p[2] = value >> 8; p[3] = value;
}
int64_t ntpNs(const uint8_t* p) {
    return int64_t(read32(p)) * 1000000000LL + (uint64_t(read32(p + 4)) * 1000000000ULL >> 32);
}
void writeNtp(uint8_t* p, int64_t ns) {
    write32(p, uint32_t(ns / 1000000000LL));
    write32(p + 4, uint32_t((uint64_t(ns % 1000000000LL) << 32) / 1000000000ULL));
}
template <size_t N> void text(char (&destination)[N], std::string_view value) {
    std::memset(destination, 0, N);
    // Sender metadata is untrusted. Preserve valid UTF-8 without splitting a
    // code point, and replace invalid sequences before UI/status consumers.
    size_t input = 0, output = 0;
    while (input < value.size() && output < N - 1) {
        const uint8_t first = uint8_t(value[input]);
        if (first < 0x80) {
            destination[output++] = first < 32 || first == 127 ? ' ' : char(first);
            ++input;
            continue;
        }
        const size_t width = first >= 0xc2 && first <= 0xdf ? 2 :
                             first >= 0xe0 && first <= 0xef ? 3 :
                             first >= 0xf0 && first <= 0xf4 ? 4 : 0;
        bool valid = width && width <= value.size() - input;
        for (size_t offset = 1; valid && offset < width; ++offset)
            valid = (uint8_t(value[input + offset]) & 0xc0) == 0x80;
        if (valid) {
            const uint8_t second = uint8_t(value[input + 1]);
            valid = !(first == 0xe0 && second < 0xa0) &&
                    !(first == 0xed && second >= 0xa0) &&
                    !(first == 0xf0 && second < 0x90) &&
                    !(first == 0xf4 && second >= 0x90);
        }
        if (!valid) {
            destination[output++] = '?';
            ++input;
        } else {
            if (width > N - 1 - output) break;
            std::memcpy(destination + output, value.data() + input, width);
            output += width;
            input += width;
        }
    }
}
std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
std::string lower(std::string value) {
    for (char& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return value;
}
bool integer(const std::string& input, uint32_t& value) {
    if (input.empty() || input.size() > 10) return false;
    uint64_t result = 0;
    for (char c : input) {
        if (c < '0' || c > '9') return false;
        result = result * 10 + unsigned(c - '0');
        if (result > UINT32_MAX) return false;
    }
    value = uint32_t(result);
    return true;
}
bool parameter(const std::string& source, const std::string& name, uint32_t& value) {
    size_t offset = 0;
    while (offset < source.size()) {
        const auto end = source.find(';', offset);
        const auto piece = trim(source.substr(offset, end == std::string::npos ? end : end - offset));
        const auto equal = piece.find('=');
        if (equal != std::string::npos && lower(trim(piece.substr(0, equal))) == name)
            return integer(trim(piece.substr(equal + 1)), value);
        if (end == std::string::npos) break;
        offset = end + 1;
    }
    return false;
}
void closeFd(int& descriptor) { if (descriptor >= 0) ::close(descriptor); descriptor = -1; }
bool nonblocking(int descriptor) {
    const int flags = fcntl(descriptor, F_GETFL, 0);
    return flags >= 0 && fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}
uint32_t platformAddress() {
#ifdef __SWITCH__
    // libnx gethostid() returns a loopback fallback on NIFM failure, which
    // would falsely advertise readiness before Wi-Fi has acquired an address.
    uint32_t address = 0;
    if (R_FAILED(nifmInitialize(NifmServiceType_User))) return 0;
    const Result result = nifmGetCurrentIpAddress(&address);
    nifmExit();
    return R_SUCCEEDED(result) ? address : 0;
#else
    ifaddrs* interfaces = nullptr;
    uint32_t address = 0;
    if (getifaddrs(&interfaces) == 0) {
        for (auto* row = interfaces; row; row = row->ifa_next)
            if (row->ifa_addr && row->ifa_addr->sa_family == AF_INET &&
                (row->ifa_flags & IFF_UP) && !(row->ifa_flags & IFF_LOOPBACK)) {
                address = reinterpret_cast<sockaddr_in*>(row->ifa_addr)->sin_addr.s_addr;
                break;
            }
        freeifaddrs(interfaces);
    }
    return address;
#endif
}
std::string addressText(uint32_t address) {
    char output[INET_ADDRSTRLEN]{};
    in_addr value{address};
    return inet_ntop(AF_INET, &value, output, sizeof(output)) ? output : "";
}
int udpSocket(uint16_t& port) {
    int descriptor = socket(AF_INET, SOCK_DGRAM, 0);
    if (descriptor < 0) return -1;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    int buffer = 64 * 1024;
    setsockopt(descriptor, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));
    if (!nonblocking(descriptor) ||
        bind(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(descriptor); return -1;
    }
    socklen_t length = sizeof(address);
    if (getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        ::close(descriptor); return -1;
    }
    port = ntohs(address.sin_port);
    return descriptor;
}
struct Request {
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers;
    std::string body;
    std::string header(const char* name) const {
        const auto found = headers.find(name);
        return found == headers.end() ? "" : found->second;
    }
};
// 0=incomplete, 1=complete, negative=RTSP error. Binary bodies are length-based.
int parseHeader(std::string &input, Request &request, size_t &remaining) {
    const auto boundary = input.find("\r\n\r\n");
    if (boundary == std::string::npos) return input.size() > MaxHeader ? -413 : 0;
    if (boundary > MaxHeader || input.find('\0') < boundary) return -400;
    std::istringstream lines(input.substr(0, boundary));
    std::string line, version, extra;
    if (!std::getline(lines, line)) return -400;
    std::istringstream first(trim(line));
    if (!(first >> request.method >> request.path >> version) || first >> extra ||
        version != "RTSP/1.0" || request.method.size() > 32 || request.path.size() > 1024) return -400;
    while (std::getline(lines, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos || colon == 0) return -400;
        const auto name = lower(trim(line.substr(0, colon)));
        if (name.empty() || request.headers.size() >= 64 || request.headers.count(name)) return -400;
        request.headers.emplace(name, trim(line.substr(colon + 1)));
    }
    uint32_t sequence = 0, length = 0;
    if (!integer(request.header("cseq"), sequence)) return -400;
    const auto contentLength = request.header("content-length");
    if ((request.headers.count("content-length") && !integer(contentLength, length)) ||
        length > MaxBody) return -413;
    if (request.headers.count("transfer-encoding")) return -400;
    remaining = length;
    input.erase(0, boundary + 4);
    return 1;
}
const char* responseReason(unsigned code) {
    switch (code) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 413: return "Request Entity Too Large";
    case 451: return "Parameter Not Understood";
    case 453: return "Not Enough Bandwidth";
    case 454: return "Session Not Found";
    case 455: return "Method Not Valid in This State";
    case 456: return "Header Field Not Valid for Resource";
    case 461: return "Unsupported Transport";
    case 501: return "Not Implemented";
    default: return "Internal Server Error";
    }
}
}

struct Receiver::Impl {
    Config config;
    Callbacks callbacks;
    mutable std::mutex mutex;
    std::condition_variable ready;
    NcmAirplayStatus snapshot{};
    std::atomic<bool> running{false};
    std::atomic<bool> connected{false};
    std::atomic<uint64_t> disconnectRequest{0}, disconnectAck{0};
    std::thread worker;
    bool initialized = false, initializationSucceeded = false;
    int listener = -1, client = -1, audio = -1, control = -1, timing = -1;
    uint16_t audioPort = 0, controlPort = 0, timingPort = 0;
    uint32_t interfaceAddress = 0, connectionAddress = 0;
    sockaddr_in peer{}, remoteControl{}, remoteTiming{};
    std::string input, output, session, deviceId;
    Request pendingRequest;
    size_t remainingBody = 0;
    bool pendingRequestActive = false, discardBody = false;
    bool closing = false, announced = false, setup = false, recording = false, encrypted = false;
    bool paused = false;
    bool pcm = false;
    uint32_t rate = 44100, frameLength = 352, latency = DefaultLatency;
    uint32_t minimumLatency = 0, maximumLatency = 0, airplayVersion = 0;
    float volumeDb = -18.0f;
    std::array<uint8_t, 16> aesKey{}, aesIv{};
    std::array<uint8_t, 8> timingOrigin{};
    AlacDecoder decoder;
    MdnsAdvertiser mdns;
    std::array<int16_t, 4096 * 2> decoded{};
    std::vector<uint8_t> decrypted;
    std::vector<uint8_t> datagram = std::vector<uint8_t>(MaxPacket);
    struct Packet {
        uint16_t sequence = 0;
        uint32_t timestamp = 0;
        std::vector<int16_t> samples;
    };
    std::array<Packet, Slots> packets;
    struct Gap { uint16_t sequence = 0; int64_t since = 0, requested = 0; };
    std::array<Gap, Slots> gaps{};
    size_t bufferedBytes = 0;
    bool haveSequence = false, haveFallback = false, haveTiming = false, haveSync = false;
    bool flushBoundary = false, flushSequencePresent = false;
    uint16_t nextSequence = 0, highestSequence = 0, flushSequence = 0;
    uint32_t expectedTimestamp = 0, firstTimestamp = 0, syncTimestamp = 0, flushTimestamp = 0;
    int64_t fallbackTime = 0, syncRemoteTime = 0, remoteOffset = 0;
    int64_t lastActivity = 0, lastAudio = 0, partialSince = 0, pendingSince = 0;
    int64_t nextTiming = 0, timingDeparture = 0, bestRoundTrip = INT64_MAX;
    int64_t nextAddressCheck = 0;
    unsigned timingCount = 0;
    unsigned diagnosticEvents = 0, rejectedAudio = 0;
    bool loggedDatagram = false, loggedDecoded = false, loggedOutput = false;

    void trace(const char *event, uint64_t a = 0, uint64_t b = 0) noexcept {
        if (diagnosticEvents++ < 96)
            diagnostic(config.diagnostic_path, event, a, b);
    }

    Impl(Config value, Callbacks handlers) : config(std::move(value)), callbacks(std::move(handlers)) {
        snapshot.version = NCM_AIRPLAY_VERSION;
        text(snapshot.name, config.name);
        snapshot.port = config.port;
        for (uint8_t byte : config.device_id) {
            char pair[3]; std::snprintf(pair, sizeof(pair), "%02X", byte); deviceId += pair;
        }
    }
    void state(NcmAirplayState value, std::string_view message, uint32_t error = 0) {
        std::lock_guard<std::mutex> lock(mutex);
        snapshot.state = value; snapshot.error_code = error; text(snapshot.message, message);
    }
    void count(bool lost, uint32_t amount = 1) {
        std::lock_guard<std::mutex> lock(mutex);
        if (lost) snapshot.lost_packets += amount;
        else snapshot.received_packets += amount;
        snapshot.buffered_frames = bufferedBytes / 4;
    }
    void clearQueue() {
        for (auto& packet : packets) { std::vector<int16_t>().swap(packet.samples); }
        bufferedBytes = 0; haveSequence = haveFallback = false;
        gaps.fill({});
        std::lock_guard<std::mutex> lock(mutex);
        snapshot.buffered_frames = 0;
    }
    void resetStream() {
        if (recording) {
            recording = false;
            try { if (callbacks.end) callbacks.end(); } catch (...) {}
        }
        clearQueue();
        decoder.close();
        closeFd(audio); closeFd(control); closeFd(timing);
        announced = setup = encrypted = pcm = paused = false;
        haveTiming = haveSync = flushBoundary = flushSequencePresent = false;
        aesKey.fill(0); aesIv.fill(0); decrypted.clear();
        session.clear();
        rate = 44100; frameLength = 352; latency = DefaultLatency;
        minimumLatency = maximumLatency = airplayVersion = 0;
        nextTiming = timingDeparture = 0; bestRoundTrip = INT64_MAX; timingCount = 0;
        volumeDb = -18.0f;
        std::lock_guard<std::mutex> lock(mutex);
        snapshot.title[0] = snapshot.artist[0] = '\0';
    }
    void closeClient() {
        if (client >= 0) {
            trace("session-close", snapshot.received_packets, snapshot.lost_packets);
        }
        resetStream();
        closeFd(client);
        connected = false;
        input.clear(); output.clear(); closing = false;
        pendingRequest = {};
        remainingBody = 0;
        pendingRequestActive = discardBody = false;
        partialSince = pendingSince = lastAudio = 0;
        if (running && listener >= 0) state(NcmAirplay_Ready, "Ready for AirPlay");
    }
    bool openListener() {
        if (callbacks.can_start_work && !callbacks.can_start_work())
            return false;
        if (config.name.empty() || config.name.size() > 48 || config.port == 0) return false;
        if (config.address.empty()) interfaceAddress = platformAddress();
        else {
            in_addr address{};
            if (inet_pton(AF_INET, config.address.c_str(), &address) != 1) return false;
            interfaceAddress = address.s_addr;
        }
        if (!interfaceAddress) return false;
        listener = socket(AF_INET, SOCK_STREAM, 0);
        if (listener < 0) return false;
        int reuse = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET; address.sin_port = htons(config.port);
        address.sin_addr.s_addr = config.address.empty() ? htonl(INADDR_ANY) : interfaceAddress;
        if (!nonblocking(listener) ||
            bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(listener, 4) != 0) return false;
        if (config.advertise && !mdns.start(config.name, config.port, deviceId, interfaceAddress)) return false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            text(snapshot.address, addressText(interfaceAddress));
        }
        return true;
    }
    void acceptClient() {
        sockaddr_in address{};
        socklen_t length = sizeof(address);
        const int incoming = accept(listener, reinterpret_cast<sockaddr*>(&address), &length);
        if (incoming < 0) return;
        if (!nonblocking(incoming)) { ::close(incoming); return; }
        if (client >= 0 && !recording && peer.sin_addr.s_addr == address.sin_addr.s_addr) {
            // A phone may probe/select the route on one TCP connection and
            // open its actual playback session on another. An idle probe is
            // not an audio owner. Keep active recordings exclusive.
            trace("idle-session-replaced");
            closeClient();
        }
        if (client >= 0) {
            // A session has one RTSP owner; unsolicited takeover cannot replace it.
            trace("active-session-busy");
            const char busy[] = "RTSP/1.0 453 Not Enough Bandwidth\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
            send(incoming, busy, sizeof(busy) - 1, SendFlags);
            ::close(incoming);
            return;
        }
        client = incoming; peer = address;
        connected = true;
        diagnosticEvents = rejectedAudio = 0;
        loggedDatagram = loggedDecoded = loggedOutput = false;
        trace("session-accept");
        sockaddr_in local{};
        length = sizeof(local);
        connectionAddress = getsockname(client, reinterpret_cast<sockaddr*>(&local), &length) == 0
            ? local.sin_addr.s_addr : interfaceAddress;
        lastActivity = nowNs();
        state(NcmAirplay_Connected, "Sender connected");
    }
    void respond(const Request& request, unsigned code, const std::string& headers = "",
                 const std::string& body = "") {
        static constexpr const char *methods[] = {"OPTIONS",       "ANNOUNCE",      "SETUP",
                                                  "RECORD",        "FLUSH",         "PAUSE",
                                                  "SET_PARAMETER", "GET_PARAMETER", "TEARDOWN"};
        unsigned method = 0;
        for (unsigned i = 0; i < sizeof(methods) / sizeof(methods[0]); ++i)
            if (request.method == methods[i])
                method = i + 1;
        trace("rtsp-reply", method, code);
        std::ostringstream response;
        response << "RTSP/1.0 " << code << ' ' << responseReason(code) << "\r\n";
        const auto cseq = request.header("cseq");
        if (!cseq.empty()) response << "CSeq: " << cseq << "\r\n";
        response << "Server: AirTunes/105.1\r\n";
        const auto challenge = request.header("apple-challenge");
        if (!challenge.empty()) {
            const auto answer = challengeResponse(challenge, connectionAddress, config.device_id);
            trace("apple-challenge", !answer.empty());
            if (!answer.empty()) response << "Apple-Response: " << answer << "\r\n";
        }
        response << headers << "Content-Length: " << body.size() << "\r\n\r\n" << body;
        if (output.empty()) pendingSince = nowNs();
        output += response.str();
        if (output.size() > 64 * 1024) closing = true;
    }
    bool announce(const Request& request) {
        if (recording) return false;
        resetStream();
        std::array<uint32_t, 12> format{{96,352,0,16,40,10,14,2,255,0,0,44100}};
        bool haveFormat = false, havePcm = false;
        std::string encodedKey, encodedIv;
        std::istringstream sdp(request.body);
        std::string line;
        while (std::getline(sdp, line)) {
            line = trim(line);
            if (line == "a=rtpmap:96 L16/44100/2") havePcm = true;
            if (line.rfind("a=fmtp:", 0) == 0) {
                std::istringstream values(line.substr(7));
                for (auto& field : format) {
                    std::string word;
                    if (!(values >> word) || !integer(word, field)) return false;
                }
                std::string trailing;
                if (values >> trailing) return false;
                haveFormat = true;
            }
            if (line.rfind("a=rsaaeskey:", 0) == 0) encodedKey = line.substr(12);
            if (line.rfind("a=aesiv:", 0) == 0) encodedIv = line.substr(8);
            if (line.rfind("a=min-latency:", 0) == 0 &&
                !integer(line.substr(14), minimumLatency)) return false;
            if (line.rfind("a=max-latency:", 0) == 0 &&
                !integer(line.substr(14), maximumLatency)) return false;
        }
        if (minimumLatency > 192000 || maximumLatency > 192000 ||
            (maximumLatency && minimumLatency > maximumLatency)) return false;
        if (encodedKey.empty() != encodedIv.empty()) return false;
        if (!encodedKey.empty()) {
            std::vector<uint8_t> iv;
            if (!decodeBase64(encodedIv, iv) || iv.size() != 16 ||
                !unwrapSessionKey(encodedKey, aesKey)) return false;
            std::copy(iv.begin(), iv.end(), aesIv.begin()); encrypted = true;
        }
        if (havePcm) { pcm = true; rate = 44100; frameLength = 352; }
        else if (haveFormat && format[0] == 96 && format[1] > 0 && format[1] <= 4096 &&
                 format[3] == 16 && format[7] == 2 &&
                 (format[11] == 44100 || format[11] == 48000) && decoder.configure(format)) {
            rate = format[11]; frameLength = format[1];
        } else return false;
        if (minimumLatency > rate * 4 || maximumLatency > rate * 4) return false;
        latency = rate * 2;
        const auto userAgent = request.header("user-agent");
        const auto airplay = userAgent.find("AirPlay/");
        if (airplay != std::string::npos) {
            auto end = userAgent.find_first_not_of("0123456789", airplay + 8);
            integer(userAgent.substr(airplay + 8, end == std::string::npos ? end : end - airplay - 8), airplayVersion);
        }
        announced = true;
        trace("format", rate, frameLength);
        trace("encoding", pcm, encrypted);
        state(NcmAirplay_Connected, "Audio session announced");
        return true;
    }
    bool setupTransport(const Request& request) {
        if (!announced || recording) return false;
        const auto transport = request.header("transport");
        uint32_t remoteControlPort = 0, remoteTimingPort = 0;
        if (transport.find("RTP/AVP/UDP") == std::string::npos ||
            !parameter(transport, "control_port", remoteControlPort) ||
            !parameter(transport, "timing_port", remoteTimingPort) ||
            remoteControlPort == 0 || remoteControlPort > 65535 ||
            remoteTimingPort == 0 || remoteTimingPort > 65535) return false;
        closeFd(audio); closeFd(control); closeFd(timing);
        audio = udpSocket(audioPort); control = udpSocket(controlPort); timing = udpSocket(timingPort);
        if (audio < 0 || control < 0 || timing < 0) {
            trace("udp-open-error", errno);
            closeFd(audio); closeFd(control); closeFd(timing); return false;
        }
        remoteControl = peer; remoteControl.sin_port = htons(uint16_t(remoteControlPort));
        remoteTiming = peer; remoteTiming.sin_port = htons(uint16_t(remoteTimingPort));
        session = "1"; setup = true;
        nextTiming = nowNs(); timingCount = 0;
        trace("udp-ports", audioPort, controlPort);
        trace("timing-port", timingPort);
        return true;
    }
    void flush(const Request& request) {
        clearQueue(); decoder.reset();
        flushBoundary = parameter(request.header("rtp-info"), "rtptime", flushTimestamp);
        uint32_t sequence = 0;
        flushSequencePresent = parameter(request.header("rtp-info"), "seq", sequence) && sequence <= 65535;
        flushSequence = uint16_t(sequence);
        if (recording && callbacks.flush) callbacks.flush();
        state(NcmAirplay_Connected, "Stream flushed");
    }
    void metadata(const uint8_t* data, size_t length, unsigned depth = 0) {
        if (depth > 6) return;
        size_t offset = 0;
        while (length - offset >= 8) {
            const auto* record = data + offset;
            const uint32_t size = read32(record + 4);
            if (size > length - offset - 8) return;
            const std::string tag(reinterpret_cast<const char*>(record), 4);
            if (tag == "mlit" || tag == "mdcl") metadata(record + 8, size, depth + 1);
            else if (tag == "minm" || tag == "asar") {
                std::lock_guard<std::mutex> lock(mutex);
                const std::string value(reinterpret_cast<const char*>(record + 8), std::min<size_t>(size, 4096));
                if (tag == "minm") text(snapshot.title, value);
                else text(snapshot.artist, value);
            }
            offset += 8 + size;
        }
    }
    unsigned setParameter(const Request& request) {
        const auto type = lower(request.header("content-type"));
        if (type.rfind("text/parameters", 0) == 0) {
            std::istringstream lines(request.body);
            std::string line;
            while (std::getline(lines, line)) {
                if (line.rfind("volume:", 0) != 0) continue;
                const auto raw = trim(line.substr(7));
                if (raw.size() > 32) return 451;
                char* end = nullptr;
                const float value = std::strtof(raw.c_str(), &end);
                if (end == raw.c_str() || *end || !std::isfinite(value) || value < -144 || value > 0)
                    return 451;
                volumeDb = value == -144 ? value : std::max(-30.0f, value);
                if (recording && callbacks.volume) callbacks.volume(volumeDb);
            }
        } else if (type.rfind("application/x-dmap-tagged", 0) == 0)
            metadata(reinterpret_cast<const uint8_t*>(request.body.data()), request.body.size());
        // Cover images are consumed with the same strict body limit but are not
        // written to disk: this receiver advertises text/progress metadata only.
        return 200;
    }
    void handle(const Request& request) {
        lastActivity = nowNs();
        const auto suppliedSession = request.header("session");
        if (!suppliedSession.empty() && setup && suppliedSession != session) {
            respond(request, 454); return;
        }
        if (request.method == "OPTIONS") {
            respond(request, 200, "Public: ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, OPTIONS, GET_PARAMETER, SET_PARAMETER\r\n");
        } else if (request.method == "ANNOUNCE") {
            respond(request, announce(request) ? 200 : 456);
        } else if (request.method == "SETUP") {
            if (!announced) { respond(request, 455); return; }
            if (!setupTransport(request)) { respond(request, 461); return; }
            std::ostringstream transport;
            transport << "Transport: RTP/AVP/UDP;unicast;interleaved=0-1;mode=record;control_port="
                      << controlPort << ";timing_port=" << timingPort << ";server_port=" << audioPort
                      << "\r\nSession: " << session << "\r\n";
            respond(request, 200, transport.str());
        } else if (request.method == "RECORD") {
            if (!setup) { respond(request, 455); return; }
            if (!recording) {
                if (!callbacks.begin || !callbacks.audio || !callbacks.begin(rate)) {
                    trace("record-rejected");
                    respond(request, 453); return;
                }
                recording = true;
                trace("record-accepted");
                if (callbacks.volume) callbacks.volume(volumeDb);
            }
            paused = false;
            if (!request.header("rtp-info").empty()) flush(request);
            respond(request, 200, "Audio-Latency: 11025\r\nSession: " + session + "\r\n");
        } else if (request.method == "FLUSH" || request.method == "PAUSE") {
            if (!setup) { respond(request, 455); return; }
            flush(request);
            paused = request.method == "PAUSE";
            respond(request, 200);
        } else if (request.method == "SET_PARAMETER") {
            respond(request, setParameter(request));
        } else if (request.method == "GET_PARAMETER") {
            if (request.body.find("volume") != std::string::npos) {
                char body[64]; std::snprintf(body, sizeof(body), "volume: %.6f\r\n", double(volumeDb));
                respond(request, 200, "Content-Type: text/parameters\r\n", body);
            } else respond(request, 200);
        } else if (request.method == "TEARDOWN") {
            resetStream();
            respond(request, 200, "Connection: close\r\n"); closing = true;
        } else respond(request, 501);
    }
    void consumeInput(unsigned &budget) {
        while (client >= 0 && !closing && budget) {
            if (!pendingRequestActive) {
                Request request;
                const int parsed = parseHeader(input, request, remainingBody);
                if (!parsed)
                    return;
                if (parsed < 0) {
                    trace("rtsp-parse-error", unsigned(-parsed),
                          input.find(" HTTP/1.1\r\n") != std::string::npos);
                    respond(request, unsigned(-parsed));
                    closing = true;
                    return;
                }
                const auto type = lower(request.header("content-type"));
                // Artwork and unknown SET_PARAMETER bodies are acknowledged but
                // unused. Drain them incrementally: an iPhone cover must not
                // allocate two full copies in a 2 MiB background heap.
                discardBody = request.method != "ANNOUNCE" && request.method != "GET_PARAMETER" &&
                              !(request.method == "SET_PARAMETER" &&
                                (type.rfind("text/parameters", 0) == 0 ||
                                 type.rfind("application/x-dmap-tagged", 0) == 0));
                if (!discardBody && remainingBody > MaxMetadata) {
                    respond(request, 413);
                    closing = true;
                    return;
                }
                pendingRequest = std::move(request);
                pendingRequestActive = true;
            }
            const size_t bytes = std::min(remainingBody, input.size());
            if (!discardBody)
                pendingRequest.body.append(input.data(), bytes);
            input.erase(0, bytes);
            remainingBody -= bytes;
            if (remainingBody)
                return;
            handle(pendingRequest);
            pendingRequest = {};
            pendingRequestActive = false;
            --budget;
            partialSince = input.empty() ? 0 : nowNs();
            if (disconnectRequest.load() != disconnectAck.load())
                return;
        }
    }
    void readClient() {
        std::array<char, 4096> buffer{};
        unsigned budget = 8;
        consumeInput(budget);
        // Bound control traffic per turn so a large cover cannot starve RTP.
        for (unsigned i = 0; client >= 0 && !closing && budget && i < 8; ++i) {
            const ssize_t size = recv(client, buffer.data(), buffer.size(), 0);
            if (size == 0) {
                trace("tcp-eof");
                closeClient();
                return;
            }
            if (size < 0) {
                if (errno == EINTR) continue;
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    trace("tcp-read-error", errno);
                    closeClient();
                }
                break;
            }
            if (!partialSince)
                partialSince = nowNs();
            input.append(buffer.data(), size);
            if (input.size() > MaxHeader + buffer.size()) {
                respond(Request{}, 413); closing = true; return;
            }
            consumeInput(budget);
            if (disconnectRequest.load() != disconnectAck.load()) break;
        }
    }
    void writeClient() {
        if (output.empty()) { if (closing) closeClient(); return; }
        const auto sent = send(client, output.data(), output.size(), SendFlags);
        if (sent > 0) output.erase(0, size_t(sent));
        else if (sent == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            trace("tcp-write-error", errno);
            closeClient(); return;
        }
        if (output.empty()) {
            pendingSince = 0;
            if (closing) closeClient();
        }
    }
    void receiveAudio(const uint8_t* packet, size_t length) {
        if (!loggedDatagram) {
            loggedDatagram = true;
            trace("first-udp-audio", length, length > 1 ? packet[1] : 0);
        }
        if (!setup || paused || length < 12) return;
        const uint8_t type = packet[1] & 0x7f;
        if (type == 0x56) {
            if (length < 16) return;
            packet += 4; length -= 4;
        } else if (type != 0x60) return;
        if ((packet[0] >> 6) != 2 || (packet[1] & 0x7f) != 0x60) return;
        size_t headerLength = 12 + size_t(packet[0] & 15) * 4;
        if (headerLength > length) return;
        if (packet[0] & 0x10) {
            if (length - headerLength < 4) return;
            const size_t extension = 4 + size_t(read16(packet + headerLength + 2)) * 4;
            if (extension > length - headerLength) return;
            headerLength += extension;
        }
        if (packet[0] & 0x20) {
            const size_t padding = packet[length - 1];
            if (!padding || padding > length - headerLength) return;
            length -= padding;
        }
        if (headerLength >= length) return;
        const uint16_t sequence = read16(packet + 2);
        const uint32_t timestamp = read32(packet + 4);
        if ((flushBoundary && int32_t(timestamp - flushTimestamp) < 0) ||
            (flushSequencePresent && int16_t(sequence - flushSequence) < 0)) return;
        if (haveSequence) {
            const int16_t distance = int16_t(sequence - nextSequence);
            if (distance < 0) return; // Already played, duplicate, or too late.
            if (distance >= int(Slots)) {
                // A sender can jump after a Wi-Fi interruption without FLUSH.
                // Recover a stale queue, while rejecting isolated far-future packets.
                if (lastAudio && nowNs() - lastAudio > 500000000) {
                    clearQueue();
                    if (recording && callbacks.flush) callbacks.flush();
                } else { count(true); return; }
            }
        }
        const uint8_t* payload = packet + headerLength;
        const size_t payloadLength = length - headerLength;
        if (encrypted) {
            if (!decryptAudio(payload, payloadLength, aesKey, aesIv, decrypted)) { count(true); return; }
            payload = decrypted.data();
        }
        size_t frames = 0;
        if (pcm) {
            if (payloadLength % 4 || payloadLength > decoded.size() * sizeof(int16_t)) { count(true); return; }
            frames = payloadLength / 4;
            for (size_t i = 0; i < frames * 2; ++i) decoded[i] = int16_t(read16(payload + i * 2));
        } else frames = decoder.decode(payload, payloadLength, decoded.data(), decoded.size() / 2);
        if (!frames || frames > 4096) {
            if (rejectedAudio++ < 4)
                trace("decode-rejected", payloadLength, frames);
            count(true);
            return;
        }
        if (!loggedDecoded) {
            loggedDecoded = true;
            trace("first-decoded", frames, payloadLength);
        }
        auto& slot = packets[sequence % Slots];
        if (!slot.samples.empty() && slot.sequence == sequence) return;
        const size_t bytes = frames * 4;
        if (bufferedBytes - slot.samples.size() * 2 + bytes > MaxPcmBytes) { count(true); return; }
        bufferedBytes -= slot.samples.size() * 2;
        slot.samples.assign(decoded.begin(), decoded.begin() + frames * 2);
        slot.sequence = sequence; slot.timestamp = timestamp; bufferedBytes += bytes;
        if (!haveSequence) {
            nextSequence = sequence; expectedTimestamp = timestamp; highestSequence = sequence;
            if (flushSequencePresent && flushBoundary &&
                int16_t(sequence - flushSequence) >= 0 && int16_t(sequence - flushSequence) < int(Slots)) {
                nextSequence = flushSequence; expectedTimestamp = flushTimestamp;
            }
            haveSequence = true;
        }
        if (int16_t(sequence - highestSequence) > 0) highestSequence = sequence;
        gaps[sequence % Slots] = {};
        if (!haveFallback) {
            firstTimestamp = timestamp; fallbackTime = nowNs() + int64_t(latency) * 1000000000LL / rate;
            haveFallback = true;
        }
        lastActivity = lastAudio = nowNs(); count(false);
    }
    void receiveControl(const uint8_t* packet, size_t length) {
        if (length < 2 || !setup) return;
        if (packet[1] == 0xd6) { receiveAudio(packet, length); return; }
        if (packet[1] != 0xd4 || length < 20) return;
        const uint32_t timestamp = read32(packet + 16);
        uint32_t requested = timestamp - read32(packet + 4);
        const uint16_t flags = read16(packet + 2);
        if (flags == 7 || (airplayVersion && (airplayVersion <= 353 || airplayVersion >= 371))) {
            if (requested > UINT32_MAX - 11025) return;
            requested += 11025;
        }
        if (minimumLatency) requested = std::max(requested, minimumLatency);
        if (maximumLatency) requested = std::min(requested, maximumLatency);
        if (requested > rate * 4 || requested < frameLength) return;
        if (!haveSync)
            trace("first-sync", requested, flags);
        latency = requested;
        syncTimestamp = timestamp; syncRemoteTime = ntpNs(packet + 8); haveSync = true;
        lastActivity = nowNs();
    }
    void sendTiming(int64_t current) {
        if (!setup || current < nextTiming) return;
        std::array<uint8_t, 32> packet{};
        packet[0] = 0x80; packet[1] = 0xd2; write16(packet.data() + 2, 7);
        writeNtp(packet.data() + 24, current);
        if (sendto(timing, packet.data(), packet.size(), 0,
                   reinterpret_cast<sockaddr*>(&remoteTiming), sizeof(remoteTiming)) >= 0) {
            timingDeparture = current;
            std::copy_n(packet.data() + 24, timingOrigin.size(), timingOrigin.begin());
        }
        nextTiming = current + (++timingCount <= 3 ? 300000000LL : 3000000000LL);
    }
    void receiveTiming(const uint8_t* packet, size_t length) {
        if (length < 32 || packet[1] != 0xd3 || !timingDeparture ||
            std::memcmp(packet + 8, timingOrigin.data(), timingOrigin.size()) != 0) return;
        const int64_t current = nowNs();
        const int64_t receiveTime = ntpNs(packet + 16), transmitTime = ntpNs(packet + 24);
        const int64_t elapsed = current - timingDeparture;
        const int64_t remoteElapsed = transmitTime - receiveTime;
        const int64_t roundTrip = elapsed - remoteElapsed;
        if (elapsed <= 0 || elapsed > 200000000 || remoteElapsed < 0 || roundTrip < 0) return;
        const int64_t offset = (receiveTime - timingDeparture) / 2 + (transmitTime - current) / 2;
        // Prefer low-delay samples; slowly age the minimum so route changes can recover.
        if (!haveTiming || roundTrip <= bestRoundTrip + 2000000) {
            if (!haveTiming)
                trace("first-timing", roundTrip / 1000);
            remoteOffset = haveTiming && std::llabs(offset - remoteOffset) < 100000000
                ? remoteOffset + (offset - remoteOffset) / 8 : offset;
            bestRoundTrip = roundTrip; haveTiming = true;
        } else bestRoundTrip = std::min<int64_t>(200000000, bestRoundTrip + 1000000);
        timingDeparture = 0; lastActivity = current;
    }
    void readDatagrams(int descriptor, unsigned type) {
        for (unsigned i = 0; i < 32; ++i) {
            sockaddr_in sender{};
            socklen_t addressLength = sizeof(sender);
            const auto length = recvfrom(descriptor, datagram.data(), datagram.size(), 0,
                                         reinterpret_cast<sockaddr*>(&sender), &addressLength);
            if (length < 0) { if (errno == EINTR) continue; break; }
            if (sender.sin_family != AF_INET || sender.sin_addr.s_addr != peer.sin_addr.s_addr) continue;
            if (type == 0) receiveAudio(datagram.data(), size_t(length));
            else if (type == 1) receiveControl(datagram.data(), size_t(length));
            else receiveTiming(datagram.data(), size_t(length));
        }
    }
    int64_t presentationTime(uint32_t timestamp) const {
        if (haveTiming && haveSync)
            return syncRemoteTime - remoteOffset +
                int64_t(int32_t(timestamp - syncTimestamp)) * 1000000000LL / rate +
                int64_t(latency) * 1000000000LL / rate;
        return fallbackTime + int64_t(int32_t(timestamp - firstTimestamp)) * 1000000000LL / rate;
    }
    void requestResends(int64_t current) {
        if (!haveSequence || !bufferedBytes) return;
        const int span = int16_t(highestSequence - nextSequence);
        if (span < 0 || span >= int(Slots)) return;
        unsigned requests = 0;
        for (int offset = 0; offset <= span && requests < 2; ++offset) {
            const uint16_t sequence = nextSequence + offset;
            const auto& packet = packets[sequence % Slots];
            if (!packet.samples.empty() && packet.sequence == sequence) continue;
            auto& gap = gaps[sequence % Slots];
            if (!gap.since || gap.sequence != sequence) gap = Gap{sequence, current, 0};
            if (current - gap.since < 100000000 ||
                (gap.requested && current - gap.requested < 150000000)) continue;
            uint16_t countMissing = 1;
            gap.requested = current;
            for (; countMissing < 128 && offset + countMissing <= span; ++countMissing) {
                const uint16_t following = sequence + countMissing;
                const auto& present = packets[following % Slots];
                if (!present.samples.empty() && present.sequence == following) break;
                auto& nextGap = gaps[following % Slots];
                if (!nextGap.since || nextGap.sequence != following) nextGap = Gap{following, current, 0};
                if (current - nextGap.since < 100000000 ||
                    (nextGap.requested && current - nextGap.requested < 150000000)) break;
                nextGap.requested = current;
            }
            std::array<uint8_t, 8> request{{0x80,0xd5,0,1,0,0,0,0}};
            write16(request.data() + 4, sequence); write16(request.data() + 6, countMissing);
            sendto(control, request.data(), request.size(), 0,
                   reinterpret_cast<sockaddr*>(&remoteControl), sizeof(remoteControl));
            offset += countMissing - 1;
            ++requests;
        }
    }
    void pumpAudio(int64_t current) {
        if (!recording || paused || !haveSequence || !haveFallback) return;
        for (unsigned i = 0; i < 8; ++i) {
            auto& packet = packets[nextSequence % Slots];
            const bool present = !packet.samples.empty() && packet.sequence == nextSequence;
            const uint32_t timestamp = present ? packet.timestamp : expectedTimestamp;
            const int64_t due = presentationTime(timestamp);
            if (current + 15000000 < due) break;
            if (!present && !bufferedBytes) break; // Sender may have paused.
            if (due > current + 5000000000LL) break;
            if (present) {
                const size_t frames = packet.samples.size() / 2;
                if (current - due > 500000000 || !callbacks.audio(packet.samples.data(), frames)) count(true);
                else {
                    if (!loggedOutput) {
                        loggedOutput = true;
                        trace("first-pcm-handoff", frames, latency);
                    }
                    state(NcmAirplay_Playing, "AirPlay audio");
                }
                expectedTimestamp = timestamp + uint32_t(frames);
                bufferedBytes -= packet.samples.size() * 2;
                // Release capacity too: a changing frame size cannot retain
                // 4096-frame allocations in every ring slot beyond the budget.
                std::vector<int16_t>().swap(packet.samples);
            } else {
                // Conceal an unrecoverable missing packet at its deadline.
                std::fill(decoded.begin(), decoded.begin() + frameLength * 2, 0);
                if (current - due <= 500000000) callbacks.audio(decoded.data(), frameLength);
                expectedTimestamp += frameLength; count(true);
            }
            gaps[nextSequence % Slots] = {};
            ++nextSequence;
            { std::lock_guard<std::mutex> lock(mutex); snapshot.buffered_frames = bufferedBytes / 4; }
        }
    }
    void run() {
        diagnosticEvents = 0;
        trace("receiver-start");
#ifdef __SWITCH__
        // libnx std::thread defaults to 59. Keep reception below the audio
        // worker (49), while the background NPDM confines both to core 3.
        svcSetThreadPriority(CUR_THREAD_HANDLE, 52);
#endif
        try {
            int64_t retryAt = 0;
            while (running) {
                const uint64_t requested = disconnectRequest.load();
                if (requested != disconnectAck.load()) {
                    closeClient(); disconnectAck = requested; ready.notify_all();
                }
                if (listener < 0) {
                    if (nowNs() < retryAt) { std::this_thread::sleep_for(20ms); continue; }
                    const bool opened = openListener();
                    if (opened) state(NcmAirplay_Ready, "Ready for AirPlay");
                    else {
                        mdns.stop(); closeFd(listener);
                        state(NcmAirplay_Error, "Waiting for network / port", 1);
                        retryAt = nowNs() + 2000000000LL;
                    }
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        initialized = true; initializationSucceeded = opened;
                    }
                    ready.notify_all();
                    if (!opened) continue;
                }
                std::array<pollfd, 6> descriptors{{
                    {listener, POLLIN, 0}, {client, short(POLLIN | (output.empty() ? 0 : POLLOUT)), 0},
                    {audio, POLLIN, 0}, {control, POLLIN, 0}, {timing, POLLIN, 0}, {mdns.fd(), POLLIN, 0}
                }};
                // Horizon's emulated BSD service rejects negative descriptors
                // instead of ignoring them as POSIX poll does. Keep the stable
                // event slots above, but submit only sockets that are open.
                std::array<pollfd, 6> active{};
                std::array<size_t, 6> slots{};
                nfds_t count = 0;
                for (size_t slot = 0; slot < descriptors.size(); ++slot) {
                    if (descriptors[slot].fd < 0) continue;
                    slots[count] = slot;
                    active[count++] = descriptors[slot];
                }
                const int selected = poll(active.data(), count, recording ? 5 : 50);
                if (selected < 0 && errno != EINTR) throw std::runtime_error("AirPlay poll failed");
                for (nfds_t index = 0; index < count; ++index)
                    descriptors[slots[index]].revents = active[index].revents;
                try {
                    if (descriptors[0].revents & POLLIN)
                        acceptClient();
                    if (client >= 0 && descriptors[1].fd == client) {
                        if ((descriptors[1].revents & POLLIN) ||
                            (!input.empty() && input.find("\r\n\r\n") != std::string::npos))
                            readClient();
                        if (client >= 0 &&
                            (descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)))
                            closeClient();
                        if (client >= 0 && !output.empty())
                            writeClient();
                    }
                    if (disconnectRequest.load() != disconnectAck.load())
                        continue;
                    if (audio >= 0 && descriptors[2].fd == audio && descriptors[2].revents & POLLIN)
                        readDatagrams(audio, 0);
                    if (control >= 0 && descriptors[3].fd == control &&
                        descriptors[3].revents & POLLIN)
                        readDatagrams(control, 1);
                    if (timing >= 0 && descriptors[4].fd == timing &&
                        descriptors[4].revents & POLLIN)
                        readDatagrams(timing, 2);
                    if (config.advertise)
                        mdns.tick();
                    const auto current = nowNs();
                    sendTiming(current);
                    requestResends(current);
                    pumpAudio(current);
                    if (client >= 0 && ((lastActivity && current - lastActivity > 60000000000LL) ||
                                        (partialSince && current - partialSince > 5000000000LL) ||
                                        (pendingSince && current - pendingSince > 5000000000LL))) {
                        trace("session-timeout", partialSince != 0, pendingSince != 0);
                        closeClient();
                    }
                    if (recording && lastAudio && current - lastAudio > 2000000000LL &&
                        !bufferedBytes)
                        state(NcmAirplay_Connected, "Waiting for sender audio");
                    if (config.address.empty() && current > nextAddressCheck) {
                        nextAddressCheck = current + 3000000000LL;
                        const auto address = platformAddress();
                        if (address != interfaceAddress) {
                            trace("network-address-changed", address != 0);
                            closeClient();
                            mdns.stop();
                            closeFd(listener);
                            state(NcmAirplay_Starting, "Network changed; reconnecting");
                            retryAt = current;
                            {
                                std::lock_guard<std::mutex> lock(mutex);
                                snapshot.address[0] = '\0';
                            }
                        }
                    }
                } catch (const std::bad_alloc &) {
                    // A single sender must not permanently kill discovery and
                    // listening if its session exceeds the compact heap.
                    trace("session-memory-limit");
                    closeClient();
                    state(NcmAirplay_Ready, "Session memory limit; retry", 12);
                    std::this_thread::sleep_for(20ms);
                }
            }
        } catch (const std::exception& error) {
            trace("receiver-exception");
            state(NcmAirplay_Error, error.what(), 1);
        } catch (...) {
            state(NcmAirplay_Error, "AirPlay receiver failed", 1);
        }
        running = false;
        try { closeClient(); } catch (...) {}
        mdns.stop(); closeFd(listener);
        {
            std::lock_guard<std::mutex> lock(mutex);
            initialized = true;
        }
        disconnectAck = disconnectRequest.load();
        ready.notify_all();
    }
};

Receiver::Receiver(Config config, Callbacks callbacks)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(callbacks))) {}
Receiver::~Receiver() { stop(); }
bool Receiver::start() {
    if (impl_->running.exchange(true)) {
        const auto current = status();
        return current.state == NcmAirplay_Ready || current.state == NcmAirplay_Connected ||
               current.state == NcmAirplay_Playing;
    }
    if (impl_->worker.joinable()) impl_->worker.join();
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->initialized = impl_->initializationSucceeded = false;
        impl_->snapshot.enabled = 1;
        impl_->snapshot.state = NcmAirplay_Starting;
        impl_->snapshot.error_code = 0;
    }
    try { impl_->worker = std::thread([this] { impl_->run(); }); }
    catch (...) { impl_->running = false; impl_->state(NcmAirplay_Error, "Cannot start AirPlay thread", 1); return false; }
    std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->ready.wait_for(lock, 1s, [this] { return impl_->initialized; });
    return impl_->initialized && impl_->initializationSucceeded;
}
void Receiver::stop() {
    impl_->running = false;
    if (impl_->worker.joinable() && impl_->worker.get_id() != std::this_thread::get_id()) impl_->worker.join();
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->snapshot.enabled = 0;
        impl_->snapshot.state = NcmAirplay_Disabled;
        impl_->snapshot.error_code = 0;
        text(impl_->snapshot.message, "AirPlay disabled");
    }
}
bool Receiver::disconnect(uint32_t timeout) {
    const uint64_t requested = impl_->disconnectRequest.fetch_add(1) + 1;
    // Ordinary music transport need not wait for an idle listener's poll.
    // The request is still recorded, so an accept racing this check is closed.
    if (!impl_->running || !impl_->connected)
        return true;
    if (impl_->worker.get_id() == std::this_thread::get_id()) return false;
    std::unique_lock<std::mutex> lock(impl_->mutex);
    return impl_->ready.wait_for(lock, std::chrono::milliseconds(std::min<uint32_t>(timeout, 5000)),
        [this, requested] { return !impl_->running || impl_->disconnectAck.load() >= requested; });
}
NcmAirplayStatus Receiver::status() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->snapshot;
}
} // namespace cloudtune::airplay
