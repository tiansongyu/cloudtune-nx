/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "aac_decoder.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using netease::playback::AacDecoder;
using netease::playback::AudioDecoder;
using namespace std::chrono_literals;
namespace {
void require(bool value, const char* reason) {
    if (!value) throw std::runtime_error(reason);
}
struct Input {
    std::vector<unsigned char> bytes;
    size_t position = 0;
    std::atomic<size_t> available{0};
    std::atomic<bool> cancelled{false};
    explicit Input(const std::string& path) {
        std::ifstream file(path, std::ios::binary);
        require(bool(file), "fixture missing");
        bytes.assign(std::istreambuf_iterator<char>(file), {});
        available = bytes.size();
    }
    static size_t read(void* opaque, void* out, size_t count) {
        auto& s = *static_cast<Input*>(opaque);
        while (!s.cancelled && s.position >= s.available && s.available < s.bytes.size())
            std::this_thread::sleep_for(1ms);
        if (s.cancelled || s.position >= s.available) return 0;
        count = std::min({count, s.available.load() - s.position, size_t(4093)});
        std::memcpy(out, s.bytes.data() + s.position, count);
        s.position += count;
        return count;
    }
    static bool seek(void* opaque, int64_t offset, AudioDecoder::Origin origin) {
        auto& s = *static_cast<Input*>(opaque);
        if (s.cancelled || (origin == AudioDecoder::Origin::End && s.available < s.bytes.size()))
            return false;
        const int64_t base = origin == AudioDecoder::Origin::Start ? 0 :
            origin == AudioDecoder::Origin::Current ? s.position : s.bytes.size();
        if (offset < -base || offset > static_cast<int64_t>(s.bytes.size()) - base)
            return false;
        const size_t target = base + offset;
        while (!s.cancelled && target > s.available && s.available < s.bytes.size())
            std::this_thread::sleep_for(1ms);
        if (s.cancelled) return false;
        s.position = target;
        return true;
    }
    static bool tell(void* opaque, int64_t* value) {
        *value = static_cast<Input*>(opaque)->position;
        return true;
    }
};
std::vector<int16_t> all(AacDecoder& decoder) {
    std::vector<int16_t> decoded, buffer(4096 * decoder.channels());
    for (;;) {
        const size_t count = decoder.read(buffer.data(), 4096);
        decoded.insert(decoded.end(), buffer.begin(), buffer.begin() + count * decoder.channels());
        if (count < 4096) break;
        require(decoded.size() < 2 * 192000 * 10, "decoder did not reach EOF");
    }
    return decoded;
}
void compare(const int16_t* left, const int16_t* right, size_t count, int tolerance) {
    for (size_t i = 0; i < count; ++i)
        if (std::abs(int(left[i]) - int(right[i])) > tolerance)
            throw std::runtime_error("PCM mismatch at sample " + std::to_string(i) +
                ": " + std::to_string(left[i]) + " versus " + std::to_string(right[i]));
}
void fixture(const std::string& path, unsigned rate, unsigned channels, bool progressive) {
    Input input(path);
    std::thread download;
    if (progressive) {
        input.available = 7;
        download = std::thread([&] {
            while (!input.cancelled && input.available < input.bytes.size()) {
                std::this_thread::sleep_for(1ms);
                input.available = std::min(input.bytes.size(), input.available.load() + 193);
            }
        });
    }
    try {
        AacDecoder decoder;
        require(decoder.open(Input::read, Input::seek, Input::tell, &input), "AAC open failed");
        if (progressive && path.find("endmoov") == std::string::npos)
            require(input.available < input.bytes.size(), "streamable AAC waited for full download");
        require(decoder.channels() == channels && decoder.sample_rate() == rate,
                "wrong AAC channel layout or sample rate");
        auto pcm = all(decoder);
        require(!decoder.damaged(), "valid AAC reported damaged");
        require(pcm.size() >= rate * channels * 3 && pcm.size() < (rate * 3 + 4096) * channels,
                "wrong decoded duration");
        Input reference(path + ".s16");
        require(reference.bytes.size() / 2 >= pcm.size(), "decoder returned extra reference samples");
        std::vector<int16_t> expected(reference.bytes.size() / 2);
        std::memcpy(expected.data(), reference.bytes.data(), reference.bytes.size());
        std::cout << "Reference " << path << '\n';
        compare(pcm.data(), expected.data(), pcm.size(), 1);
        std::vector<int16_t> probe(3000 * channels);
        for (uint64_t target : {uint64_t(rate * 2), uint64_t(0), uint64_t(rate / 2)}) {
            std::cout << "Seek " << target << '\n';
            require(decoder.seek(target), "AAC seek failed");
            require(decoder.read(probe.data(), 3000) == 3000, "AAC read after seek failed");
            compare(probe.data(), pcm.data() + target * channels, probe.size(), 1);
        }
        decoder.close();
        decoder.close();
        if (download.joinable()) download.join();
        std::cout << "PASS " << path << (progressive ? " progressive" : " complete") << '\n';
    } catch (...) {
        input.cancelled = true;
        if (download.joinable()) download.join();
        throw;
    }
}
}
int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: aac-decoder-tests fixture-directory");
        const std::string directory = argv[1];
        for (bool progressive : {false, true}) {
            fixture(directory + "/adts.bin", 48000, 2, progressive);
            fixture(directory + "/faststart.flac", 48000, 2, progressive);
            fixture(directory + "/endmoov.mp3", 48000, 2, progressive);
            fixture(directory + "/mono.bin", 44100, 1, progressive);
            fixture(directory + "/surround51.bin", 48000, 2, progressive);
            fixture(directory + "/surround71.bin", 48000, 2, progressive);
        }
        Input damaged(directory + "/faststart.flac");
        damaged.bytes.resize(damaged.bytes.size() / 2);
        damaged.available = damaged.bytes.size();
        AacDecoder decoder;
        if (decoder.open(Input::read, Input::seek, Input::tell, &damaged)) {
            (void)all(decoder);
            require(decoder.damaged(), "truncated M4A was silently accepted");
        }
        Input invalid(directory + "/adts.bin");
        invalid.bytes.assign(100, 0);
        invalid.available = invalid.bytes.size();
        require(!decoder.open(Input::read, Input::seek, Input::tell, &invalid),
                "unrecognized bytes were accepted as AAC");
        Input cancelled(directory + "/adts.bin");
        cancelled.available = 4;
        auto pending = std::async(std::launch::async, [&] {
            AacDecoder interrupted;
            return interrupted.open(Input::read, Input::seek, Input::tell, &cancelled);
        });
        std::this_thread::sleep_for(30ms);
        cancelled.cancelled = true;
        require(pending.wait_for(2s) == std::future_status::ready && !pending.get(),
                "progressive AAC cancellation did not finish");
        std::cout << "AAC content decode, reference PCM, seek, progressive, truncation and cancellation passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "AAC test failed: " << error.what() << '\n';
        return 1;
    }
}
