// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "engine.hpp"
#include "netease/library.hpp"
#include "netease/playback_protocol.h"
#include <memory>

namespace netease {
class Player {
  public:
    explicit Player(std::string directory);
    ~Player();
    bool play(const std::vector<Media> &tracks, size_t index);
    bool command(NcmCommand command, uint32_t argument = 0);
    NcmPlaybackStatus status();
    bool background() const {
        return background_;
    }
    const std::string &error() const {
        return error_;
    }
    void shutdown();

  private:
    bool ensure_engine();
    std::string directory_, error_;
    bool background_ = false;
    std::unique_ptr<playback::Engine> local_;
};
} // namespace netease
