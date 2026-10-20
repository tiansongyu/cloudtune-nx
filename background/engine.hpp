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
/* One engine per process owns the audio device. command()/status() are thread
 * safe; the owner serializes start()/stop(). start() is idempotent. stop()
 * joins the worker and releases audout. */
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
    /* Independent tools deliver bounded stereo signed-16 PCM. The same worker
     * pauses music and owns the output device; sender threads never open it. */
    bool begin_external(uint32_t sample_rate);
    bool write_external(const int16_t* stereo, size_t frames);
    void flush_external();
    void end_external();
    void external_volume(uint32_t permille);
    bool external_active() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
