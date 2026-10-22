// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "netease/library.hpp"
#include "netease/playback_protocol.h"
#include "runtime.hpp"
#include <chrono>
#include <memory>
#include <mutex>

namespace netease {
class Player {
  public:
    explicit Player(std::string directory);
    ~Player();
    bool start_tools();
    bool play(const std::vector<Media> &tracks, size_t index);
    bool command(NcmCommand command, uint32_t argument = 0);
    NcmPlaybackStatus status();
    NcmAirplayStatus airplay_status();
    bool set_airplay_enabled(bool enabled);
    bool disconnect_airplay();
    // Complete the music boundary before publishing another account session.
    bool clear_account();
    bool background() const;
    bool waiting_for_background() const;
    std::string error() const;
    void shutdown();

  private:
    // All helpers below run while mutex_ is held. IPC and local ownership
    // cannot change while the model worker and UI submit commands concurrently.
    bool ensure_runtime();
    bool start_local_runtime();
    bool connect_background();
    bool command_locked(NcmCommand command, uint32_t argument);
    NcmAirplayStatus airplay_status_locked();
    mutable std::mutex mutex_;
    std::string directory_, error_;
    bool background_ = false, remote_seen_ = false, stopped_ = false;
    std::chrono::steady_clock::time_point reconnect_at_{};
    std::unique_ptr<playback::Runtime> local_;
};
} // namespace netease
