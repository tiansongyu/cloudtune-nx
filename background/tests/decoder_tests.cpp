/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "audio_decoder.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>
using netease::playback::AudioDecoder;
static void check(bool value, const char* reason) {
    if (!value) throw std::runtime_error(reason);
}
static size_t readFile(void* file, void* data, size_t bytes) {
    return std::fread(data, 1, bytes, static_cast<FILE*>(file));
}
static bool seekFile(void* file, int64_t offset, AudioDecoder::Origin origin) {
    const int whence = origin == AudioDecoder::Origin::Start ? SEEK_SET :
        origin == AudioDecoder::Origin::Current ? SEEK_CUR : SEEK_END;
    return std::fseek(static_cast<FILE*>(file), static_cast<long>(offset), whence) == 0;
}
static bool tellFile(void* file, int64_t* cursor) {
    *cursor = std::ftell(static_cast<FILE*>(file)); return *cursor >= 0;
}
int main(int argc, char** argv) {
    FILE* file = nullptr;
    try {
        check(argc == 6, "usage: decoder-tests AUDIO EXPECTED_S16LE CHANNELS RATE good|unknown|downmix|reject|truncated");
        file = std::fopen(argv[1], "rb");
        check(file != nullptr, "cannot open generated fixture");
        AudioDecoder decoder;
        const bool opened = decoder.open(readFile, seekFile, tellFile, file);
        if (std::strcmp(argv[5], "reject") == 0) {
            check(!opened, "invalid/unsupported stream accepted");
        } else {
            check(opened, "FLAC did not open");
            check(std::strcmp(decoder.format(), "FLAC") == 0, "wrong content-based format detection");
            check(decoder.channels() == static_cast<unsigned>(std::stoul(argv[3])), "wrong channels");
            check(decoder.sample_rate() == static_cast<unsigned>(std::stoul(argv[4])), "wrong sample rate");
            std::vector<int16_t> decoded;
            std::array<int16_t, 2048> pcm{};
            size_t got;
            while ((got = decoder.read(pcm.data(), 1024))) {
                decoded.insert(decoded.end(), pcm.begin(), pcm.begin() + got * decoder.channels());
            }
            if (std::strcmp(argv[5], "truncated") == 0) {
                check(decoder.damaged(), "truncated FLAC treated as a complete song");
            } else {
                check(!decoder.damaged(), "valid FLAC marked damaged");
                std::ifstream reference(argv[2], std::ios::binary | std::ios::ate);
                check(reference.good(), "cannot read reference PCM");
                const auto bytes = reference.tellg();
                std::vector<int16_t> expected(static_cast<size_t>(bytes) / sizeof(int16_t));
                reference.seekg(0);
                reference.read(reinterpret_cast<char*>(expected.data()), bytes);
                const bool downmix=std::strcmp(argv[5],"downmix")==0;
                const auto equal=[downmix](int16_t a,int16_t b) {
                    return downmix ? std::abs(int(a)-int(b))<=2 : a==b;
                };
                check(decoded.size()==expected.size() &&
                      std::equal(decoded.begin(),decoded.end(),expected.begin(),equal),
                      "FLAC PCM differs from signed-16 reference");
                const uint64_t actualFrames=decoded.size()/decoder.channels();
                const bool unknown=std::strcmp(argv[5],"unknown")==0;
                check(unknown ? decoder.frames()==0 : actualFrames==decoder.frames(),
                      "duration differs from decoded PCM");
                for (const auto frame : {actualFrames - 200, uint64_t(0), uint64_t(decoder.sample_rate())}) {
                    check(decoder.seek(frame), "FLAC seek failed");
                    const auto count = decoder.read(pcm.data(), 100);
                    check(count == 100, "seek returned wrong frame count");
                    check(std::equal(pcm.begin(), pcm.begin() + count * decoder.channels(),
                                     expected.begin() + frame * decoder.channels(),equal),
                          "seek returned wrong PCM");
                }
                check(decoder.seek(actualFrames),"seek to EOF failed");
                check(decoder.read(pcm.data(),1)==0 && !decoder.damaged(),"EOF seek produced audio or corruption");
                check(!decoder.seek(actualFrames+1),"seek beyond EOF was accepted");
                check(decoder.seek(0) && decoder.read(pcm.data(),100)==100 &&
                      std::equal(pcm.begin(),pcm.begin()+100*decoder.channels(),expected.begin(),equal),
                      "seek after EOF did not restore valid PCM");
            }
        }
        decoder.close();
        std::fclose(file); file = nullptr;
        std::printf("PASS decoder %s (%s)\n", argv[1], argv[5]);
    } catch (const std::exception& error) {
        if (file) std::fclose(file);
        std::fprintf(stderr, "FAIL decoder: %s\n", error.what()); return 1;
    }
}
