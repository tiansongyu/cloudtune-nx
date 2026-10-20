/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "engine.hpp"
#include "netease/api.hpp"
#define DR_MP3_IMPLEMENTATION
#include "vendor/dr_mp3.h"
#include <curl/curl.h>
#ifdef __SWITCH__
#include <switch.h>
#include <malloc.h>
#else
#include <SDL2/SDL.h>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

namespace netease::playback {
namespace {
constexpr size_t FramesPerBuffer = 2048;
constexpr uint32_t SampleRate = 48000;
constexpr uint64_t MaxDownloadBytes = 128u * 1024u * 1024u;
using namespace std::chrono_literals;
template <size_t N> void copyText(char (&dst)[N], const char* src) {
    std::snprintf(dst, N, "%s", src ? src : "");
    // Avoid leaving a partial UTF-8 character at the end of fixed ABI strings.
    size_t len = std::strlen(dst);
    if (len == N - 1) {
        size_t start = len;
        while (start && (static_cast<unsigned char>(dst[start-1]) & 0xC0) == 0x80) --start;
        if (start) {
            unsigned char lead = dst[start-1];
            size_t needed = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
            if (len - (start-1) < needed) dst[start-1] = 0;
        }
    }
}
class AudioSink {
public:
    ~AudioSink() { close(); }
    bool open() {
        if (active_) return true;
#ifdef __SWITCH__
        if (R_FAILED(audoutInitialize())) return false;
        active_ = true;
        for (auto& buffer : buffers_) {
            buffer.buffer = memalign(0x1000, 0x2000);
            if (!buffer.buffer) { close(); return false; }
            buffer.buffer_size = 0x2000;
            buffer.data_size = 0;
        }
        active_ = true;
        if (R_FAILED(audoutStartAudioOut())) { close(); return false; }
#else
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return false;
        SDL_AudioSpec wanted{};
        wanted.freq = SampleRate;
        wanted.format = AUDIO_S16SYS;
        wanted.channels = 2;
        wanted.samples = FramesPerBuffer;
        device_ = SDL_OpenAudioDevice(nullptr, 0, &wanted, nullptr, 0);
        if (!device_) return false;
        active_ = true;
        SDL_PauseAudioDevice(device_, 0);
#endif
        return true;
    }
    void close() {
#ifdef __SWITCH__
        if (active_) { audoutStopAudioOut(); audoutExit(); }
        for (auto& buffer : buffers_) {
            if (buffer.buffer) std::free(buffer.buffer);
            buffer = {};
        }
        busy_.fill(false);
#else
        if (device_) SDL_CloseAudioDevice(device_);
        device_ = 0;
#endif
        active_ = false;
    }
    void flush() {
#ifdef __SWITCH__
        if (active_) {
            audoutStopAudioOut();
            bool flushed = false;
            audoutFlushAudioOutBuffers(&flushed);
            AudioOutBuffer* released = nullptr;
            u32 count = 0;
            // libnx returns one tag per call. Drain all old tags before
            // reusing their addresses for newly submitted PCM buffers.
            do {
                released = nullptr; count = 0;
                if (R_FAILED(audoutGetReleasedAudioOutBuffer(&released, &count))) break;
            } while (count);
            busy_.fill(false);
            audoutStartAudioOut();
        }
#else
        if (device_) SDL_ClearQueuedAudio(device_);
#endif
    }
    void pause(bool paused) {
        if (!active_) return;
#ifdef __SWITCH__
        if (paused) audoutStopAudioOut(); else audoutStartAudioOut();
#else
        SDL_PauseAudioDevice(device_, paused ? 1 : 0);
#endif
    }
    bool canWrite() {
        if (!active_) return false;
#ifdef __SWITCH__
        AudioOutBuffer* released = nullptr;
        u32 count = 0;
        if (R_FAILED(audoutGetReleasedAudioOutBuffer(&released, &count))) return false;
        while (released) {
            for (size_t i=0; i<buffers_.size(); ++i) if (&buffers_[i] == released) busy_[i] = false;
            released = released->next;
        }
        return std::find(busy_.begin(), busy_.end(), false) != busy_.end();
#else
        return SDL_GetQueuedAudioSize(device_) < FramesPerBuffer * 4 * 3;
#endif
    }
    bool write(const int16_t* data, size_t frames) {
#ifdef __SWITCH__
        for (size_t i=0; i<buffers_.size(); ++i) if (!busy_[i]) {
            auto& buffer = buffers_[i];
            buffer.next = nullptr;
            buffer.data_offset = 0;
            buffer.data_size = frames * 4;
            std::memcpy(buffer.buffer, data, buffer.data_size);
            armDCacheFlush(buffer.buffer, buffer.data_size);
            if (R_FAILED(audoutAppendAudioOutBuffer(&buffer))) return false;
            busy_[i] = true;
            return true;
        }
        return false;
#else
        return SDL_QueueAudio(device_, data, static_cast<uint32_t>(frames * 4)) == 0;
#endif
    }
    uint32_t queuedMs() const {
#ifdef __SWITCH__
        return static_cast<uint32_t>(std::count(busy_.begin(), busy_.end(), true) * FramesPerBuffer * 1000 / SampleRate);
#else
        return device_ ? SDL_GetQueuedAudioSize(device_) * 1000 / (SampleRate * 4) : 0;
#endif
    }
private:
    bool active_ = false;
#ifdef __SWITCH__
    std::array<AudioOutBuffer, 4> buffers_{};
    std::array<bool, 4> busy_{};
#else
    SDL_AudioDeviceID device_ = 0;
#endif
};
}

struct Engine::Impl {
    explicit Impl(EngineConfig c) : config(std::move(c)), rng(std::random_device{}()) {
        snapshot.version = NCM_PROTOCOL_VERSION;
        snapshot.volume_permille = 700;
        snapshot.queue_index = UINT32_MAX;
        copyText(snapshot.message, "Ready");
    }
    EngineConfig config;
    mutable std::mutex mutex;
    std::condition_variable cv;
    NcmPlaybackStatus snapshot{};
    std::deque<std::pair<NcmCommand,uint32_t>> commands;
    std::vector<NcmQueueEntry> queue;
    std::thread worker, downloader;
    std::atomic<bool> running{false}, interrupt{false}, downloadCancel{false}, downloadDone{true};
    std::atomic<uint64_t> downloaded{0};
    std::atomic<long> downloadError{0};
    AudioSink sink;
    std::mt19937 rng;
    drmp3 decoder{};
    bool decoderOpen = false, paused = false, preview = false;
    FILE* input = nullptr;
    uint64_t cursor = 0, producedFrames = 0;
    uint32_t seekBase = 0, failures = 0;
    std::string currentPath;
    std::array<int16_t, 8192> decoded{};
    size_t decodedCount = 0, decodedPosition = 0;
    int16_t leftA = 0, rightA = 0, leftB = 0, rightB = 0;
    double phase = 0;
    bool havePair = false, reachedEnd = false;
    Json settings() const {
        FILE* input = std::fopen((config.directory+"/settings.json").c_str(),"rb");
        if (!input) return Json::object();
        std::array<char,16384> buffer{};
        const auto length = std::fread(buffer.data(),1,buffer.size()-1,input);
        std::fclose(input);
        auto values = Json::parse(buffer.data(),buffer.data()+length,nullptr,false);
        return values.is_object() ? values : Json::object();
    }
    void pruneCache() {
        struct Entry { std::filesystem::path path; std::filesystem::file_time_type time; uint64_t bytes; };
        std::vector<Entry> files;
        uint64_t total = 0;
        std::error_code error;
        auto directory = std::filesystem::directory_iterator(config.directory + "/cache",error);
        if (error) return;
        for (const auto& item : directory) {
            if (item.path().extension() != ".mp3" || item.path() == currentPath) continue;
            uint64_t size = item.file_size(error);
            if (error) { error.clear(); continue; }
            auto time = item.last_write_time(error);
            if (error) { error.clear(); continue; }
            total += size;
            files.push_back({item.path(),time,size});
        }
        std::sort(files.begin(),files.end(),[](const Entry& a,const Entry& b){return a.time < b.time;});
        for (const auto& item : files) {
            if (total <= 384u*1024u*1024u) break; // Leave room for the next bounded 128 MiB stream.
            if (std::filesystem::remove(item.path,error)) {
                total -= item.bytes;
                std::filesystem::remove(item.path.string()+".complete",error);
            }
        }
    }

    void state(uint32_t value, const char* message, uint32_t error = 0) {
        std::lock_guard<std::mutex> lock(mutex);
        snapshot.state = value; snapshot.error_code = error; copyText(snapshot.message, message);
    }
    void closeTrack() {
        downloadCancel = true;
        if (downloader.joinable()) downloader.join();
        if (decoderOpen) drmp3_uninit(&decoder);
        decoderOpen = false;
        if (input) std::fclose(input);
        input = nullptr;
        sink.flush();
        havePair = false; reachedEnd = false;
        decodedCount = decodedPosition = 0; producedFrames = 0; phase = 0;
        cursor = 0; seekBase = 0;
    }
    static size_t writeDownload(char* data, size_t size, size_t count, void* context) {
        auto* pair = static_cast<std::pair<Impl*,FILE*>*>(context);
        Impl* self = pair->first;
        size_t bytes = size * count;
        if (self->downloadCancel || self->interrupt || !self->running ||
            self->downloaded + bytes > MaxDownloadBytes) return 0;
        size_t written = std::fwrite(data, 1, bytes, pair->second);
        if (std::fflush(pair->second) != 0) return 0;
        self->downloaded.fetch_add(written);
        return written;
    }
    static int progress(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
        auto* self = static_cast<Impl*>(context);
        return self->downloadCancel || self->interrupt || !self->running;
    }
    void fetch(std::string url, std::string path) {
        FILE* output = std::fopen(path.c_str(), "wb");
        CURL* curl = curl_easy_init();
        if (!output || !curl) {
            if (output) std::fclose(output);
            if (curl) curl_easy_cleanup(curl);
            downloadError = CURLE_WRITE_ERROR; downloadDone = true; return;
        }
        std::pair<Impl*,FILE*> context{this,output};
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "NetEaseSwitch/0.1");
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeDownload);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
        if (!config.ca_bundle.empty()) curl_easy_setopt(curl, CURLOPT_CAINFO, config.ca_bundle.c_str());
        const auto preferences = settings();
        const auto proxy = preferences.contains("proxy") && preferences["proxy"].is_string()
            ? preferences["proxy"].get<std::string>() : std::string{};
        if (!proxy.empty()) curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str());
        CURLcode rc = curl_easy_perform(curl);
        std::fclose(output);
        curl_easy_cleanup(curl);
        downloadError = rc;
        downloadDone = true;
        if (rc == CURLE_OK) {
            // A marker is only published for a completely received response.
            FILE* marker = std::fopen((path + ".complete").c_str(), "wb");
            if (marker) std::fclose(marker);
        }
    }
    static size_t readStream(void* context, void* destination, size_t bytes) {
        auto* self = static_cast<Impl*>(context);
        bool buffering=false;
        while (self->running && !self->interrupt && !self->downloadDone &&
               self->downloaded < self->cursor + bytes) {
            if (self->decoderOpen && !self->paused && !buffering) {
                self->state(NcmPlayback_Buffering,"Buffering audio"); buffering=true;
            }
            std::this_thread::sleep_for(10ms);
        }
        if (!self->running || self->interrupt) return 0;
        if (buffering) self->state(self->paused?NcmPlayback_Paused:NcmPlayback_Playing,
            self->paused?"Paused":self->preview?"Playing preview / 试听":"Playing");
        uint64_t available = self->downloaded > self->cursor ? self->downloaded - self->cursor : 0;
        size_t count = static_cast<size_t>(std::min<uint64_t>(bytes, available));
        clearerr(self->input);
        size_t got = std::fread(destination, 1, count, self->input);
        self->cursor += got;
        return got;
    }
    static drmp3_bool32 seekStream(void* context, int offset, drmp3_seek_origin origin) {
        auto* self = static_cast<Impl*>(context);
        if (origin == DRMP3_SEEK_END && !self->downloadDone) return DRMP3_FALSE;
        int64_t base = origin == DRMP3_SEEK_SET ? 0 : origin == DRMP3_SEEK_CUR ?
            static_cast<int64_t>(self->cursor) : static_cast<int64_t>(self->downloaded.load());
        int64_t target = base + offset;
        if (target < 0 || static_cast<uint64_t>(target) > MaxDownloadBytes) return DRMP3_FALSE;
        while (self->running && !self->interrupt && !self->downloadDone &&
               self->downloaded < static_cast<uint64_t>(target)) std::this_thread::sleep_for(10ms);
        if (!self->running || self->interrupt || static_cast<uint64_t>(target) > self->downloaded)
            return DRMP3_FALSE;
        if (std::fseek(self->input, static_cast<long>(target), SEEK_SET) != 0) return DRMP3_FALSE;
        self->cursor = static_cast<uint64_t>(target); return DRMP3_TRUE;
    }
    static drmp3_bool32 tellStream(void* context, drmp3_int64* position) {
        *position = static_cast<Impl*>(context)->cursor; return DRMP3_TRUE;
    }
    std::string resolveUrl(const NcmQueueEntry& entry, bool force) {
        std::string url(entry.url);
        if (force || url.empty()) {
            ApiConfig c;
            c.cookie_file = config.directory + "/cookies.txt";
            c.ca_bundle = config.ca_bundle;
            c.timeout_seconds = 20;
            c.persist_cookies = false; // The GUI owns the durable authentication session.
            const auto preferences = settings();
            c.proxy = preferences.contains("proxy") && preferences["proxy"].is_string()
                ? preferences["proxy"].get<std::string>() : std::string{};
            auto quality = preferences.contains("quality") && preferences["quality"].is_string()
                ? preferences["quality"].get<std::string>() : std::string{"standard"};
            if (quality != "standard" && quality != "higher" && quality != "exhigh")
                quality = "standard"; // dr_mp3 accepts MP3; lossless modes are API-only.
            ApiClient client(c);
            auto response = client.song_urls({entry.id}, quality);
            if (response.contains("data") && response["data"].is_array()) {
                for (const auto& row : response["data"]) {
                    if (row.contains("url") && row["url"].is_string()) {
                        if (row.contains("type") && row["type"].is_string() &&
                            row["type"].get<std::string>() != "mp3")
                            throw std::runtime_error("Service returned unsupported audio format");
                        preview = row.contains("freeTrialInfo") && row["freeTrialInfo"].is_object();
                        if (row.contains("time") && row["time"].is_number_unsigned()) {
                            const uint64_t duration = row["time"].get<uint64_t>();
                            if (duration && duration < UINT32_MAX) {
                                std::lock_guard<std::mutex> lock(mutex);
                                snapshot.duration_ms = static_cast<uint32_t>(duration);
                            }
                        }
                        url = row["url"].get<std::string>(); break;
                    }
                }
            }
        }
        // NetEase CDN accepts HTTPS; never transfer cookies/media over cleartext.
        if (url.rfind("http://", 0) == 0) url.replace(0, 7, "https://");
        return url;
    }
    bool beginTrack(uint32_t index) {
        closeTrack();
        if (index >= queue.size()) return false;
        auto& entry = queue[index];
        paused = false; preview = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            snapshot.queue_index = index; snapshot.track_id = entry.id;
            snapshot.duration_ms = entry.duration_ms; snapshot.position_ms = 0;
            copyText(snapshot.title, entry.title); copyText(snapshot.artist, entry.artist);
        }
        state(NcmPlayback_Buffering, "Connecting");
        std::string url(entry.url);
        const std::string cachePath = config.directory + "/cache/" + std::to_string(entry.id) + ".mp3";
        // A completed local copy remains playable when the account API or
        // network is unavailable. No authentication request precedes this hit.
        bool cacheHit = std::filesystem::exists(cachePath+".complete");
        try {
            if (cacheHit) url=cachePath;
            else url = resolveUrl(entry, false);
        }
        catch (const std::exception& e) { state(NcmPlayback_Error, e.what(), 1); return false; }
        bool local = cacheHit || url.rfind("sdmc:/", 0) == 0 || url.rfind("/", 0) == 0;
        if (!local && url.rfind("https://", 0) != 0) {
            state(NcmPlayback_Error, "Track unavailable or requires membership", 2); return false;
        }
        currentPath = local ? url : cachePath;
        bool cached = local || std::filesystem::exists(currentPath + ".complete");
        downloadCancel = false; downloadError = 0; downloaded = 0; downloadDone = cached;
        if (cached) {
            std::error_code error;
            downloaded = std::filesystem::file_size(currentPath, error);
            if (error) { state(NcmPlayback_Error, "Cached audio is missing", 3); return false; }
        } else {
            pruneCache();
            // Truncate before publishing the path to the streaming reader.
            FILE* empty = std::fopen(currentPath.c_str(), "wb");
            if (!empty) { state(NcmPlayback_Error, "Cannot write SD audio cache", 3); return false; }
            std::fclose(empty);
            downloader = std::thread([this,url,path=currentPath] { fetch(url,path); });
        }
        input = std::fopen(currentPath.c_str(), "rb");
        if (!input) { state(NcmPlayback_Error, "Cannot open audio cache", 3); return false; }
        // Start after 32 KiB. The decoder blocks on the growing file only on
        // underrun, so RAM use is independent of the song's total length.
        while (running && !interrupt && !downloadDone && downloaded < 32768)
            std::this_thread::sleep_for(10ms);
        if (!running || interrupt) return false;
        if (downloadDone && downloadError && !cached) {
            // Signed CDN URLs expire. Resolve once with the shared cookie jar.
            if (downloader.joinable()) downloader.join();
            try {
                url = resolveUrl(entry, true);
                if (url.rfind("https://", 0) != 0) throw std::runtime_error("No playable URL");
                copyText(entry.url, url.c_str());
                std::fclose(input); input = nullptr;
                downloadDone = false; downloaded = 0; downloadError = 0;
                downloader = std::thread([this,url,path=currentPath] { fetch(url,path); });
                input = std::fopen(currentPath.c_str(), "rb");
                while (running && !interrupt && !downloadDone && downloaded < 32768)
                    std::this_thread::sleep_for(10ms);
            } catch (const std::exception& e) { state(NcmPlayback_Error, e.what(), 4); return false; }
        }
        if (!input || !drmp3_init(&decoder, readStream, seekStream, tellStream, nullptr, this, nullptr)) {
            if (cacheHit) std::remove((cachePath+".complete").c_str());
            state(NcmPlayback_Error, downloadError ? "Audio download failed" : "Unsupported or damaged MP3", 5);
            return false;
        }
        decoderOpen = true;
        if (decoder.channels < 1 || decoder.channels > 2 || !decoder.sampleRate || !sink.open()) {
            state(NcmPlayback_Error, "Audio output unavailable", 6); return false;
        }
        if (decoder.totalPCMFrameCount != DRMP3_UINT64_MAX && decoder.totalPCMFrameCount > decoder.delayInPCMFrames + decoder.paddingInPCMFrames) {
            const uint64_t audibleFrames=decoder.totalPCMFrameCount-decoder.delayInPCMFrames-decoder.paddingInPCMFrames;
            const uint64_t duration=audibleFrames*1000/decoder.sampleRate;
            if (duration && duration<UINT32_MAX) {
                std::lock_guard<std::mutex> lock(mutex);
                // MP3 Xing metadata reflects the actual entitled preview,
                // including when replayed offline from the SD cache.
                if (snapshot.duration_ms>duration+2000) preview=true;
                snapshot.duration_ms=duration;
            }
        }
        sink.pause(false);
        state(NcmPlayback_Playing, preview ? "Playing preview / 试听" : "Playing");
        return true;
    }
    bool readSample(int16_t& left, int16_t& right) {
        if (decodedPosition >= decodedCount) {
            decodedCount = static_cast<size_t>(drmp3_read_pcm_frames_s16(&decoder, 4096, decoded.data()));
            decodedPosition = 0;
            if (!decodedCount) return false;
        }
        left = decoded[decodedPosition * decoder.channels];
        right = decoder.channels == 2 ? decoded[decodedPosition * 2 + 1] : left;
        ++decodedPosition;
        return true;
    }
    size_t render(int16_t* out, size_t requested, uint32_t volume) {
        if (!havePair) {
            if (!readSample(leftA,rightA)) return 0;
            if (!readSample(leftB,rightB)) { leftB=leftA; rightB=rightA; reachedEnd=true; }
            havePair = true;
        }
        size_t produced = 0;
        const double ratio = static_cast<double>(decoder.sampleRate) / SampleRate;
        while (produced < requested) {
            out[produced*2] = static_cast<int16_t>((leftA + (leftB-leftA)*phase) * volume / 1000.0);
            out[produced*2+1] = static_cast<int16_t>((rightA + (rightB-rightA)*phase) * volume / 1000.0);
            ++produced; phase += ratio;
            while (phase >= 1.0) {
                phase -= 1.0;
                if (reachedEnd) { havePair = false; return produced; }
                leftA=leftB; rightA=rightB;
                if (!readSample(leftB,rightB)) reachedEnd = true;
            }
        }
        return produced;
    }
    bool loadQueue(uint32_t index) {
        FILE* file = std::fopen((config.directory + "/queue.nsq").c_str(),"rb");
        if (!file && errno == ENOENT)
            file = std::fopen((config.directory + "/queue.nsq.bak").c_str(),"rb");
        if (!file) {
            closeTrack(); queue.clear();
            { std::lock_guard<std::mutex> lock(mutex); snapshot.queue_count=0; }
            state(NcmPlayback_Error,"No queue; select a song in the app",7); return false;
        }
        NcmQueueHeader header{};
        bool valid = std::fread(&header,sizeof(header),1,file)==1 &&
            std::memcmp(header.magic,"NCMQUEUE",8)==0 && header.version==NCM_PROTOCOL_VERSION &&
            header.count>0 && header.count<=NCM_MAX_QUEUE && index<header.count;
        std::vector<NcmQueueEntry> loaded;
        if (valid) {
            loaded.resize(header.count);
            valid=std::fread(loaded.data(),sizeof(NcmQueueEntry),header.count,file)==header.count;
        }
        std::fclose(file);
        if (!valid) {
            closeTrack(); queue.clear();
            { std::lock_guard<std::mutex> lock(mutex); snapshot.queue_count=0; }
            state(NcmPlayback_Error,"Invalid playback queue",7); return false;
        }
        for (auto& row : loaded) {
            row.title[sizeof(row.title)-1]=0; row.artist[sizeof(row.artist)-1]=0;
            row.album[sizeof(row.album)-1]=0; row.url[sizeof(row.url)-1]=0;
        }
        queue=std::move(loaded); failures=0;
        { std::lock_guard<std::mutex> lock(mutex); snapshot.queue_count=queue.size(); }
        return beginTrack(index);
    }
    bool advance(bool manual, bool previous = false) {
        auto s = getStatus();
        if (queue.empty()) return false;
        uint32_t index = s.queue_index < queue.size() ? s.queue_index : 0;
        if (!manual && s.repeat == NcmRepeat_One) return beginTrack(index);
        if (s.shuffle && queue.size()>1) {
            std::uniform_int_distribution<uint32_t> pick(1, static_cast<uint32_t>(queue.size()-1));
            index=(index+pick(rng))%queue.size();
        } else if (previous) index = index ? index-1 : queue.size()-1;
        else {
            ++index;
            if (index >= queue.size()) {
                if (manual || s.repeat == NcmRepeat_All) index=0;
                else { closeTrack(); sink.close(); state(NcmPlayback_Stopped,"Queue finished"); return true; }
            }
        }
        return beginTrack(index);
    }
    NcmPlaybackStatus getStatus() const {
        std::lock_guard<std::mutex> lock(mutex); return snapshot;
    }
    void process(NcmCommand cmd, uint32_t argument) {
        switch (cmd) {
        case NcmCommand_LoadQueue: loadQueue(argument); break;
        case NcmCommand_Next: failures=0; advance(true); break;
        case NcmCommand_Previous:
            if (getStatus().position_ms > 3000) beginTrack(getStatus().queue_index);
            else { failures=0; advance(true,true); }
            break;
        case NcmCommand_Stop: closeTrack(); sink.close(); state(NcmPlayback_Stopped,"Stopped"); break;
        case NcmCommand_PlayPause:
        case NcmCommand_Play:
        case NcmCommand_Pause:
            if (!decoderOpen) {
                if (cmd != NcmCommand_Pause) {
                    failures=0;
                    if (queue.empty()) loadQueue(0);
                    else beginTrack(std::min<uint32_t>(getStatus().queue_index,queue.size()-1));
                }
                break;
            }
            paused=cmd==NcmCommand_Pause || (cmd==NcmCommand_PlayPause && !paused);
            sink.pause(paused);
            state(paused?NcmPlayback_Paused:NcmPlayback_Playing,
                paused?"Paused":preview?"Playing preview / 试听":"Playing");
            break;
        case NcmCommand_Volume: { std::lock_guard<std::mutex> lock(mutex); snapshot.volume_permille=argument; } break;
        case NcmCommand_Repeat: { std::lock_guard<std::mutex> lock(mutex); snapshot.repeat=argument; } break;
        case NcmCommand_Shuffle: { std::lock_guard<std::mutex> lock(mutex); snapshot.shuffle=argument; } break;
        case NcmCommand_Seek:
            if (decoderOpen) {
                sink.flush();
                uint32_t target=std::min(argument,getStatus().duration_ms);
                if (drmp3_seek_to_pcm_frame(&decoder,static_cast<uint64_t>(target)*decoder.sampleRate/1000)) {
                    decodedCount=decodedPosition=0; havePair=false; reachedEnd=false; phase=0;
                    seekBase=target; producedFrames=0;
                    std::lock_guard<std::mutex> lock(mutex); snapshot.position_ms=target;
                }
                sink.pause(paused);
            }
            break;
        default: break;
        }
    }
    void run() {
        std::array<int16_t, FramesPerBuffer*2> pcm{};
        while (running) {
            std::pair<NcmCommand,uint32_t> next{};
            bool hasCommand=false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!commands.empty()) {
                    next=commands.front(); commands.pop_front(); hasCommand=true;
                    interrupt=false;
                }
            }
            if (hasCommand) { process(next.first,next.second); continue; }
            if (interrupt) { std::this_thread::sleep_for(2ms); continue; }
            if (decoderOpen && !paused) {
                if (!sink.canWrite()) { std::this_thread::sleep_for(4ms); continue; }
                size_t frames=render(pcm.data(),FramesPerBuffer,getStatus().volume_permille);
                if (interrupt || !running) continue;
                if (!frames) {
                    if (downloadError) state(NcmPlayback_Error,"Network interrupted; track skipped",static_cast<uint32_t>(downloadError.load()));
                    // Drain the last queued PCM before changing tracks.
                    if (sink.queuedMs()) { std::this_thread::sleep_for(5ms); continue; }
                    failures=0;
                    advance(false);
                } else if (!sink.write(pcm.data(),frames)) {
                    closeTrack(); state(NcmPlayback_Error,"Audio output failed",6);
                } else {
                    producedFrames+=frames;
                    uint64_t milliseconds=producedFrames*1000/SampleRate;
                    std::lock_guard<std::mutex> lock(mutex);
                    snapshot.position_ms=seekBase+static_cast<uint32_t>(milliseconds>sink.queuedMs()?milliseconds-sink.queuedMs():0);
                }
            } else if (getStatus().state == NcmPlayback_Error && !queue.empty() && ++failures < queue.size()) {
                // Bounded skipping prevents an unavailable album spinning forever.
                closeTrack();
                std::this_thread::sleep_for(250ms);
                advance(false);
            } else {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait_for(lock,50ms,[this]{return !commands.empty() || !running;});
            }
        }
        closeTrack(); sink.close();
    }
};

Engine::Engine(EngineConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
Engine::~Engine() { stop(); }
bool Engine::start() {
    if (impl_->running.exchange(true)) return true;
    std::error_code error;
    std::filesystem::create_directories(impl_->config.directory + "/cache",error);
    if (error) { impl_->running=false; return false; }
    impl_->worker=std::thread([this]{impl_->run();});
    return true;
}
void Engine::stop() {
    impl_->running=false; impl_->interrupt=true; impl_->downloadCancel=true; impl_->cv.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
}
bool Engine::command(NcmCommand cmd,uint32_t argument) {
    if (cmd < NcmCommand_LoadQueue || cmd > NcmCommand_Stop ||
        (cmd==NcmCommand_Volume && argument>1000) ||
        (cmd==NcmCommand_Repeat && argument>NcmRepeat_All) ||
        (cmd==NcmCommand_Shuffle && argument>1) ||
        (cmd==NcmCommand_LoadQueue && argument>=NCM_MAX_QUEUE)) return false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->commands.size()>=32) return false;
        impl_->commands.emplace_back(cmd,argument);
        if (cmd==NcmCommand_LoadQueue || cmd==NcmCommand_Next || cmd==NcmCommand_Previous || cmd==NcmCommand_Stop)
            impl_->interrupt=true;
    }
    impl_->cv.notify_one(); return true;
}
NcmPlaybackStatus Engine::status() const { return impl_->getStatus(); }
}
