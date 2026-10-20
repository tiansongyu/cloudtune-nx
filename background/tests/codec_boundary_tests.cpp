/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "audio_decoder.hpp"
#include "pcm_stream.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>
using netease::playback::AudioDecoder;
using netease::playback::PcmStream;
namespace {
void check(bool value, const char* reason) {
    if (!value) throw std::runtime_error(reason);
}
struct Input {
    std::vector<unsigned char> bytes;
    size_t position = 0, reads = 0, seeks = 0, transferred = 0;
    size_t available = 0;
    std::atomic<bool> cancelled{false};
    static size_t read(void* opaque, void* data, size_t count) {
        auto& s = *static_cast<Input*>(opaque);
        while (!s.cancelled && s.available < s.bytes.size() &&
               std::min(count, s.bytes.size() - s.position) > s.available - s.position)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (s.cancelled || ++s.reads > 8192 || s.transferred > 16 * 1024 * 1024)
            return 0;
        count = std::min(count, s.bytes.size() - s.position);
        if (!count) return 0;
        std::memcpy(data, s.bytes.data() + s.position, count);
        s.position += count;
        s.transferred += count;
        return count;
    }
    static bool seek(void* opaque, int64_t offset, AudioDecoder::Origin origin) {
        auto& s = *static_cast<Input*>(opaque);
        if (s.cancelled || ++s.seeks > 8192) return false;
        if (origin == AudioDecoder::Origin::End && s.available < s.bytes.size())
            return false;
        const int64_t base = origin == AudioDecoder::Origin::Start ? 0 :
            origin == AudioDecoder::Origin::Current ? s.position : s.bytes.size();
        if (offset < -base || offset > static_cast<int64_t>(s.bytes.size()) - base)
            return false;
        while (!s.cancelled && static_cast<size_t>(base + offset) > s.available)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (s.cancelled) return false;
        s.position = static_cast<size_t>(base + offset);
        return true;
    }
    static bool tell(void* opaque, int64_t* value) {
        *value = static_cast<Input*>(opaque)->position;
        return true;
    }
};
void pcm_boundaries() {
    for (uint32_t rate : {44100u, 48000u}) {
        PcmStream whole, split;
        check(whole.open(rate) && split.open(rate), "PCM initialization failed");
        std::vector<int16_t> source(PcmStream::Capacity * 2);
        for (size_t i = 0; i < source.size(); ++i)
            source[i] = static_cast<int16_t>(static_cast<int>((i * 997) % 65536) - 32768);
        check(whole.push(source.data(), PcmStream::Capacity), "PCM capacity rejected");
        check(!whole.push(source.data(), 1), "PCM capacity overflow accepted");
        std::vector<int16_t> reference(60000 * 2), output(reference.size());
        const size_t expected = whole.render(reference.data(), 60000, .75f);
        size_t consumed = 0, produced = 0;
        while (consumed < PcmStream::Capacity) {
            const size_t chunk = std::min<size_t>(137, PcmStream::Capacity - consumed);
            check(split.push(source.data() + consumed * 2, chunk), "PCM chunk rejected");
            consumed += chunk;
            produced += split.render(output.data() + produced * 2, 503, .75f);
        }
        produced += split.render(output.data() + produced * 2, 1000, .75f);
        check(produced == expected &&
            std::equal(output.begin(), output.begin() + produced * 2, reference.begin()),
            "PCM packet boundaries changed resampling phase or samples");
        split.pause(true);
        check(split.push(source.data(), 20) && !split.render(output.data(), 20, 1),
              "Paused PCM retained audio");
        split.pause(false);
        check(!split.render(output.data(), 20, 1), "PCM resume replayed buffered audio");
        split.flush();
        split.close();
        check(!split.active() && !split.push(source.data(), 1), "PCM close retained input");
        check(split.open(rate) && !split.render(output.data(), 20, 1),
              "PCM reopen retained previous samples");
    }
    PcmStream concurrent;
    check(concurrent.open(48000), "PCM concurrent initialization failed");
    std::array<int16_t, 64 * 2> chunk{};
    std::atomic<bool> finished{false};
    std::thread producer([&] {
        for (unsigned i = 0; i < 2000; ++i) concurrent.push(chunk.data(), 64);
        finished = true;
    });
    std::array<int16_t, 128 * 2> output{};
    for (unsigned i = 0; i < 2000 || !finished; ++i) {
        concurrent.render(output.data(), 128, 1);
        if (i % 31 == 0) concurrent.flush();
        if (i % 97 == 0) concurrent.pause(false);
    }
    producer.join();
    concurrent.close();
}
void probe(const std::string& path, const std::string& expectation) {
    Input input;
    std::ifstream file(path, std::ios::binary);
    check(bool(file), "Generated fixture missing");
    input.bytes.assign(std::istreambuf_iterator<char>(file), {});
    input.available = input.bytes.size();
    if (expectation == "cancel") {
        input.available = std::min<size_t>(8, input.bytes.size());
        auto pending = std::async(std::launch::async, [&] {
            AudioDecoder interrupted;
            return interrupted.open(Input::read, Input::seek, Input::tell, &input);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        input.cancelled = true;
        check(pending.wait_for(std::chrono::seconds(2)) == std::future_status::ready &&
              !pending.get(), "Blocked decoder open did not cancel");
        return;
    }
    AudioDecoder decoder;
    check(!decoder.open(nullptr, Input::seek, Input::tell, &input),
          "Null read callback accepted");
    const bool opened = decoder.open(Input::read, Input::seek, Input::tell, &input);
    if (expectation == "reject") check(!opened, "Invalid stream accepted");
    if (expectation == "good" || expectation == "short")
        check(opened, "Recognizable stream failed to open");
    if (opened) {
        const auto channels = decoder.channels();
        check(channels >= 1 && channels <= 2 && decoder.sample_rate(),
              "Invalid output format");
        std::array<int16_t, 8192 * 2 + 16> output{};
        std::vector<int16_t> reference;
        uint64_t frames = 0;
        for (unsigned step = 0; step < 2048; ++step) {
            output.fill(12345);
            const size_t request = std::array<size_t, 4>{1, 31, 1024, 8192}[step % 4];
            const size_t count = decoder.read(output.data() + 8, request);
            check(count <= request, "Decoder returned excessive frame count");
            check(std::all_of(output.begin(), output.begin() + 8,
                             [](auto v) { return v == 12345; }) &&
                  std::all_of(output.begin() + 8 + request * channels, output.end(),
                             [](auto v) { return v == 12345; }),
                  "Decoder wrote outside output buffer");
            reference.insert(reference.end(), output.begin() + 8,
                             output.begin() + 8 + count * channels);
            frames += count;
            check(frames <= 1000000, "Decoded output exceeded the fixture budget");
            if (!count) break;
            check(step < 2047, "Decoder did not reach EOF");
        }
        if (expectation == "good") {
            check(!decoder.damaged() && frames >= decoder.sample_rate() * 39 / 100 &&
                  frames < decoder.sample_rate() * 45 / 100,
                  "Valid short fixture was damaged or had wrong duration");
            for (uint64_t target : {uint64_t(0), frames / 2, frames / 4, frames - 32}) {
                check(decoder.seek(target), "Valid sample seek failed");
                check(decoder.read(output.data(), 31) == 31, "Read after seek failed");
                for (size_t i = 0; i < 31 * channels; ++i)
                    if (std::abs(int(output[i]) - int(reference[target * channels + i])) > 1)
                        throw std::runtime_error("Seek changed decoded PCM at frame " +
                            std::to_string(target) + " sample " + std::to_string(i) +
                            ": " + std::to_string(output[i]) + " versus " +
                            std::to_string(reference[target * channels + i]));
            }
            check(decoder.seek(frames) && !decoder.read(output.data(), 1) &&
                  !decoder.damaged(), "Valid EOF seek reported corruption");
            check(!decoder.seek(frames + 1), "Seek beyond decoded EOF accepted");
            decoder.close();
            input.position = 0;
            check(decoder.open(Input::read, Input::seek, Input::tell, &input) &&
                  decoder.read(output.data(), 31) == 31,
                  "Decoder could not reopen after EOF");
            for (size_t i = 0; i < 31 * channels; ++i)
                check(std::abs(int(output[i]) - int(reference[i])) <= 1,
                      "Reopened decoder retained previous state");
        }
        if (expectation == "short")
            check(decoder.damaged(), "Truncated declared-duration stream reported clean EOF");
        check(!decoder.seek(std::numeric_limits<uint64_t>::max()),
              "Extreme frame seek accepted");
        input.cancelled = true;
        decoder.seek(123);
        decoder.read(output.data(), 31);
        check(input.reads <= 8193 && input.seeks <= 8193 &&
              input.transferred <= 17 * 1024 * 1024, "Callback budget exhausted");
    }
    decoder.close();
    decoder.close();
    check(!decoder.channels() && !decoder.sample_rate() &&
          !decoder.frames() && !decoder.damaged(), "Decoder close retained state");
}
}
int main(int argc, char** argv) {
    try {
        check(argc == 3, "usage: codec-boundary-tests AUDIO good|short|reject|any|pcm|cancel");
        if (std::string(argv[2]) == "pcm") pcm_boundaries();
        else probe(argv[1], argv[2]);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
