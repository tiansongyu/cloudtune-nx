#pragma once
/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <cstddef>
#include <cstdint>
#include <memory>

namespace netease::playback {
/* Both codecs consume the same seekable (possibly still downloading) stream.
 * Detection uses its bytes, never a URL suffix or the server's MIME label. */
class AudioDecoder {
public:
    enum class Origin { Start, Current, End };
    using Read = size_t (*)(void*, void*, size_t);
    using Seek = bool (*)(void*, int64_t, Origin);
    using Tell = bool (*)(void*, int64_t*);
    AudioDecoder();
    ~AudioDecoder();
    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;
    bool open(Read read, Seek seek, Tell tell, void* context);
    void close();
    size_t read(int16_t* interleaved, size_t frames);
    bool seek(uint64_t frame);
    unsigned channels() const;
    unsigned sample_rate() const;
    uint64_t frames() const; // Zero when the stream does not declare a duration.
    const char* format() const;
    bool damaged() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
