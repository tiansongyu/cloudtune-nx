// SPDX-License-Identifier: GPL-3.0-or-later
#include "netease/player.hpp"
#include "netease/storage.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <thread>
#ifdef __SWITCH__
#include "netease/playback_client.h"
#endif
namespace netease {
Player::Player(std::string directory) : directory_(std::move(directory)) {}
Player::~Player() {
    shutdown();
}
void Player::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    if (local_) {
        local_->stop();
        local_.reset();
    }
#ifdef __SWITCH__
    ncmPlaybackExit();
#endif
    background_ = false;
}
bool Player::background() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return background_;
}
std::string Player::error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}
bool Player::start_tools() {
    std::lock_guard<std::mutex> lock(mutex_);
    return ensure_runtime();
}
bool Player::ensure_runtime() {
    if (stopped_) {
        error_ = "播放器正在退出";
        return false;
    }
    if (background_ || local_)
        return true;
#ifdef __SWITCH__
    if (R_SUCCEEDED(ncmPlaybackInitialize())) {
        background_ = remote_seen_ = true;
        return true;
    }
    // A service that is already starting can report "already launched".
    // Always give that process a short bounded chance to register its IPC.
    if (hosversionIsAtmosphere()) {
        const Result launch = ncmPlaybackLaunchBackground();
        if (R_SUCCEEDED(launch))
            remote_seen_ = true;
        for (int i = 0; i < 15; ++i) {
            if (R_SUCCEEDED(ncmPlaybackInitialize())) {
                background_ = remote_seen_ = true;
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    if (remote_seen_) {
        // A known background process can still be starting or recovering.
        // Never create another listener while its ownership is unresolved.
        error_ = "后台服务尚未就绪，请稍后重试";
        return false;
    }
#endif
    playback::EngineConfig c;
    c.directory = directory_;
#ifdef __SWITCH__
    c.ca_bundle = "romfs:/certs/cacert.pem";
#endif
    try {
        local_ = std::make_unique<playback::Runtime>(c);
        if (local_->start())
            return true;
        error_ = "无法初始化播放与工具服务";
    } catch (const std::exception &) {
        error_ = "无法初始化播放与工具服务";
    }
    if (local_) {
        local_->stop();
        local_.reset();
    }
    return false;
}
bool Player::play(const std::vector<Media> &tracks, size_t index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_)
        return false;
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
    if (!ensure_runtime())
        return false;
    return command_locked(NcmCommand_LoadQueue, static_cast<uint32_t>(index - begin));
}
bool Player::command(NcmCommand cmd, uint32_t arg) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_)
        return false;
    return command_locked(cmd, arg);
}
bool Player::clear_account() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_)
        return false;
    try {
        const NcmQueueHeader empty{{'N','C','M','Q','U','E','U','E'}, NCM_PROTOCOL_VERSION, 0};
        storage::write_file(directory_ + "/queue.nsq",
                            std::string(reinterpret_cast<const char*>(&empty), sizeof(empty)));
        if (!ensure_runtime())
            return false;
#ifdef __SWITCH__
        if (background_) {
            if (R_SUCCEEDED(ncmPlaybackClearQueue()))
                return true;
            error_ = "无法清除后台播放队列；请更新后台服务后重试账号切换";
            return false;
        }
#endif
        if (local_ && local_->command(NcmCommand_ClearQueue))
            return true;
        error_ = "播放队列尚未清除，请稍后重试账号切换";
    } catch (const std::exception&) {
        error_ = "无法清除账号播放队列，请检查存储空间";
    }
    return false;
}
bool Player::command_locked(NcmCommand cmd, uint32_t arg) {
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
        case NcmCommand_ClearQueue:
            rc = ncmPlaybackClearQueue();
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
    std::lock_guard<std::mutex> lock(mutex_);
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
NcmAirplayStatus Player::airplay_status() {
    std::lock_guard<std::mutex> lock(mutex_);
    return airplay_status_locked();
}
NcmAirplayStatus Player::airplay_status_locked() {
    NcmAirplayStatus result{};
    result.version = NCM_AIRPLAY_VERSION;
    result.state = NcmAirplay_Error;
#ifdef __SWITCH__
    if (background_) {
        const Result rc = ncmAirplayGetStatus(&result);
        if (R_SUCCEEDED(rc))
            return result;
        // An old playback service can still play music correctly. A rejected
        // tool command must not discard it or create a second LAN listener.
        NcmPlaybackStatus music{};
        if (R_SUCCEEDED(ncmPlaybackGetStatus(&music))) {
            result = {};
            result.state = NcmAirplay_Error;
            result.error_code = rc;
            std::snprintf(result.message, sizeof(result.message), "Update background service");
            error_ = "当前后台服务不支持 AirPlay，请更新后台服务";
        } else {
            ncmPlaybackExit();
            background_ = false;
            result = {};
            result.version = NCM_AIRPLAY_VERSION;
            result.state = NcmAirplay_Error;
            result.error_code = rc;
            std::snprintf(result.message, sizeof(result.message), "Background disconnected");
            error_ = "后台服务连接中断，请稍后重试";
        }
        return result;
    }
#endif
    if (local_)
        return local_->airplay_status();
    std::snprintf(result.message, sizeof(result.message),
                  stopped_ ? "Application stopped" : "Playback service unavailable");
    return result;
}
bool Player::set_airplay_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensure_runtime())
        return false;
#ifdef __SWITCH__
    if (background_) {
        if (airplay_status_locked().version != NCM_AIRPLAY_VERSION || !background_)
            return false;
        if (R_SUCCEEDED(ncmAirplaySetEnabled(enabled)))
            return true;
        error_ = "AirPlay 操作未完成，请查看接收状态并检查网络与存储";
        return false;
    }
#endif
    try {
        if (local_ && local_->set_airplay_enabled(enabled))
            return true;
    } catch (const std::exception &) {
    }
    error_ = "AirPlay 操作未完成，请查看接收状态并检查网络与存储";
    return false;
}
bool Player::disconnect_airplay() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_)
        return false;
#ifdef __SWITCH__
    if (background_) {
        if (airplay_status_locked().version != NCM_AIRPLAY_VERSION || !background_)
            return false;
        if (R_SUCCEEDED(ncmAirplayDisconnect()))
            return true;
        error_ = "无法断开 AirPlay 会话";
        return false;
    }
#endif
    if (!local_) {
        error_ = "AirPlay 服务尚未启动";
        return false;
    }
    try {
        if (local_->disconnect_airplay())
            return true;
    } catch (const std::exception &) {
    }
    error_ = "AirPlay 会话未在限定时间内断开，请重试";
    return false;
}
} // namespace netease
