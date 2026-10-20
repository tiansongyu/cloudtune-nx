/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "runtime.hpp"
#include "airplay/receiver.hpp"
#include "netease/api.hpp"
#include "netease/storage.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>

namespace netease::playback {
namespace {
template<size_t N> void text(char (&target)[N], const std::string& value) {
    size_t length = std::min(value.size(), N - 1);
    while (length && length < value.size() &&
           (static_cast<unsigned char>(value[length]) & 0xc0) == 0x80) --length;
    std::memcpy(target, value.data(), length); target[length] = 0;
}
}

struct Runtime::Impl {
    explicit Impl(EngineConfig config) : config(std::move(config)), engine(this->config) {}
    EngineConfig config;
    Engine engine;
    mutable std::mutex control;
    std::unique_ptr<cloudtune::airplay::Receiver> receiver;
    std::atomic<bool> accepting{false};
    bool running = false, enabled = true;
    std::string settingsError;
    Json preferences = Json::object();
    cloudtune::airplay::Config airplay;

    void save() {
        preferences["enabled"] = enabled;
        storage::write_file(config.directory + "/airplay.json", preferences.dump(2) + "\n");
    }
    void configure() {
        preferences = Json::object();
        const auto bytes = storage::read_file(config.directory + "/airplay.json");
        if (!bytes.empty()) {
            preferences = Json::parse(bytes, nullptr, false);
            if (!preferences.is_object())
                throw std::runtime_error("Invalid airplay.json");
        }
        enabled = preferences.value("enabled", true);
        std::string identity = preferences.value("device_id", "");
        bool valid = identity.size() == 12;
        for (char c : identity)
            valid = valid && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                              (c >= 'A' && c <= 'F'));
        if (!valid) {
            std::random_device random;
            for (auto& byte : airplay.device_id) byte = static_cast<uint8_t>(random());
            airplay.device_id[0] = (airplay.device_id[0] | 2) & 0xfe;
            char value[13];
            for (size_t i = 0; i < 6; ++i)
                std::snprintf(value + i * 2, 3, "%02X", airplay.device_id[i]);
            identity = value;
            preferences["device_id"] = identity;
        }
        for (size_t i = 0; i < 6; ++i)
            airplay.device_id[i] = static_cast<uint8_t>(std::stoul(identity.substr(i * 2, 2), nullptr, 16));
        airplay.name = preferences.value("name", std::string("CloudTune NX"));
        if (airplay.name.empty() || airplay.name.size() > 40 ||
            airplay.name.find_first_of("\r\n") != std::string::npos)
            throw std::runtime_error("Invalid AirPlay device name");
        const auto port = preferences.value("port", 5000);
        if (port < 1024 || port > 65535) throw std::runtime_error("Invalid AirPlay port");
        airplay.port = static_cast<uint16_t>(port);
        // Useful for isolated loopback tests and hosts with several interfaces.
        airplay.address = preferences.value("address", std::string{});
        airplay.advertise = preferences.value("advertise", true);
    }
    void makeReceiver() {
        cloudtune::airplay::Callbacks callbacks;
        callbacks.begin = [this](uint32_t rate) {
            return accepting.load() && engine.begin_external(rate);
        };
        callbacks.audio = [this](const int16_t* pcm, size_t frames) {
            return accepting.load() && engine.write_external(pcm, frames);
        };
        callbacks.end = [this] { engine.end_external(); };
        callbacks.flush = [this] { engine.flush_external(); };
        callbacks.volume = [this](float db) {
            const auto gain = db <= -144 || !std::isfinite(db) ? 0u :
                static_cast<uint32_t>(1000.0 * std::pow(10.0, std::clamp(db, -30.f, 0.f) / 20.0));
            engine.external_volume(gain);
        };
        receiver = std::make_unique<cloudtune::airplay::Receiver>(airplay, std::move(callbacks));
    }
};

Runtime::Runtime(EngineConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
Runtime::~Runtime() { stop(); }
bool Runtime::start() {
    std::lock_guard<std::mutex> lock(impl_->control);
    if (impl_->running) return true;
    if (!impl_->engine.start()) return false;
    impl_->running = true;
    try {
        impl_->settingsError.clear();
        impl_->configure();
        impl_->makeReceiver();
        impl_->save();
        impl_->accepting = impl_->enabled;
        if (impl_->enabled) impl_->receiver->start();
    } catch (const std::exception& error) {
        impl_->accepting = false;
        impl_->settingsError = error.what();
    }
    // An unavailable LAN/port never prevents NetEase playback.
    return true;
}
void Runtime::stop() {
    std::lock_guard<std::mutex> lock(impl_->control);
    if (!impl_->running) return;
    impl_->accepting = false;
    if (impl_->receiver) impl_->receiver->stop();
    impl_->engine.stop();
    impl_->running = false;
}
bool Runtime::command(NcmCommand command, uint32_t argument) {
    std::lock_guard<std::mutex> lock(impl_->control);
    if (!impl_->running) return false;
    if (command == NcmCommand_LoadQueue || command == NcmCommand_Next ||
        command == NcmCommand_Previous || command == NcmCommand_Stop) {
        impl_->accepting = false;
        const bool disconnected = !impl_->receiver || impl_->receiver->disconnect();
        if (!disconnected) {
            impl_->accepting = impl_->enabled;
            return false;
        }
        const bool queued = impl_->engine.command(command, argument);
        impl_->accepting = impl_->enabled;
        return queued;
    }
    return impl_->engine.command(command, argument);
}
NcmPlaybackStatus Runtime::status() const {
    std::lock_guard<std::mutex> lock(impl_->control);
    auto status = impl_->engine.status();
    if (impl_->receiver && impl_->engine.external_active()) {
        const auto receiver = impl_->receiver->status();
        text(status.title, receiver.title[0] ? receiver.title : "AirPlay 1");
        text(status.artist, receiver.artist[0] ? receiver.artist : receiver.name);
    }
    return status;
}
NcmAirplayStatus Runtime::airplay_status() const {
    std::lock_guard<std::mutex> lock(impl_->control);
    NcmAirplayStatus status{};
    status.version = NCM_AIRPLAY_VERSION;
    status.enabled = impl_->enabled;
    status.state = impl_->enabled ? NcmAirplay_Starting : NcmAirplay_Disabled;
    status.port = impl_->airplay.port;
    text(status.name, impl_->airplay.name);
    if (impl_->receiver) status = impl_->receiver->status();
    status.enabled = impl_->enabled;
    if (!impl_->enabled) status.state = NcmAirplay_Disabled;
    const auto audio = impl_->engine.status();
    if (status.state == NcmAirplay_Playing && audio.track_id == 0 &&
        audio.state == NcmPlayback_Error) {
        status.state = NcmAirplay_Error;
        status.error_code = audio.error_code;
        text(status.message, audio.message);
    }
    if (!impl_->settingsError.empty()) {
        status.state = NcmAirplay_Error;
        status.error_code = 1;
        text(status.message, impl_->settingsError);
    }
    return status;
}
bool Runtime::set_airplay_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(impl_->control);
    if (!impl_->running) return false;
    const bool previous = impl_->enabled;
    const auto previousPreferences = impl_->preferences;
    try {
        if (!impl_->receiver) {
            impl_->configure();
            impl_->makeReceiver();
        }
        impl_->enabled = enabled;
        impl_->save();
        impl_->settingsError.clear();
    } catch (const std::exception& error) {
        impl_->enabled = previous;
        impl_->preferences = previousPreferences;
        impl_->settingsError = error.what();
        return false;
    }
    impl_->accepting = enabled;
    if (enabled) return impl_->receiver->start();
    impl_->receiver->stop();
    return true;
}
bool Runtime::disconnect_airplay() {
    std::lock_guard<std::mutex> lock(impl_->control);
    impl_->accepting = false;
    const bool disconnected = !impl_->receiver || impl_->receiver->disconnect();
    impl_->accepting = impl_->enabled;
    return disconnected;
}
}
