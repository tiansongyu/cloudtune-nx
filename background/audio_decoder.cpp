/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "audio_decoder.hpp"
#include "aac_decoder.hpp"
#include "allocation_budget.hpp"
#define DR_MP3_NO_STDIO
#define DR_MP3_IMPLEMENTATION
#include "vendor/dr_mp3.h"
#define DR_FLAC_NO_STDIO
#define DR_FLAC_IMPLEMENTATION
#include "vendor/dr_flac.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace netease::playback {
struct AudioDecoder::Impl {
    AllocationBudget memory;
    Read read = nullptr;
    Seek seek = nullptr;
    Tell tell = nullptr;
    void* context = nullptr;
    drmp3 mp3{};
    drflac* flac = nullptr;
    AacDecoder aac;
    bool mp3Open = false, aacOpen = false, damaged = false;
    uint64_t cursor = 0;
    int64_t streamStart = 0;
    std::array<int16_t,4096*8> multichannel{};
    std::array<std::array<int,8>,2> weights{};
    int normalization = 1;
    static size_t onRead(void* p, void* data, size_t bytes) {
        auto& self = *static_cast<Impl*>(p);
        return self.read(self.context, data, bytes);
    }
    template<class Origin> static bool onSeek(void* p, int offset, Origin origin) {
        // Both upstream enums use SET=0, CUR=1, END=2.
        return seek64(p,offset,static_cast<AudioDecoder::Origin>(origin));
    }
    static bool seek64(void* p, int64_t offset, AudioDecoder::Origin where) {
        auto& self = *static_cast<Impl*>(p);
        if (where==AudioDecoder::Origin::Start &&
            (offset<0 || offset>std::numeric_limits<int64_t>::max()-self.streamStart))
            return false;
        return self.seek(self.context, offset +
            (where == AudioDecoder::Origin::Start ? self.streamStart : 0), where);
    }
    static drmp3_bool32 mp3Seek(void* p, int offset, drmp3_seek_origin origin) {
        return onSeek(p, offset, origin);
    }
    static drflac_bool32 flacSeek(void* p, int offset, drflac_seek_origin origin) {
        return onSeek(p, offset, origin);
    }
    template<class Position> static bool onTell(void* p, Position* position) {
        auto& self = *static_cast<Impl*>(p);
        int64_t value = 0;
        const bool ok = self.tell(self.context, &value);
        *position = value - self.streamStart;
        return ok;
    }
    static drmp3_bool32 mp3Tell(void* p, drmp3_int64* position) { return onTell(p, position); }
    static drflac_bool32 flacTell(void* p, drflac_int64* position) { return onTell(p, position); }
    static bool tell64(void* p, int64_t* position) { return onTell(p,position); }
    void prepareDownmix(unsigned channels) {
        weights={};
        weights[0][0]=16384; weights[1][1]=16384;
        const auto both=[this](unsigned index,int gain) { weights[0][index]=weights[1][index]=gain; };
        const auto pair=[this](unsigned index) { weights[0][index]=weights[1][index+1]=11585; };
        // RFC 9639 §9.1.3 standard FLAC channel order. Center/surround -3 dB,
        // LFE -6 dB, then common headroom normalization prevents clipping.
        if (channels==4) pair(2);
        else {
            if (channels>=3) both(2,11585);
            if (channels==5) pair(3);
            if (channels>=6) both(3,8192);
            if (channels==6 || channels==8) pair(4);
            if (channels==7) { both(4,11585); pair(5); }
            if (channels==8) pair(6);
        }
        int left=0,right=0;
        for (unsigned i=0;i<channels;++i) { left+=weights[0][i]; right+=weights[1][i]; }
        normalization=std::max(left,right);
    }
    size_t readFlac(int16_t* output,size_t frames) {
        if (flac->channels<=2) return drflac_read_pcm_frames_s16(flac,frames,output);
        size_t total=0;
        while (total<frames) {
            const size_t requested=std::min<size_t>(frames-total,4096);
            const size_t got=drflac_read_pcm_frames_s16(flac,requested,multichannel.data());
            for (size_t frame=0;frame<got;++frame) for (unsigned side=0;side<2;++side) {
                int64_t sample=0;
                for (unsigned channel=0;channel<flac->channels;++channel)
                    sample+=int64_t(multichannel[frame*flac->channels+channel])*weights[side][channel];
                output[(total+frame)*2+side]=static_cast<int16_t>(sample/normalization);
            }
            total+=got;
            if (got<requested) break;
        }
        return total;
    }
};

AudioDecoder::AudioDecoder() : impl_(std::make_unique<Impl>()) {}
AudioDecoder::~AudioDecoder() { close(); }
bool AudioDecoder::open(Read read, Seek seek, Tell tell, void* context) {
    close();
    if (!read || !seek || !tell) return false;
    auto& s = *impl_;
    s.memory.limited = false;
    s.read = read; s.seek = seek; s.tell = tell; s.context = context;
    unsigned char header[4]{};
    if (read(context, header, sizeof(header)) != sizeof(header)) return false;
    int64_t prefix = 0;
    // Normalize ID3-prefixed FLAC to a stream beginning at its signature.
    // Some FLAC metadata seeks are relative to the signature rather than the
    // physical file, so both SEEK_SET and tell must share this virtual origin.
    while (std::memcmp(header, "ID3", 3) == 0) {
        unsigned char tag[6]{};
        if (read(context,tag,sizeof(tag))!=sizeof(tag) ||
            ((tag[2]|tag[3]|tag[4]|tag[5])&0x80)) return false;
        const uint32_t bytes=(uint32_t(tag[2])<<21)|(uint32_t(tag[3])<<14)|
            (uint32_t(tag[4])<<7)|tag[5];
        prefix+=10+bytes+((tag[1]&0x10)?10:0);
        if (prefix>16*1024*1024 || !seek(context,prefix,Origin::Start) ||
            read(context,header,sizeof(header))!=sizeof(header)) return false;
    }
    const bool flac = std::memcmp(header, "fLaC", 4) == 0 ||
                      std::memcmp(header, "OggS", 4) == 0;
    const bool adts = header[0]==0xff && (header[1]&0xf6)==0xf0;
    unsigned char box[4]{};
    const bool mp4 = read(context,box,sizeof(box))==sizeof(box) &&
        (std::memcmp(box,"ftyp",4)==0 || std::memcmp(box,"moov",4)==0 ||
         std::memcmp(box,"mdat",4)==0 || std::memcmp(box,"free",4)==0 ||
         std::memcmp(box,"wide",4)==0);
    if (flac) {
        s.streamStart=prefix;
        if (!seek(context,prefix,Origin::Start)) return false;
        const drflac_allocation_callbacks allocation{
            &s.memory, AllocationBudget::allocate, AllocationBudget::resize, AllocationBudget::release};
        s.flac = drflac_open(Impl::onRead, Impl::flacSeek, Impl::flacTell, &s, &allocation);
    }
    if (!flac && (adts || mp4)) {
        s.streamStart=prefix;
        if (!seek(context,prefix,Origin::Start)) return false;
        s.aacOpen=s.aac.open(Impl::onRead,Impl::seek64,Impl::tell64,&s);
    }
    if (!flac && !adts && !mp4 && seek(context, 0, Origin::Start)) {
        const drmp3_allocation_callbacks allocation{
            &s.memory, AllocationBudget::allocate, AllocationBudget::resize, AllocationBudget::release};
        s.mp3Open = drmp3_init(&s.mp3, Impl::onRead, Impl::mp3Seek, Impl::mp3Tell,
                              nullptr, &s, &allocation) != 0;
    }
    if ((!s.flac && !s.mp3Open && !s.aacOpen) ||
        (s.flac && s.flac->channels>8) || channels()<1 || channels()>2 || !sample_rate()) {
        const bool limited = resource_limited();
        close();
        s.memory.limited = limited;
        return false;
    }
    if (s.flac && s.flac->channels>2) s.prepareDownmix(s.flac->channels);
    return true;
}
void AudioDecoder::close() {
    auto& s = *impl_;
    if (s.flac) drflac_close(s.flac);
    if (s.mp3Open) drmp3_uninit(&s.mp3);
    s.aac.close();
    s.flac=nullptr; s.mp3Open=false; s.aacOpen=false; s.damaged=false; s.cursor=0; s.streamStart=0;
}
size_t AudioDecoder::read(int16_t* interleaved, size_t frames) {
    auto& s = *impl_;
    const auto got = s.flac ? s.readFlac(interleaved,frames) :
        s.mp3Open ? drmp3_read_pcm_frames_s16(&s.mp3,frames,interleaved) :
        s.aacOpen ? s.aac.read(interleaved,frames) : 0;
    s.cursor += got;
    const auto declared = this->frames();
    if ((s.flac || s.mp3Open) && got < frames && declared && s.cursor < declared)
        s.damaged = true;
    return static_cast<size_t>(got);
}
bool AudioDecoder::seek(uint64_t frame) {
    auto& s = *impl_;
    const auto declared = frames();
    if (declared && frame > declared)
        return false;
    if (s.mp3Open || (s.flac && !s.flac->totalPCMFrameCount && frame)) {
        // A zero STREAMINFO frame count means "unknown", not an empty stream.
        // dr_flac clamps random seeks to that zero. Rewind and discard bounded
        // decoded blocks instead, preserving exact PCM and cancellation via
        // the normal stream callbacks. MP3's internal cursor includes encoder
        // delay; use our audible-frame cursor for both forward and backward
        // seeks so gapless padding never shifts the requested position.
        if (s.flac || !frame || frame < s.cursor) {
            const bool reset = s.flac ? drflac_seek_to_pcm_frame(s.flac, 0) != 0 :
                drmp3_seek_to_pcm_frame(&s.mp3, 0) != 0;
            if (!reset) return false;
            s.cursor = 0;
        }
        s.damaged = false;
        std::array<int16_t,8192> scratch{};
        while (s.cursor<frame) {
            const size_t count=static_cast<size_t>(std::min<uint64_t>(frame-s.cursor,4096));
            if (read(scratch.data(),count)!=count) return false;
        }
        return true;
    }
    const bool ok = s.flac ? drflac_seek_to_pcm_frame(s.flac, frame) != 0 :
        s.aacOpen && s.aac.seek(frame);
    if (ok) { s.cursor = frame; s.damaged = false; }
    return ok;
}
unsigned AudioDecoder::channels() const {
    return impl_->flac ? std::min<unsigned>(impl_->flac->channels,2) :
        impl_->mp3Open ? impl_->mp3.channels : impl_->aacOpen ? impl_->aac.channels() : 0;
}
unsigned AudioDecoder::sample_rate() const {
    return impl_->flac ? impl_->flac->sampleRate : impl_->mp3Open ? impl_->mp3.sampleRate :
        impl_->aacOpen ? impl_->aac.sample_rate() : 0;
}
uint64_t AudioDecoder::frames() const {
    if (impl_->flac) return impl_->flac->totalPCMFrameCount;
    if (impl_->aacOpen) return impl_->aac.frames();
    const auto& mp3 = impl_->mp3;
    const uint64_t trim = uint64_t(mp3.delayInPCMFrames) + mp3.paddingInPCMFrames;
    return impl_->mp3Open && mp3.totalPCMFrameCount != DRMP3_UINT64_MAX &&
        mp3.totalPCMFrameCount > trim ? mp3.totalPCMFrameCount - trim : 0;
}
const char* AudioDecoder::format() const {
    return impl_->flac ? "FLAC" : impl_->mp3Open ? "MP3" :
        impl_->aacOpen ? impl_->aac.format() : "none";
}
bool AudioDecoder::resource_limited() const { return impl_->memory.limited || impl_->aac.resource_limited(); }
bool AudioDecoder::damaged() const {
    return impl_->damaged || (impl_->aacOpen && impl_->aac.damaged());
}
}
