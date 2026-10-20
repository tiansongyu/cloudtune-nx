#pragma once
/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace netease::playback {
/* Bounded handoff from a network decoder to the sole audio worker. Linear
 * interpolation retains its fractional phase across packets. Network threads
 * never call audout/SDL, allocate an unbounded queue or wait for the device. */
class PcmStream {
public:
    static constexpr size_t Capacity = 48000;
    bool open(uint32_t rate) {
        if (rate != 44100 && rate != 48000) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        rate_ = rate; active_ = true; paused_ = false; clear_locked();
        return true;
    }
    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        active_ = false; clear_locked();
    }
    bool active() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return active_;
    }
    void flush() {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_locked();
    }
    void pause(bool paused) {
        std::lock_guard<std::mutex> lock(mutex_);
        paused_ = paused; clear_locked();
    }
    bool push(const int16_t* stereo, size_t frames) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_ || !stereo || frames > Capacity) return false;
        if (paused_) return true; // Resume at the live edge, never replay stale audio.
        if (frames > Capacity - count_) return false;
        for (size_t i = 0; i < frames; ++i) {
            const size_t index = (head_ + count_ + i) % Capacity;
            samples_[index * 2] = stereo[i * 2];
            samples_[index * 2 + 1] = stereo[i * 2 + 1];
        }
        count_ += frames;
        return true;
    }
    size_t render(int16_t* output, size_t limit, float gain) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_ || paused_) return 0;
        size_t produced = 0;
        const double ratio = static_cast<double>(rate_) / 48000.0;
        while (produced < limit && count_ >= 2) {
            const size_t next = (head_ + 1) % Capacity;
            for (size_t channel = 0; channel < 2; ++channel) {
                const auto a = samples_[head_ * 2 + channel];
                const auto b = samples_[next * 2 + channel];
                const double sample = (a + (b - a) * phase_) * gain;
                output[produced * 2 + channel] =
                    static_cast<int16_t>(std::clamp(std::lround(sample), -32768L, 32767L));
            }
            ++produced;
            phase_ += ratio;
            const size_t advance = static_cast<size_t>(phase_);
            head_ = (head_ + advance) % Capacity;
            count_ -= advance;
            phase_ -= advance;
        }
        return produced;
    }
private:
    void clear_locked() { head_ = count_ = 0; phase_ = 0; }
    mutable std::mutex mutex_;
    std::array<int16_t, Capacity * 2> samples_{};
    size_t head_ = 0, count_ = 0;
    uint32_t rate_ = 44100;
    double phase_ = 0;
    bool active_ = false, paused_ = false;
};
}
