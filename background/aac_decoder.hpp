/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "audio_decoder.hpp"

namespace netease::playback {
// Custom AVIO keeps downloading, storage and cancellation under Engine control.
// FFmpeg has no file/network protocols and cannot open a second input itself.
class AacDecoder {
public:
    AacDecoder();
    ~AacDecoder();
    AacDecoder(const AacDecoder&) = delete;
    AacDecoder& operator=(const AacDecoder&) = delete;
    bool open(AudioDecoder::Read, AudioDecoder::Seek, AudioDecoder::Tell, void*);
    void close();
    size_t read(int16_t* interleaved, size_t frames);
    bool seek(uint64_t frame);
    unsigned channels() const;
    unsigned sample_rate() const;
    uint64_t frames() const;
    const char* format() const;
    bool damaged() const;
    bool resource_limited() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
