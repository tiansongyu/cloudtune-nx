// SPDX-License-Identifier: GPL-3.0-or-later
// Compile the real Player's Switch branch against fake system endpoints. The
// local Runtime is real, so accidental fallback/dual ownership is observable.
#include "netease/playback_client.h"
#include "netease/player.hpp"
#include "netease/storage.hpp"
#include <cassert>
#include <filesystem>
#include <iostream>
#include <thread>
#include <unistd.h>

namespace {
bool available, active;
Result connect_result = MAKERESULT(Module_Libnx, LibnxError_NotFound);
Result process_result = 0, launch_result = 0, control_result;
unsigned polls, controls, attempts;
} // namespace
extern "C" {
bool ncmPlaybackEnvironmentSupported() {
    return true;
}
Result ncmPlaybackInitialize() {
    ++attempts;
    active = available;
    return available ? 0 : connect_result;
}
void ncmPlaybackExit() {
    active = false;
}
void ncmPlaybackShutdown() {
    active = false;
}
bool ncmPlaybackIsActive() {
    return active;
}
void ncmPlaybackConnectionMessage(char *out, size_t size) {
    snprintf(out, size, "Synthetic startup diagnostic");
}
Result ncmPlaybackGetBackgroundProcessId(u64 *pid) {
    *pid = process_result ? 0 : 123;
    return process_result;
}
Result ncmPlaybackLaunchBackground() {
    return launch_result;
}
Result ncmPlaybackGetStatus(NcmPlaybackStatus *out) {
    ++polls;
    *out = {};
    if (!available) {
        active = false;
        return 1;
    }
    out->version = NCM_PROTOCOL_VERSION;
    out->track_id = 42;
    return 0;
}
Result ncmAirplayGetStatus(NcmAirplayStatus *out) {
    *out = {};
    out->version = NCM_AIRPLAY_VERSION;
    return 0;
}
Result ncmAirplaySetEnabled(bool) {
    return 0;
}
Result ncmAirplayDisconnect() {
    return 0;
}
#define SIMPLE(name)                                                                               \
    Result name() {                                                                                \
        ++controls;                                                                                \
        return control_result;                                                                     \
    }
#define VALUE(name, type)                                                                          \
    Result name(type) {                                                                            \
        ++controls;                                                                                \
        return control_result;                                                                     \
    }
SIMPLE(ncmPlaybackPlayPause)
SIMPLE(ncmPlaybackPlay)
SIMPLE(ncmPlaybackPause)
SIMPLE(ncmPlaybackNext) SIMPLE(ncmPlaybackPrevious) SIMPLE(ncmPlaybackStop)
    SIMPLE(ncmPlaybackClearQueue) VALUE(ncmPlaybackLoadQueue, uint32_t)
        VALUE(ncmPlaybackSetVolume, uint32_t) VALUE(ncmPlaybackSeek, uint32_t)
            VALUE(ncmPlaybackSetRepeat, NcmRepeatMode) VALUE(ncmPlaybackSetShuffle, bool)
}
int main() {
    char root[] = "/tmp/cloudtune-connection-XXXXXX";
    assert(mkdtemp(root));
    netease::storage::write_file(std::string(root) + "/airplay.json", "{\"enabled\":false}");
    {
        netease::Player player(root);
        // PM says the process is alive; IPC registration is later than the
        // initial foreground startup attempt. Never create a local Runtime.
        assert(!player.start_tools() && !player.background());
        assert(player.waiting_for_background());
        assert(player.airplay_status().state == NcmAirplay_Error);
        assert(player.error().find("Synthetic startup diagnostic") != std::string::npos);
        const unsigned before = attempts;
        for (int i = 0; i < 100; ++i)
            player.status();
        assert(attempts - before <= 1); // Retry cadence is bounded.
        available = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2050));
        assert(player.status().track_id == 42 && player.background());
        assert(!player.waiting_for_background());
        assert(player.error().empty());
        control_result = MAKERESULT(Module_Libnx, LibnxError_BadInput);
        assert(!player.command(NcmCommand_Pause) && player.background());
        control_result = 0;
        assert(player.command(NcmCommand_Pause) && player.background());
        available = false;
        assert(player.status().state == NcmPlayback_Error && !player.background());
        available = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2050));
        assert(player.status().track_id == 42 && player.background());
        player.shutdown();
        const unsigned stopped_attempts = attempts;
        player.status();
        assert(attempts == stopped_attempts);
    }
    available = false;
    process_result = 0x1234;
    launch_result = 0x1234;
    {
        netease::Player player(root);
        assert(!player.start_tools()); // PM errors are not "process absent".
    }
    process_result = MAKERESULT(15, 1);
    launch_result = 0; // Launch was accepted, but the resource gate then exited.
    {
        netease::Player player(root);
        assert(player.start_tools() && !player.background()); // NRO-only mode survives.
        assert(player.airplay_status().state == NcmAirplay_Disabled);
    }
    process_result = 0;
    {
        netease::Player player(root);
        assert(!player.start_tools() && player.waiting_for_background());
        process_result = MAKERESULT(15, 1);
        player.status(); // Confirmed process exit releases the stale owner.
        assert(!player.waiting_for_background() && !player.background());
        assert(player.airplay_status().state == NcmAirplay_Disabled);
        assert(player.error().empty());
    }
    std::filesystem::remove_all(root);
    std::cout << "PASS: delayed background registration, bounded reconnect, rejected controls, PM "
                 "uncertainty and NRO-only playback\n";
}
