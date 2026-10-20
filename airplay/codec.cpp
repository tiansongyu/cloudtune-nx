/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "codec.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
}

namespace cloudtune::airplay {
namespace {
constexpr size_t kMaximumPacket = 64 * 1024;
constexpr uint32_t kMaximumFrames = 4096;
void big32(uint8_t *bytes, uint32_t value) {
    bytes[0] = static_cast<uint8_t>(value >> 24);
    bytes[1] = static_cast<uint8_t>(value >> 16);
    bytes[2] = static_cast<uint8_t>(value >> 8);
    bytes[3] = static_cast<uint8_t>(value);
}
} // namespace

struct AlacDecoder::Impl {
    AVCodecContext *context = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    uint32_t rate = 0, maximumFrames = 0;
    ~Impl() { close(); }
    void close() {
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&context);
        rate = maximumFrames = 0;
    }
};
AlacDecoder::AlacDecoder() : impl(std::make_unique<Impl>()) {}
AlacDecoder::~AlacDecoder() = default;

bool AlacDecoder::configure(const std::vector<uint32_t> &fmtp) {
    if (fmtp.size() != 12) {
        close();
        return false;
    }
    std::array<uint32_t, 12> values;
    std::copy(fmtp.begin(), fmtp.end(), values.begin());
    return configure(values);
}
bool AlacDecoder::configure(const std::array<uint32_t, 12> &fmtp) {
    close();
    if (fmtp[0] > 127 || fmtp[1] == 0 || fmtp[1] > kMaximumFrames || fmtp[2] != 0 ||
        fmtp[3] != 16 || fmtp[4] > 255 || fmtp[5] > 255 || fmtp[6] > 31 || fmtp[7] != 2 ||
        fmtp[8] > 65535 || fmtp[9] > kMaximumPacket ||
        (fmtp[11] != 44100 && fmtp[11] != 48000))
        return false;
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_ALAC);
    if (!codec)
        return false;
    impl->context = avcodec_alloc_context3(codec);
    impl->packet = av_packet_alloc();
    impl->frame = av_frame_alloc();
    if (!impl->context || !impl->packet || !impl->frame) {
        close();
        return false;
    }
    AVCodecContext *context = impl->context;
    context->extradata = static_cast<uint8_t *>(av_mallocz(36 + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!context->extradata) {
        close();
        return false;
    }
    context->extradata_size = 36;
    uint8_t *cookie = context->extradata;
    big32(cookie, 36);
    std::memcpy(cookie + 4, "alac", 4);
    big32(cookie + 12, fmtp[1]);
    cookie[16] = static_cast<uint8_t>(fmtp[2]);
    cookie[17] = static_cast<uint8_t>(fmtp[3]);
    cookie[18] = static_cast<uint8_t>(fmtp[4]);
    cookie[19] = static_cast<uint8_t>(fmtp[5]);
    cookie[20] = static_cast<uint8_t>(fmtp[6]);
    cookie[21] = static_cast<uint8_t>(fmtp[7]);
    cookie[22] = static_cast<uint8_t>(fmtp[8] >> 8);
    cookie[23] = static_cast<uint8_t>(fmtp[8]);
    big32(cookie + 24, fmtp[9]);
    big32(cookie + 28, fmtp[10]);
    big32(cookie + 32, fmtp[11]);
    context->sample_rate = static_cast<int>(fmtp[11]);
    av_channel_layout_default(&context->ch_layout, 2);
    context->bits_per_raw_sample = 16;
    context->thread_count = 1;
    context->thread_type = 0;
    context->max_samples = kMaximumFrames * 2;
    context->err_recognition = AV_EF_CRCCHECK | AV_EF_BITSTREAM | AV_EF_EXPLODE;
    if (avcodec_open2(context, codec, nullptr) < 0) {
        close();
        return false;
    }
    impl->rate = fmtp[11];
    impl->maximumFrames = fmtp[1];
    return true;
}
size_t AlacDecoder::decode(const uint8_t *packet, size_t length, int16_t *output,
                           size_t capacityFrames) {
    if (!impl->context || !packet || !output || !length || length > kMaximumPacket ||
        capacityFrames == 0)
        return 0;
    av_packet_unref(impl->packet);
    av_frame_unref(impl->frame);
    if (av_new_packet(impl->packet, static_cast<int>(length)) < 0)
        return 0;
    // av_new_packet supplies the zeroed overread padding required by FFmpeg.
    std::memcpy(impl->packet->data, packet, length);
    int result = avcodec_send_packet(impl->context, impl->packet);
    av_packet_unref(impl->packet);
    if (result < 0 || avcodec_receive_frame(impl->context, impl->frame) < 0) {
        reset();
        return 0;
    }
    AVFrame *frame = impl->frame;
    if (frame->nb_samples <= 0 ||
        static_cast<uint32_t>(frame->nb_samples) > impl->maximumFrames ||
        static_cast<size_t>(frame->nb_samples) > capacityFrames ||
        frame->ch_layout.nb_channels != 2 ||
        (frame->sample_rate && frame->sample_rate != static_cast<int>(impl->rate))) {
        reset();
        return 0;
    }
    const size_t count = static_cast<size_t>(frame->nb_samples);
    if (frame->format == AV_SAMPLE_FMT_S16P && frame->data[0] && frame->data[1] &&
        frame->linesize[0] >= frame->nb_samples * 2) {
        const auto *left = reinterpret_cast<const int16_t *>(frame->data[0]);
        const auto *right = reinterpret_cast<const int16_t *>(frame->data[1]);
        for (size_t i = 0; i < count; ++i) {
            output[i * 2] = left[i];
            output[i * 2 + 1] = right[i];
        }
    } else if (frame->format == AV_SAMPLE_FMT_S16 && frame->data[0] &&
               frame->linesize[0] >= frame->nb_samples * 4) {
        std::memcpy(output, frame->data[0], count * sizeof(int16_t) * 2);
    } else {
        reset();
        return 0;
    }
    av_frame_unref(frame);
    return count;
}
uint32_t AlacDecoder::sampleRate() const { return impl->rate; }
void AlacDecoder::reset() {
    if (impl->context)
        avcodec_flush_buffers(impl->context);
    if (impl->frame)
        av_frame_unref(impl->frame);
    if (impl->packet)
        av_packet_unref(impl->packet);
}
void AlacDecoder::close() { impl->close(); }
} // namespace cloudtune::airplay
