// SPDX-License-Identifier: GPL-3.0-or-later
#include "netease/player.hpp"
#include "netease/storage.hpp"
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <thread>

using namespace netease;
static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Predicate>
static NcmPlaybackStatus wait_for_status(Player& player, Predicate predicate,
                                         const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do {
        const auto status = player.status();
        if (predicate(status))
            return status;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error(message);
}
static void registry_playback_tests(const std::string& root, const std::string& queue) {
    const std::string mirror =
        "# Netscape HTTP Cookie File\n.music.163.com\tTRUE\t/\tTRUE\t0\tMUSIC_U\t"
        "synthetic-stale-mirror\n";
    const std::string committed =
        "# Netscape HTTP Cookie File\n.music.163.com\tTRUE\t/\tTRUE\t0\tMUSIC_U\t"
        "synthetic-committed-account\n";
    const auto valid = nlohmann::json{
        {"version", 1}, {"active_user_id", 11},
        {"accounts", nlohmann::json::array({
            {{"user_id", 11}, {"nickname", "Synthetic account"}, {"cookies", committed}}
        })}
    }.dump();
    const NcmCommand commands[] = {NcmCommand_Play, NcmCommand_PlayPause, NcmCommand_LoadQueue};
    unsigned scenario = 0;
    for (const std::string& damaged : {std::string("{\"cookies\":\"synthetic-private\", broken"),
                                      std::string()}) {
        for (const auto command : commands) {
            const auto directory = root + "/registry-" + std::to_string(scenario++);
            storage::write_file(directory + "/airplay.json", "{\"enabled\":false}");
            storage::write_file(directory + "/queue.nsq", queue);
            storage::write_file(directory + "/cookies.txt", mirror);
            storage::write_file(directory + "/accounts.json", damaged);
            auto unchanged = [&](const std::string& registry) {
                check(std::filesystem::is_regular_file(directory + "/accounts.json") &&
                          storage::read_file(directory + "/accounts.json") == registry,
                      "playback changed the account archive");
                check(storage::read_file(directory + "/cookies.txt") == mirror,
                      "playback changed the account cookie mirror");
                check(storage::read_file(directory + "/queue.nsq") == queue,
                      "playback changed the saved queue");
            };
            Player player(directory);
            check(player.start_tools(), "registry playback startup failed");
            check(player.status().state == NcmPlayback_Stopped,
                  "startup activated music through a damaged account archive");
            unchanged(damaged);
            check(player.command(command), "cold playback command was not accepted");
            const auto failed = wait_for_status(player, [](const NcmPlaybackStatus& status) {
                check(status.track_id == 0 && status.queue_count == 0 &&
                          status.position_ms == 0 &&
                          status.state != NcmPlayback_Playing &&
                          status.state != NcmPlayback_Buffering,
                      "damaged account archive allowed saved music to activate");
                return status.state == NcmPlayback_Error;
            }, "damaged account archive did not produce a playback error");
            check(failed.error_code != 0, "damaged account archive has no playback error code");
            const char* end = static_cast<const char*>(
                std::memchr(failed.message, '\0', sizeof(failed.message)));
            const std::string message(failed.message, end ? end - failed.message
                                                         : sizeof(failed.message));
            check(message.find("synthetic-private") == std::string::npos &&
                      message.find("synthetic-stale-mirror") == std::string::npos,
                  "account playback error revealed session contents");
            unchanged(damaged);

            // A read-only check must recover on the next cold command without
            // replacing a stale compatibility mirror with the committed session.
            storage::write_file(directory + "/accounts.json", valid);
            check(player.command(command), "recovered cold playback was not accepted");
            const auto playing = wait_for_status(player, [](const NcmPlaybackStatus& status) {
                // The asynchronous command may not yet have replaced the
                // previous error snapshot when this predicate first runs.
                return status.state == NcmPlayback_Playing && status.position_ms >= 150;
            }, "recovered account archive did not start saved music");
            check(playing.track_id == 1001 && playing.queue_count == 1,
                  "recovered cold playback selected the wrong saved queue");
            unchanged(valid);

            // Damage discovered later must not revoke a queue already loaded
            // into the engine or interfere with its ordinary pause/resume.
            storage::write_file(directory + "/accounts.json", damaged);
            wait_for_status(player, [&](const NcmPlaybackStatus& status) {
                check(status.state == NcmPlayback_Playing && status.track_id == 1001,
                      "account archive damage interrupted already-loaded music");
                return status.position_ms >= playing.position_ms + 100;
            }, "already-loaded music stopped advancing after account archive damage");
            check(player.command(NcmCommand_Pause), "loaded music pause was not accepted");
            const auto paused = wait_for_status(player, [](const NcmPlaybackStatus& status) {
                check(status.state != NcmPlayback_Error, "loaded music pause failed");
                return status.state == NcmPlayback_Paused;
            }, "loaded music did not pause with a damaged account archive");
            check(player.command(NcmCommand_PlayPause), "loaded music resume was not accepted");
            wait_for_status(player, [&](const NcmPlaybackStatus& status) {
                check(status.state != NcmPlayback_Error && status.track_id == 1001 &&
                          status.queue_count == 1,
                      "account archive damage blocked an already-loaded queue");
                return status.state == NcmPlayback_Playing &&
                       status.position_ms >= paused.position_ms + 100;
            }, "loaded music did not resume with a damaged account archive");
            player.shutdown();
            unchanged(damaged);
        }
    }
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
        for (int i = 0; i < 42; ++i)
            song.title += "音";
        song.title += "🎵 Synthetic old account song";
        song.url = argv[2];
        song.duration_ms = 8000;
        check(player.play({song}, 0), "queue publication failed");
        const auto published = storage::read_file(directory + "/queue.nsq");
        check(published.size() == sizeof(NcmQueueHeader) + sizeof(NcmQueueEntry),
              "unexpected saved queue size");
        NcmQueueEntry entry{};
        std::memcpy(&entry, published.data() + sizeof(NcmQueueHeader), sizeof(entry));
        // Strict JSON encoding independently rejects a split UTF-8 code point.
        check(!nlohmann::json(entry.title).dump().empty(), "queue title is not valid UTF-8");
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
        reopened.shutdown();
        registry_playback_tests(directory, published);
        std::cout << "PASS: player account barrier, disk queue tombstone, restart isolation, "
                     "damaged-registry cold playback rejection and loaded queue continuity\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
