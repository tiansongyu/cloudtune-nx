/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "runtime.hpp"
#include "netease/api.hpp"
#include "netease/storage.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace netease;
static void check(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}
static uint16_t availablePort() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    check(fd >= 0, "Cannot create test socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    check(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "Cannot bind port");
    socklen_t size = sizeof(address);
    check(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0, "Cannot read port");
    close(fd);
    return ntohs(address.sin_port);
}
int main() {
    char temporary[] = "/tmp/ctnx-airplay-runtime-XXXXXX";
    if (!mkdtemp(temporary)) return 1;
    const std::string directory = temporary;
    const std::string path = directory + "/airplay.json";
    int result = 0;
    try {
        Json preferences{{"address","127.0.0.1"}, {"port",availablePort()},
                         {"advertise",false}, {"name","CloudTune Test"}};
        storage::write_file(path, preferences.dump());
        playback::Runtime runtime({directory, ""});
        check(runtime.start(), "Runtime failed to start");
        auto status = runtime.airplay_status();
        check(status.enabled && status.state == NcmAirplay_Ready, "Default-on receiver not ready");
        const auto first = Json::parse(storage::read_file(path));
        check(first.at("device_id").get<std::string>().size() == 12, "Stable receiver identity missing");
        check(runtime.disconnect_airplay(), "Idle receiver failed to disconnect");
        check(runtime.set_airplay_enabled(false), "Disable failed");
        check(!runtime.airplay_status().enabled &&
              runtime.airplay_status().state == NcmAirplay_Disabled, "Disabled state mismatch");
        check(Json::parse(storage::read_file(path)).at("enabled") == false, "Disable not persisted");
        runtime.stop();
        check(runtime.start(), "Runtime restart failed");
        check(!runtime.airplay_status().enabled, "Restart ignored saved off preference");
        check(runtime.set_airplay_enabled(true), "Enable failed");
        check(runtime.airplay_status().state == NcmAirplay_Ready, "Receiver not restarted");
        check(Json::parse(storage::read_file(path)).at("device_id") == first.at("device_id"),
              "Receiver identity changed after restart");
        runtime.stop();

        preferences["name"] = "";
        preferences["enabled"] = true;
        storage::write_file(path, preferences.dump());
        playback::Runtime invalid({directory, ""});
        check(invalid.start(), "Invalid tool config should not break music runtime");
        check(invalid.airplay_status().state == NcmAirplay_Error, "Invalid config not reported");
        const auto before = storage::read_file(path);
        check(!invalid.set_airplay_enabled(false), "Invalid configuration accepted");
        check(storage::read_file(path) == before, "Failed change modified saved preference");
        invalid.stop();
        preferences["name"] = "CloudTune Restored";
        storage::write_file(path, preferences.dump());
        check(invalid.start(), "Restart after config repair failed");
        check(invalid.airplay_status().state == NcmAirplay_Ready, "Stale error survived valid restart");
        invalid.stop();
        std::cout << "PASS: default-on, persistent switch/identity, listener lifecycle and error recovery\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        result = 1;
    }
    std::filesystem::remove_all(directory);
    return result;
}
