// SPDX-License-Identifier: GPL-3.0-or-later
#include "netease/player.hpp"
#include "netease/storage.hpp"
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

using namespace netease;
static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    try {
        const std::string directory = argv[1];
        storage::write_file(directory + "/airplay.json", "{\"enabled\":false}");
        Player player(directory);
        check(player.start_tools(), "player start failed");
        Media song;
        song.id = 1001;
        song.title = "Synthetic old account song";
        song.url = argv[2];
        song.duration_ms = 8000;
        check(player.play({song}, 0), "queue publication failed");
        for (int i = 0; i < 100 && player.status().position_ms < 150; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        check(player.status().position_ms >= 150, "music did not start");
        check(player.clear_account(), "account barrier failed");
        auto status = player.status();
        check(status.queue_count == 0 && status.track_id == 0 &&
              status.state == NcmPlayback_Stopped, "old playback survived boundary");
        const auto bytes = storage::read_file(directory + "/queue.nsq");
        check(bytes.size() == sizeof(NcmQueueHeader), "empty queue not persisted");
        NcmQueueHeader header{};
        std::memcpy(&header, bytes.data(), sizeof(header));
        check(!std::memcmp(header.magic, "NCMQUEUE", 8) && header.count == 0,
              "invalid queue tombstone");
        check(player.command(NcmCommand_Play), "post-switch play not accepted");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        check(player.status().track_id == 0 && player.status().queue_count == 0,
              "old music resumed through transport controls");
        player.shutdown();
        Player reopened(directory);
        check(reopened.start_tools(), "restart failed");
        check(reopened.command(NcmCommand_Play), "reopened play not accepted");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        check(reopened.status().track_id == 0 && reopened.status().queue_count == 0,
              "old music resumed after restart");
        std::cout << "PASS: player account barrier, disk queue tombstone and restart isolation\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
