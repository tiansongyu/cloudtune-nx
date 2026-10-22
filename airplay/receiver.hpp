#pragma once
/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "netease/airplay_protocol.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace cloudtune::airplay {
struct Config {
    std::string name = "CloudTune NX";
    uint16_t port = 5000;
    std::array<uint8_t, 6> device_id{{0x02, 0x43, 0x54, 0x4e, 0x58, 0x01}};
    std::string address; // Empty: discover the active IPv4 interface.
    bool advertise = true;
    std::string diagnostic_path; // Optional bounded stage log; no sender payloads.
};
struct Callbacks {
    // Optional platform resource admission before opening/reopening sockets.
    bool (*can_start_work)() = nullptr;
    // Callbacks run only on the receiver thread and must return promptly.
    // begin obtains exclusive ownership; false rejects RECORD with RTSP 453.
    std::function<bool(uint32_t rate)> begin;
    // Stereo S16, interleaved. false means the bounded output queue is full.
    std::function<bool(const int16_t* stereo, size_t frames)> audio;
    std::function<void()> end;
    std::function<void()> flush;
    std::function<void(float db)> volume;
};

class Receiver {
public:
    Receiver(Config config, Callbacks callbacks);
    ~Receiver();
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;
    bool start();
    void stop();
    // Disconnects the active sender, leaving discovery/listening enabled.
    // The socket thread acknowledges teardown within timeout_ms. A callback
    // calling disconnect itself only requests teardown and returns false.
    bool disconnect(uint32_t timeout_ms = 1000);
    NcmAirplayStatus status() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace cloudtune::airplay
