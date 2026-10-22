/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "aac_decoder.hpp"
#include "../airplay/codec_limits.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
}

namespace netease::playback {
struct AacDecoder::Impl {
    AudioDecoder::Read on_read = nullptr;
    AudioDecoder::Seek on_seek = nullptr;
    AudioDecoder::Tell on_tell = nullptr;
    void* opaque = nullptr;
    AVFormatContext* input = nullptr;
    AVIOContext* io = nullptr;
    AVCodecContext* codec = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    int stream = -1;
    unsigned rate = 0, channel_count = 0, source_channels = 0;
    uint64_t total = 0, cursor = 0;
    bool failed = false, input_end = false, draining = false, finished = false, adts = false;
    bool limited = false;
    std::vector<int16_t> pcm;
    size_t pcm_offset = 0;

    ~Impl() { close(); }
    void close() {
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&codec);
        avformat_close_input(&input);
        // avformat may replace this buffer while probing; free the current one.
        if (io) av_freep(&io->buffer);
        avio_context_free(&io);
        stream = -1; rate = channel_count = source_channels = 0; total = cursor = 0;
        failed = input_end = draining = finished = adts = limited = false;
        pcm.clear(); pcm_offset = 0;
    }
    static int input_read(void* opaque, uint8_t* bytes, int count) {
        auto& s = *static_cast<Impl*>(opaque);
        if (count <= 0) return AVERROR(EINVAL);
        const size_t got = s.on_read(s.opaque, bytes, static_cast<size_t>(count));
        return got ? static_cast<int>(got) : AVERROR_EOF;
    }
    static int64_t input_seek(void* opaque, int64_t offset, int whence) {
        auto& s = *static_cast<Impl*>(opaque);
        int64_t position = 0;
        if (whence & AVSEEK_SIZE) {
            if (!s.on_tell(s.opaque, &position)) return AVERROR(EIO);
            // An unfinished progressive stream reports size as unavailable.
            if (!s.on_seek(s.opaque, 0, AudioDecoder::Origin::End))
                return AVERROR(ENOSYS);
            int64_t size = 0;
            const bool got = s.on_tell(s.opaque, &size);
            if (!s.on_seek(s.opaque, position, AudioDecoder::Origin::Start))
                return AVERROR(EIO);
            return got ? size : AVERROR(EIO);
        }
        whence &= ~AVSEEK_FORCE;
        AudioDecoder::Origin origin;
        if (whence == SEEK_SET) origin = AudioDecoder::Origin::Start;
        else if (whence == SEEK_CUR) origin = AudioDecoder::Origin::Current;
        else if (whence == SEEK_END) origin = AudioDecoder::Origin::End;
        else return AVERROR(EINVAL);
        if (!s.on_seek(s.opaque, offset, origin) || !s.on_tell(s.opaque, &position))
            return AVERROR(EIO);
        return position;
    }
    static int deny_external_input(AVFormatContext*, AVIOContext**, const char*, int,
                                   AVDictionary**) {
        return AVERROR(EPERM);
    }
    bool convert() {
        if ((frame->flags & AV_FRAME_FLAG_CORRUPT) || frame->nb_samples < 0 ||
            frame->nb_samples > 8192 ||
            frame->ch_layout.nb_channels != static_cast<int>(source_channels) ||
            frame->sample_rate != static_cast<int>(rate) || !frame->extended_data)
            return false;
        const auto sample_format = static_cast<AVSampleFormat>(frame->format);
        const bool planar = av_sample_fmt_is_planar(sample_format);
        const auto packed = av_get_packed_sample_fmt(sample_format);
        const int bytes = av_get_bytes_per_sample(sample_format);
        if (bytes <= 0) return false;
        std::array<std::array<double, 2>, 8> weights{};
        double normalization = 1;
        if (source_channels > 2) {
            double left = 0, right = 0;
            for (unsigned c = 0; c < source_channels; ++c) {
                switch (av_channel_layout_channel_from_index(&frame->ch_layout, c)) {
                case AV_CHAN_FRONT_LEFT: weights[c] = {1, 0}; break;
                case AV_CHAN_FRONT_RIGHT: weights[c] = {0, 1}; break;
                case AV_CHAN_FRONT_CENTER: weights[c] = {.707106781, .707106781}; break;
                case AV_CHAN_LOW_FREQUENCY: weights[c] = {.5, .5}; break;
                case AV_CHAN_BACK_LEFT: case AV_CHAN_SIDE_LEFT:
                case AV_CHAN_FRONT_LEFT_OF_CENTER: weights[c] = {.707106781, 0}; break;
                case AV_CHAN_BACK_RIGHT: case AV_CHAN_SIDE_RIGHT:
                case AV_CHAN_FRONT_RIGHT_OF_CENTER: weights[c] = {0, .707106781}; break;
                case AV_CHAN_BACK_CENTER: weights[c] = {.5, .5}; break;
                default: return false;
                }
                left += weights[c][0]; right += weights[c][1];
            }
            normalization = std::max(left, right);
        }
        pcm.resize(static_cast<size_t>(frame->nb_samples) * channel_count);
        pcm_offset = 0;
        for (int i = 0; i < frame->nb_samples; ++i) {
            std::array<double, 2> output{};
            for (unsigned c = 0; c < source_channels; ++c) {
                const auto* plane = frame->extended_data[planar ? c : 0];
                if (!plane) return false;
                const size_t index = planar ? static_cast<size_t>(i) : i * source_channels + c;
                const auto* sample = plane + index * bytes;
                double value;
                switch (packed) {
                case AV_SAMPLE_FMT_FLT: { float v; std::memcpy(&v, sample, sizeof(v)); value = v; break; }
                case AV_SAMPLE_FMT_DBL: { double v; std::memcpy(&v, sample, sizeof(v)); value = v; break; }
                case AV_SAMPLE_FMT_S16: { int16_t v; std::memcpy(&v, sample, sizeof(v)); value = v / 32768.0; break; }
                case AV_SAMPLE_FMT_S32: { int32_t v; std::memcpy(&v, sample, sizeof(v)); value = v / 2147483648.0; break; }
                case AV_SAMPLE_FMT_U8: value = (int(*sample) - 128) / 128.0; break;
                default: return false;
                }
                if (!std::isfinite(value)) return false;
                if (source_channels <= 2) output[c] = value;
                else {
                    output[0] += value * weights[c][0] / normalization;
                    output[1] += value * weights[c][1] / normalization;
                }
            }
            for (unsigned c = 0; c < channel_count; ++c)
                pcm[static_cast<size_t>(i) * channel_count + c] = static_cast<int16_t>(
                    std::lrint(std::clamp(output[c], -1.0, 32767.0 / 32768.0) * 32768.0));
        }
        return true;
    }
    bool next_frame() {
        while (!failed && !finished) {
            av_frame_unref(frame);
            int result = avcodec_receive_frame(codec, frame);
            limited |= result == AVERROR(ENOMEM);
            if (result == 0) {
                if (!rate) {
                    rate = frame->sample_rate;
                    source_channels = frame->ch_layout.nb_channels;
                    channel_count = std::min(2u, source_channels);
                }
                if (source_channels < 1 || source_channels > 8 || !rate || rate > 192000 ||
                    !convert()) {
                    failed = true;
                    return false;
                }
                if (!pcm.empty()) return true;
                continue;
            }
            if (result == AVERROR_EOF) { finished = true; break; }
            if (result != AVERROR(EAGAIN)) { failed = true; break; }
            if (input_end) {
                if (draining) { failed = true; break; }
                result = avcodec_send_packet(codec, nullptr);
                limited |= result == AVERROR(ENOMEM);
                draining = true;
                if (result < 0 && result != AVERROR_EOF) failed = true;
                continue;
            }
            for (;;) {
                av_packet_unref(packet);
                result = av_read_frame(input, packet);
                limited |= result == AVERROR(ENOMEM);
                if (result < 0 || packet->stream_index == stream) break;
            }
            if (result < 0) {
                input_end = true;
                if (result != AVERROR_EOF) failed = true;
                continue;
            }
            if (packet->flags & AV_PKT_FLAG_CORRUPT) {
                failed = true;
                break;
            }
            result = avcodec_send_packet(codec, packet);
            limited |= result == AVERROR(ENOMEM);
            av_packet_unref(packet);
            if (result < 0) failed = true;
        }
        return false;
    }
};

AacDecoder::AacDecoder() : impl_(std::make_unique<Impl>()) { cloudtune::audio::configure_limits(); }
AacDecoder::~AacDecoder() = default;
bool AacDecoder::open(AudioDecoder::Read read, AudioDecoder::Seek seek,
                       AudioDecoder::Tell tell, void* opaque) {
    close();
    if (!read || !seek || !tell) return false;
    auto& s = *impl_;
    s.on_read = read; s.on_seek = seek; s.on_tell = tell; s.opaque = opaque;
    auto fail = [&](int result = 0) {
        const bool limited = s.limited || result == AVERROR(ENOMEM);
        close();
        s.limited = limited;
        return false;
    };
    if (!seek(opaque, 0, AudioDecoder::Origin::Start)) return false;
    auto* buffer = static_cast<uint8_t*>(av_malloc(32768));
    if (!buffer) return fail(AVERROR(ENOMEM));
    s.io = avio_alloc_context(buffer, 32768, 0, &s, Impl::input_read, nullptr, Impl::input_seek);
    if (!s.io) { av_free(buffer); return fail(AVERROR(ENOMEM)); }
    // A seek callback describes the cache's random access, not whether the
    // complete file exists yet. MOV otherwise seeks past mdat looking for more
    // atoms and waits for EOF even when faststart's moov is already available.
    if (Impl::input_seek(&s, 0, AVSEEK_SIZE) < 0)
        s.io->seekable = 0;
    s.input = avformat_alloc_context();
    if (!s.input) return fail(AVERROR(ENOMEM));
    s.input->pb = s.io;
    s.input->flags |= AVFMT_FLAG_CUSTOM_IO;
    s.input->io_open = Impl::deny_external_input;
    s.input->probesize = 65536;
    s.input->max_analyze_duration = AV_TIME_BASE;
    s.input->max_streams = 4;
    s.input->max_probe_packets = 64;
    s.input->max_index_size = 256 * 1024;
    int result = avformat_open_input(&s.input, nullptr, nullptr, nullptr);
    if (result < 0) return fail(result);
    s.io->seekable = AVIO_SEEKABLE_NORMAL;
    // M4A headers normally already contain a complete AAC description.
    // Probing them again retains a second decoder and buffered packets in
    // AVFormatContext. Headerless ADTS still needs the normal probe path.
    bool described = false;
    for (unsigned i = 0; i < s.input->nb_streams; ++i) {
        const auto* parameters = s.input->streams[i]->codecpar;
        described |= parameters->codec_id == AV_CODEC_ID_AAC && parameters->sample_rate > 0 &&
                     parameters->ch_layout.nb_channels > 0 && parameters->extradata_size > 0;
    }
    if (!described) {
        result = avformat_find_stream_info(s.input, nullptr);
        if (result < 0) return fail(result);
    }
    s.adts = std::strcmp(s.input->iformat->name, "aac") == 0;
    for (unsigned i = 0; i < s.input->nb_streams; ++i)
        if (s.input->streams[i]->codecpar->codec_id == AV_CODEC_ID_AAC) {
            s.stream = static_cast<int>(i); break;
        }
    if (s.stream < 0) return fail();
    const AVCodec* decoder = avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (!decoder) return fail();
    s.codec = avcodec_alloc_context3(decoder);
    s.packet = av_packet_alloc();
    s.frame = av_frame_alloc();
    auto* stream = s.input->streams[s.stream];
    if (!s.codec || !s.packet || !s.frame) return fail(AVERROR(ENOMEM));
    result = avcodec_parameters_to_context(s.codec, stream->codecpar);
    if (result < 0) return fail(result);
    s.codec->pkt_timebase = stream->time_base;
    s.codec->thread_count = 1;
    s.codec->thread_type = 0;
    s.codec->max_samples = 65536;
    s.codec->err_recognition = AV_EF_CRCCHECK | AV_EF_BITSTREAM | AV_EF_EXPLODE;
    result = avcodec_open2(s.codec, decoder, nullptr);
    if (result < 0) return fail(result);
    if (!s.next_frame()) return fail();
    // ADTS durations are bitrate estimates. Never trim real frames to an estimate.
    if (!s.adts && stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
        const int64_t frames = av_rescale_q(stream->duration, stream->time_base,
                                            AVRational{1, static_cast<int>(s.rate)});
        if (frames > 0) s.total = static_cast<uint64_t>(frames);
    }
    return true;
}
void AacDecoder::close() { impl_->close(); }
size_t AacDecoder::read(int16_t* out, size_t frames) {
    auto& s = *impl_;
    if (!s.codec || !out || !frames) return 0;
    if (s.total) frames = static_cast<size_t>(std::min<uint64_t>(frames, s.total - s.cursor));
    size_t done = 0;
    while (done < frames) {
        if (s.pcm_offset == s.pcm.size() && !s.next_frame()) break;
        const size_t count = std::min(frames - done, (s.pcm.size() - s.pcm_offset) / s.channel_count);
        std::memcpy(out + done * s.channel_count, s.pcm.data() + s.pcm_offset,
                    count * s.channel_count * sizeof(int16_t));
        s.pcm_offset += count * s.channel_count;
        s.cursor += count; done += count;
    }
    if (s.finished && s.total && s.cursor < s.total) s.failed = true;
    return done;
}
bool AacDecoder::seek(uint64_t target) {
    auto& s = *impl_;
    if (!s.codec || (s.total && target > s.total)) return false;
    // Starting from the beginning preserves AAC priming and gives sample-accurate
    // seeking even for ADTS without a timestamp index or a reliable bitrate.
    // Reopening also resets AAC's perceptual-noise RNG and MP4 edit-list delay;
    // a codec flush alone need not reproduce the initial priming samples.
    const auto read_callback = s.on_read;
    const auto seek_callback = s.on_seek;
    const auto tell_callback = s.on_tell;
    void* context = s.opaque;
    if (!open(read_callback, seek_callback, tell_callback, context)) return false;
    std::array<int16_t, 8192> scratch{};
    while (s.cursor < target) {
        const size_t count = static_cast<size_t>(std::min<uint64_t>(
            target - s.cursor, scratch.size() / s.channel_count));
        if (read(scratch.data(), count) != count) return false;
    }
    return true;
}
unsigned AacDecoder::channels() const { return impl_->channel_count; }
unsigned AacDecoder::sample_rate() const { return impl_->rate; }
uint64_t AacDecoder::frames() const { return impl_->total; }
const char* AacDecoder::format() const { return impl_->codec ? "AAC" : "none"; }
bool AacDecoder::damaged() const { return impl_->failed; }
bool AacDecoder::resource_limited() const { return impl_->limited; }
}
