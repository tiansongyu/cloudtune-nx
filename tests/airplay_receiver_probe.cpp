/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../airplay/receiver.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <new>
#include <string>

static std::atomic<bool> boundedAllocations{false};
static std::atomic<bool> failNextAllocation{false};
bool airplayUdpTestCommand(const char *command);
extern "C" void *__real__Znwm(size_t);
extern "C" void *__wrap__Znwm(size_t bytes) {
    if (failNextAllocation.exchange(false))
        throw std::bad_alloc();
    if (boundedAllocations && bytes > 64 * 1024)
        throw std::bad_alloc();
    return __real__Znwm(bytes);
}

namespace {
struct Capture {
    std::mutex mutex;
    uint64_t frames = 0, blocks = 0;
    uint32_t crc = 0xFFFFFFFFu, peak = 0, begins = 0, ends = 0, flushes = 0, rate = 0;
    float volume = 0;
    bool accept = true;
    void byte(uint8_t value) {
        crc ^= value;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
};
std::string quote(const char *text) {
    std::string result = "\"";
    for (const unsigned char *cursor = reinterpret_cast<const unsigned char *>(text); *cursor; ++cursor) {
        if (*cursor == '"' || *cursor == '\\')
            result += '\\';
        if (*cursor >= 32)
            result += static_cast<char>(*cursor);
    }
    return result + '"';
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 2 && (argc != 3 || std::string(argv[2]) != "--retry"))
        return 2;
    char *end = nullptr;
    unsigned long port = std::strtoul(argv[1], &end, 10);
    if (!end || *end || port < 1024 || port > 65535)
        return 2;
    Capture capture;
    cloudtune::airplay::Config config;
    config.port = static_cast<uint16_t>(port);
    config.address = "127.0.0.1";
    config.advertise = std::getenv("CLOUDTUNE_TEST_UDP_BUDGET") != nullptr;
    if (const char *path = std::getenv("CLOUDTUNE_TEST_AIRPLAY_LOG"))
        config.diagnostic_path = path;
    cloudtune::airplay::Callbacks callbacks;
    callbacks.begin = [&](uint32_t rate) {
        std::lock_guard<std::mutex> lock(capture.mutex);
        if (!capture.accept)
            return false;
        ++capture.begins;
        capture.rate = rate;
        return true;
    };
    callbacks.audio = [&](const int16_t *audio, size_t frames) {
        std::lock_guard<std::mutex> lock(capture.mutex);
        capture.frames += frames;
        ++capture.blocks;
        for (size_t i = 0; i < frames * 2; ++i) {
            uint16_t sample = static_cast<uint16_t>(audio[i]);
            capture.byte(static_cast<uint8_t>(sample));
            capture.byte(static_cast<uint8_t>(sample >> 8));
            capture.peak = std::max(capture.peak, static_cast<uint32_t>(std::abs(int(audio[i]))));
        }
        return true;
    };
    callbacks.end = [&] {
        std::lock_guard<std::mutex> lock(capture.mutex);
        ++capture.ends;
    };
    callbacks.flush = [&] {
        std::lock_guard<std::mutex> lock(capture.mutex);
        ++capture.flushes;
    };
    callbacks.volume = [&](float value) {
        std::lock_guard<std::mutex> lock(capture.mutex);
        capture.volume = value;
    };
    cloudtune::airplay::Receiver receiver(config, callbacks);
    const bool ready = receiver.start();
    std::cout << "{\"ready\":" << (ready ? "true" : "false")
              << ",\"port\":" << port << "}\n" << std::flush;
    if (!ready && argc != 3)
        return 1;
    std::string command;
    while (std::getline(std::cin, command)) {
        if (airplayUdpTestCommand(command.c_str())) {
            std::cout << "{\"armed\":true}\n" << std::flush;
            continue;
        }
        if (command == "bounded-memory")
            boundedAllocations = true;
        if (command == "fail-next-allocation") {
            // Arm after formatting this response, so the receiver thread is
            // the only allocating producer until the next wire request.
            std::cout << "{\"armed\":true}\n" << std::flush;
            failNextAllocation = true;
            continue;
        }
        if (command == "stop")
            break;
        if (command == "disconnect") {
            bool result = receiver.disconnect();
            std::cout << "{\"disconnected\":" << (result ? "true" : "false") << "}\n" << std::flush;
            continue;
        }
        if (command == "restart") {
            receiver.stop();
            const bool restarted = receiver.start();
            std::cout << "{\"restarted\":" << (restarted ? "true" : "false") << "}\n" << std::flush;
            continue;
        }
        auto status = receiver.status();
        std::lock_guard<std::mutex> lock(capture.mutex);
        if (command == "reset") {
            capture.frames = capture.blocks = capture.peak = 0;
            capture.crc = 0xFFFFFFFFu;
        } else if (command == "deny")
            capture.accept = false;
        else if (command == "allow")
            capture.accept = true;
        std::cout << "{\"frames\":" << capture.frames << ",\"crc\":" << (capture.crc ^ 0xFFFFFFFFu)
                  << ",\"blocks\":" << capture.blocks << ",\"peak\":" << capture.peak
                  << ",\"begins\":" << capture.begins << ",\"ends\":" << capture.ends
                  << ",\"flushes\":" << capture.flushes << ",\"rate\":" << capture.rate
                  << ",\"volume\":" << capture.volume << ",\"state\":" << status.state
                  << ",\"enabled\":" << status.enabled
                  << ",\"received\":" << status.received_packets << ",\"lost\":" << status.lost_packets
                  << ",\"buffered\":" << status.buffered_frames << ",\"title\":" << quote(status.title)
                  << ",\"artist\":" << quote(status.artist) << "}\n" << std::flush;
    }
    receiver.stop();
    std::cout << "{\"stopped\":true}\n" << std::flush;
}
