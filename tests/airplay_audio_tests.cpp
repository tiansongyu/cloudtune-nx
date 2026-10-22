/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "engine.hpp"
#include "pcm_stream.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace netease::playback;
using namespace std::chrono_literals;
static void check(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}
template<class Predicate> static NcmPlaybackStatus wait(Engine& engine, Predicate predicate) {
    for (int i = 0; i < 200; ++i) {
        auto status = engine.status();
        if (predicate(status)) return status;
        std::this_thread::sleep_for(10ms);
    }
    throw std::runtime_error("Timed out waiting for audio state");
}
int main(int argc, char** argv) {
    try {
        check(argc == 3, "usage: audio-tests <private-directory> <generated-mp3>");
        PcmStream stream;
        check(!stream.open(96000), "Unsupported sample rate accepted");
        check(stream.open(44100), "PCM open failed");
        std::vector<int16_t> ramp(441 * 2), output(480 * 2);
        for (size_t i = 0; i < 441; ++i) ramp[i * 2] = ramp[i * 2 + 1] = static_cast<int16_t>(i);
        check(stream.push(ramp.data(), 441), "PCM push failed");
        const auto count = stream.render(output.data(), 480, 1);
        check(count == 479 && std::abs(output[200] - 92) <= 1,
              "44.1 -> 48 kHz phase or sample value incorrect");
        stream.pause(true);
        check(stream.push(ramp.data(), 441) && stream.render(output.data(), 480, 1) == 0,
              "Paused live audio was buffered");
        stream.pause(false);
        check(stream.render(output.data(), 480, 1) == 0, "Resume replayed stale audio");
        check(!stream.push(ramp.data(), PcmStream::Capacity + 1), "Unbounded input accepted");
        stream.close();
        check(!stream.push(ramp.data(), 441), "Closed source accepted audio");
        check(stream.open(44100) && stream.push(ramp.data(), 441), "Block aggregation setup");
        check(stream.render(output.data(), 480, 1, true) == 0,
              "Partial network packet consumed an output buffer");
        check(stream.push(ramp.data(), 441) && stream.render(output.data(), 480, 1, true) == 480,
              "Complete output block did not preserve pending input");
        stream.close();

        const std::string directory = argv[1];
        std::filesystem::create_directories(directory);
        NcmQueueEntry track{};
        track.id = 901; track.duration_ms = 8000;
        std::snprintf(track.title, sizeof(track.title), "Generated music");
        std::snprintf(track.url, sizeof(track.url), "%s", argv[2]);
        check(ncmWriteQueueFile((directory + "/queue.nsq").c_str(), &track, 1) == 0,
              "Queue write failed");
        Engine engine({directory, ""});
        check(engine.start() && engine.command(NcmCommand_LoadQueue), "Music start failed");
        const auto music = wait(engine, [](auto s) { return s.track_id == 901 && s.position_ms > 200; });
        check(engine.begin_external(44100), "External handoff failed");
        std::vector<int16_t> audio(22050 * 2);
        for (size_t i = 0; i < 22050; ++i)
            audio[i * 2] = audio[i * 2 + 1] = static_cast<int16_t>(1000 * std::sin(i * .07));
        check(engine.write_external(audio.data(), 22050), "External PCM rejected");
        wait(engine, [](auto s) { return s.track_id == 0 && s.position_ms > 150; });
        check(engine.external_active(), "External source not active");
        engine.command(NcmCommand_Pause);
        const auto paused = wait(engine, [](auto s) { return s.state == NcmPlayback_Paused; });
        std::this_thread::sleep_for(100ms);
        check(engine.status().position_ms == paused.position_ms, "Paused external clock advanced");
        engine.write_external(audio.data(), 22050); // Must be discarded at the live edge.
        engine.command(NcmCommand_Play);
        wait(engine, [](auto s) { return s.state == NcmPlayback_Playing; });
        engine.flush_external();
        engine.write_external(audio.data(), 22050);
        wait(engine, [](auto s) { return s.position_ms > 100; });
        engine.end_external();
        const auto restored = wait(engine, [](auto s) {
            return s.track_id == 901 && s.state == NcmPlayback_Paused;
        });
        check(restored.position_ms >= music.position_ms, "Handoff lost the saved music cursor");
        check(!engine.external_active(), "External source remained active");
        engine.command(NcmCommand_Play);
        wait(engine, [restored](auto s) { return s.position_ms > restored.position_ms + 100; });
        engine.stop();
        std::puts("PASS: bounded PCM, resampling, pause, flush, exclusive handoff and music resume");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
