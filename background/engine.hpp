#pragma once
/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "netease/playback_protocol.h"
#include <memory>
#include <string>

namespace netease::playback {
struct EngineConfig {
    std::string directory;
    /* Optional PEM CA bundle. Never disables peer or hostname verification. */
    std::string ca_bundle;
};
/* One engine per process owns the audio device. All public methods are thread
 * safe. start() is idempotent. stop() joins the worker and releases audout. */
class Engine {
public:
    explicit Engine(EngineConfig config);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    bool start();
    void stop();
    /* Returns false for invalid arguments or a full bounded command queue. */
    bool command(NcmCommand command, uint32_t argument = 0);
    NcmPlaybackStatus status() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
