// SPDX-License-Identifier: GPL-3.0-or-later
#include "netease/player.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#ifdef __SWITCH__
#include "netease/playback_client.h"
#endif
namespace netease {
Player::Player(std::string directory) : directory_(std::move(directory)) {
#ifdef __SWITCH__
    background_ = R_SUCCEEDED(ncmPlaybackInitialize());
#endif
}
Player::~Player() {
    shutdown();
}
void Player::shutdown() {
    if (local_) {
        local_->stop();
        local_.reset();
    }
#ifdef __SWITCH__
    ncmPlaybackExit();
#endif
}
bool Player::ensure_engine() {
    if (background_ || local_)
        return true;
#ifdef __SWITCH__
    if (R_SUCCEEDED(ncmPlaybackInitialize())) {
        background_ = true;
        return true;
    }
    // A service that is already starting can report "already launched".
    // Always give that process a short bounded chance to register its IPC.
    if (hosversionIsAtmosphere()) {
        ncmPlaybackLaunchBackground();
        for (int i = 0; i < 15; ++i) {
            if (R_SUCCEEDED(ncmPlaybackInitialize())) {
                background_ = true;
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
#endif
    playback::EngineConfig c;
    c.directory = directory_;
#ifdef __SWITCH__
    c.ca_bundle = "romfs:/certs/cacert.pem";
#endif
    local_ = std::make_unique<playback::Engine>(c);
    if (!local_->start()) {
        error_ = "无法初始化音频输出";
        local_.reset();
        return false;
    }
    return true;
}
bool Player::play(const std::vector<Media> &tracks, size_t index) {
    if (index >= tracks.size())
        return false;
    // Limit the binary handoff rather than overflowing its fixed format.
    const size_t begin = index >= NCM_MAX_QUEUE ? (index / NCM_MAX_QUEUE) * NCM_MAX_QUEUE : 0;
    const size_t count = std::min<size_t>(tracks.size() - begin, NCM_MAX_QUEUE);
    std::vector<NcmQueueEntry> queue(count);
    for (size_t i = 0; i < count; ++i) {
        const auto &t = tracks[begin + i];
        auto &e = queue[i];
        e.id = t.id;
        e.duration_ms = t.duration_ms;
        auto copy = [](char *d, size_t n, const std::string &s) {
            std::memcpy(d, s.data(), std::min(n - 1, s.size()));
        };
        copy(e.title, sizeof(e.title), t.title);
        copy(e.artist, sizeof(e.artist), t.artist);
        copy(e.album, sizeof(e.album), t.album);
        copy(e.url, sizeof(e.url), t.url);
    }
    if (ncmWriteQueueFile((directory_ + "/queue.nsq").c_str(), queue.data(), queue.size()) != 0) {
        error_ = "无法保存播放队列，请检查 SD 卡空间";
        return false;
    }
    if (!ensure_engine())
        return false;
    return command(NcmCommand_LoadQueue, static_cast<uint32_t>(index - begin));
}
bool Player::command(NcmCommand cmd, uint32_t arg) {
#ifdef __SWITCH__
    if (background_) {
        Result rc = 0;
        switch (cmd) {
        case NcmCommand_LoadQueue:
            rc = ncmPlaybackLoadQueue(arg);
            break;
        case NcmCommand_PlayPause:
            rc = ncmPlaybackPlayPause();
            break;
        case NcmCommand_Play:
            rc = ncmPlaybackPlay();
            break;
        case NcmCommand_Pause:
            rc = ncmPlaybackPause();
            break;
        case NcmCommand_Next:
            rc = ncmPlaybackNext();
            break;
        case NcmCommand_Previous:
            rc = ncmPlaybackPrevious();
            break;
        case NcmCommand_Volume:
            rc = ncmPlaybackSetVolume(arg);
            break;
        case NcmCommand_Seek:
            rc = ncmPlaybackSeek(arg);
            break;
        case NcmCommand_Repeat:
            rc = ncmPlaybackSetRepeat(static_cast<NcmRepeatMode>(arg));
            break;
        case NcmCommand_Shuffle:
            rc = ncmPlaybackSetShuffle(arg != 0);
            break;
        case NcmCommand_Stop:
            rc = ncmPlaybackStop();
            break;
        default:
            return false;
        }
        if (R_FAILED(rc)) {
            ncmPlaybackExit();
            background_ = false;
            error_ = "后台服务连接中断，重新选择歌曲可重连";
            return false;
        }
        return true;
    }
#endif
    return local_ && local_->command(cmd, arg);
}
NcmPlaybackStatus Player::status() {
    NcmPlaybackStatus s{};
    s.volume_permille = 700;
#ifdef __SWITCH__
    if (background_) {
        if (R_FAILED(ncmPlaybackGetStatus(&s))) {
            ncmPlaybackExit();
            background_ = false;
            s.state = NcmPlayback_Error;
            std::strncpy(s.message, "Background service disconnected", sizeof(s.message) - 1);
        }
        return s;
    }
#endif
    return local_ ? local_->status() : s;
}
} // namespace netease
