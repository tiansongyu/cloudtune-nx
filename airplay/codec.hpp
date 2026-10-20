/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace cloudtune::airplay {
/* One decoder per RAOP session. All calls belong to the receiver thread.
 * Output capacity is in stereo frames, not individual int16_t samples. */
class AlacDecoder {
  public:
    AlacDecoder();
    ~AlacDecoder();
    AlacDecoder(const AlacDecoder &) = delete;
    AlacDecoder &operator=(const AlacDecoder &) = delete;
    bool configure(const std::array<uint32_t, 12> &fmtp);
    bool configure(const std::vector<uint32_t> &fmtp);
    size_t decode(const uint8_t *packet, size_t length, int16_t *output,
                  size_t capacityFrames);
    uint32_t sampleRate() const;
    void reset();
    void close();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace cloudtune::airplay
