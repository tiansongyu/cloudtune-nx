#pragma once
/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "engine.hpp"
#include "netease/airplay_protocol.h"
#include <memory>

namespace netease::playback {
/* Application/sysmodule owner. Tests using Engine alone never open LAN ports.
 * Playback commands and tool settings are serialized here; only Engine owns
 * the physical audio device. */
class Runtime {
public:
    explicit Runtime(EngineConfig config);
    ~Runtime();
    bool start();
    void stop();
    bool command(NcmCommand command, uint32_t argument = 0);
    NcmPlaybackStatus status() const;
    NcmAirplayStatus airplay_status() const;
    bool set_airplay_enabled(bool enabled);
    bool disconnect_airplay();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
