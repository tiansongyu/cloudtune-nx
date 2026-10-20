/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../airplay/codec.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
void require(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "ALAC test failed: %s\n", message);
        std::exit(1);
    }
}
struct Bits {
    std::vector<uint8_t> bytes;
    unsigned position = 0;
    void put(uint32_t value, unsigned width) {
        for (unsigned i = width; i; --i) {
            if (!(position % 8))
                bytes.push_back(0);
            bytes.back() |= ((value >> (i - 1)) & 1) << (7 - position % 8);
            ++position;
        }
    }
};
// ALAC's uncompressed stereo element provides an independent exact PCM oracle;
// no encoder library or user audio is involved in this fixture.
std::vector<uint8_t> packet(const std::vector<int16_t> &samples, uint32_t frames) {
    Bits bits;
    bits.put(1, 3); // channel-pair element
    bits.put(0, 4); // instance tag
    bits.put(0, 12);
    bits.put(1, 1); // explicit frame length
    bits.put(0, 2); // no extra bits
    bits.put(1, 1); // uncompressed element
    bits.put(frames, 32);
    for (int16_t sample : samples)
        bits.put(static_cast<uint16_t>(sample), 16);
    bits.put(7, 3); // end element
    return bits.bytes;
}
} // namespace
int main() {
    cloudtune::airplay::AlacDecoder decoder;
    decoder.reset(); // safe before initialization
    const std::array<uint32_t, 12> format{96, 352, 0, 16, 40, 10, 14, 2, 255, 0, 0, 44100};
    require(decoder.configure(format), "valid classic format");
    const std::vector<int16_t> expected{-32768, 32767, -1, 1, 0, 0, 1234, -2345,
                                        12000, -12000, 42, -42, 256, -256, 30000, -30000};
    const auto encoded = packet(expected, 8);
    std::array<int16_t, 8194> output{};
    output.front() = 123;
    output.back() = 456;
    require(decoder.decode(encoded.data(), encoded.size(), output.data() + 1, 4096) == 8,
            "valid stereo frame decodes");
    for (size_t i = 0; i < expected.size(); ++i)
        require(output[i + 1] == expected[i], "decoded PCM equals independent sample oracle");
    require(output.front() == 123 && output.back() == 456, "output guards intact");
    require(decoder.decode(encoded.data(), encoded.size(), output.data() + 1, 7) == 0,
            "insufficient output capacity rejected");
    require(decoder.decode(encoded.data(), encoded.size() - 4, output.data() + 1, 4096) == 0,
            "truncated stereo payload rejected");
    const auto excessive = packet({}, 4097);
    require(decoder.decode(excessive.data(), excessive.size(), output.data() + 1, 4096) == 0,
            "in-band frame-length excess rejected");
    std::vector<uint8_t> oversized(65537);
    require(decoder.decode(oversized.data(), oversized.size(), output.data() + 1, 4096) == 0,
            "packet size bound");
    require(decoder.decode(encoded.data(), encoded.size(), output.data() + 1, 4096) == 8,
            "decoder recovers after invalid packet");
    for (auto mutation : std::array<std::pair<size_t, uint32_t>, 6>{
             {{1, 4097}, {1, 0}, {3, 24}, {7, 1}, {11, 192000}, {6, 255}}}) {
        auto invalid = format;
        invalid[mutation.first] = mutation.second;
        require(!decoder.configure(invalid), "unsupported format rejected");
    }
    auto format48 = format;
    format48[11] = 48000;
    require(decoder.configure(format48) && decoder.sampleRate() == 48000, "48 kHz accepted");
    require(decoder.decode(encoded.data(), encoded.size(), output.data() + 1, 4096) == 8,
            "48 kHz PCM");
    require(!decoder.configure(std::vector<uint32_t>{96, 352}), "short fmtp rejected");
    decoder.close();
    decoder.reset();
    std::puts("ALAC bounds, exact signed stereo PCM, format rejection and recovery passed.");
}
