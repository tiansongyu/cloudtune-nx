/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "engine.hpp"
#include "audio_decoder.hpp"
#include "pcm_stream.hpp"
#include "netease/api.hpp"
#include "netease/accounts.hpp"
#include "netease/crypto.hpp"
#include "netease/storage.hpp"
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
#include <filesystem>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

namespace netease::playback {
namespace {
constexpr size_t FramesPerBuffer = 2048;
constexpr uint32_t SampleRate = 48000;
constexpr uint64_t MaxDownloadBytes = 512u * 1024u * 1024u;
constexpr uint64_t RetainedCacheBytes = 512u * 1024u * 1024u;
constexpr size_t CommandCapacity = 32;
// Internal events are outside the public command enum's representable range.
constexpr uint32_t ExternalBegin = 100;
constexpr uint32_t ExternalEnd = 101;
constexpr uint32_t ExternalFlush = 102;
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
        error_ = 0;
#ifdef __SWITCH__
        if (R_FAILED(error_ = audoutInitialize())) return false;
        active_ = true;
        for (auto& buffer : buffers_) {
            buffer.buffer = memalign(0x1000, 0x2000);
            if (!buffer.buffer) { error_=UINT32_MAX; close(); return false; }
            buffer.buffer_size = 0x2000;
            buffer.data_size = 0;
        }
        active_ = true;
        if (R_FAILED(error_ = audoutStartAudioOut())) { close(); return false; }
#else
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return false;
        SDL_AudioSpec wanted{};
        wanted.freq = SampleRate;
        wanted.format = AUDIO_S16SYS;
        wanted.channels = 2;
        wanted.samples = FramesPerBuffer;
        device_ = SDL_OpenAudioDevice(nullptr, 0, &wanted, nullptr, 0);
        if (!device_) { SDL_QuitSubSystem(SDL_INIT_AUDIO); return false; }
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
        if (device_) { SDL_CloseAudioDevice(device_); SDL_QuitSubSystem(SDL_INIT_AUDIO); }
        device_ = 0;
#endif
        active_ = false;
    }
    bool canWrite() {
        if (!active_) return false;
#ifdef __SWITCH__
        AudioOutBuffer* released = nullptr;
        u32 count = 0;
        if (R_FAILED(error_ = audoutGetReleasedAudioOutBuffer(&released, &count))) return false;
        while (released) {
            for (size_t i=0; i<buffers_.size(); ++i) if (&buffers_[i] == released) busy_[i] = false;
            released = released->next;
        }
        return std::find(busy_.begin(), busy_.end(), false) != busy_.end();
#else
        if (SDL_GetAudioDeviceStatus(device_) != SDL_AUDIO_PLAYING) {
            error_ = 1;
            return false;
        }
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
            if (R_FAILED(error_ = audoutAppendAudioOutBuffer(&buffer))) return false;
            busy_[i] = true;
            return true;
        }
        return false;
#else
        error_ = SDL_QueueAudio(device_, data, static_cast<uint32_t>(frames * 4)) == 0 ? 0 : 1;
        return !error_;
#endif
    }
    uint32_t queuedMs() const {
#ifdef __SWITCH__
        uint64_t frames = 0;
        for (size_t i = 0; i < buffers_.size(); ++i)
            if (busy_[i]) frames += buffers_[i].data_size / 4;
        return static_cast<uint32_t>(frames * 1000 / SampleRate);
#else
        return device_ ? SDL_GetQueuedAudioSize(device_) * 1000 / (SampleRate * 4) : 0;
#endif
    }
    uint32_t error() const { return error_; }
private:
    bool active_ = false;
    uint32_t error_ = 0;
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
        // One reserved lifecycle slot ensures a sender can always end its
        // session. No command producer allocates after construction.
        commands.reserve(CommandCapacity + 1);
        snapshot.version = NCM_PROTOCOL_VERSION;
        snapshot.volume_permille = 700;
        snapshot.queue_index = UINT32_MAX;
        copyText(snapshot.message, "Ready");
    }
    EngineConfig config;
    mutable std::mutex mutex;
    std::condition_variable cv;
    NcmPlaybackStatus snapshot{};
    struct PendingCommand {
        uint32_t first = 0;
        uint32_t second = 0;
        uint64_t ticket = 0;
        PendingCommand() = default;
        PendingCommand(uint32_t kind, uint32_t argument, uint64_t id = 0)
            : first(kind), second(argument), ticket(id) {}
    };
    std::vector<PendingCommand> commands;
    uint64_t clearRequested = 0, clearCompleted = 0;
    std::vector<NcmQueueEntry> queue;
    std::thread worker, downloader;
    std::atomic<bool> running{false}, interrupt{false}, downloadCancel{false}, downloadDone{true};
    std::atomic<uint64_t> downloaded{0};
    std::atomic<long> downloadError{0};
    AudioSink sink;
    std::mt19937 rng;
    AudioDecoder decoder;
    bool decoderOpen = false, paused = false, preview = false;
    // Horizon SD files must not be reopened by the streaming reader while a
    // writer owns them. One update stream has two logical offsets, serialized
    // here; the worker closes it only after joining the downloader.
    std::mutex cacheMutex;
    FILE* input = nullptr;
    std::atomic<int> cacheError{0};
    const char *cacheOperation = "open";
    uint32_t cacheResult = 0;
    bool managedCache = false;
    uint64_t cursor = 0, producedFrames = 0;
    uint32_t seekBase = 0, failures = 0;
    std::string currentPath;
    std::string activeQuality = "standard";
    std::string activeSession; // Captured with the loaded queue, never read from a staged mirror.
    bool diagnostics = false;
    std::chrono::steady_clock::time_point nextDiagnostic{};
    std::array<int16_t, 8192> decoded{};
    size_t decodedCount = 0, decodedPosition = 0;
    int16_t leftA = 0, rightA = 0, leftB = 0, rightB = 0;
    double phase = 0;
    bool havePair = false, reachedEnd = false;
    PcmStream external;
    std::atomic<uint32_t> externalGain{1000};
    bool externalMode = false, externalPaused = false, resumeMusic = false;
    NcmPlaybackStatus suspendedMusic{};
    uint64_t externalFrames = 0;
    bool recoveringOutput = false;
    unsigned outputAttempts = 0;
    uint32_t recoveryPosition = 0;
    std::chrono::steady_clock::time_point nextOutputRetry{}, healthyOutputSince{};
    Json settings() const {
        try {
            const auto text = storage::read_file(config.directory+"/settings.json");
            auto values = Json::parse(text,nullptr,false);
            return values.is_object() ? values : Json::object();
        } catch (const std::exception&) { return Json::object(); }
    }
    std::string cacheIdentity() const {
        // A preview cached anonymously must not mask the newly authenticated
        // entitlement. Only a digest, never an authentication token, reaches
        // filenames. Guest-cookie rotation does not invalidate offline music.
        auto token=session_auth_cookie(activeSession,"MUSIC_U");
        if (token.empty()) token="anonymous";
        return crypto::md5(token + "\n" + activeQuality);
    }
    void diagnostic(const char* event, const int16_t* pcm = nullptr, size_t frames = 0) {
        if (!diagnostics) return;
        const auto now = std::chrono::steady_clock::now();
        if (!pcm && now < nextDiagnostic) return;
        nextDiagnostic = now + 3s;
        const auto path=config.directory+"/audio.log";
        std::error_code ignored;
        const auto size=std::filesystem::file_size(path,ignored);
        FILE* file = std::fopen(path.c_str(),!ignored && size>256*1024 ? "wb" : "ab");
        if (!file) return;
        int lo=0, hi=0;
        if (frames) {
            auto range=std::minmax_element(pcm,pcm+frames*2); lo=*range.first; hi=*range.second;
        }
        const auto s=getStatus();
        std::fprintf(file,"%s id=%llu state=%u codec=%s rate=%u produced=%llu queued_ms=%u pos=%u input=%llu done=%d curl=%ld audout=%08x pcm=%d..%d\n",
            event,static_cast<unsigned long long>(s.track_id),s.state,decoder.format(),decoder.sample_rate(),
            static_cast<unsigned long long>(producedFrames),sink.queuedMs(),s.position_ms,
            static_cast<unsigned long long>(downloaded.load()),downloadDone.load(),
            downloadError.load(),sink.error(),lo,hi);
        std::fclose(file);
    }
    void pruneCache() {
        struct Entry { std::filesystem::path path; std::filesystem::file_time_type time; uint64_t bytes; };
        constexpr size_t Batch = 256;
        std::vector<Entry> files;
        files.reserve(Batch);
        const auto older = [](const Entry& a, const Entry& b) { return a.time < b.time; };
        // Keep only the oldest bounded batch, even on an SD card containing
        // thousands of tiny tracks. Rescan only when another eviction is needed.
        while (running && !interrupt) {
            files.clear();
            uint64_t total = 0;
            std::error_code error;
            auto directory = std::filesystem::directory_iterator(config.directory + "/cache",error);
            if (error) return;
            for (const auto& item : directory) {
                const auto extension = item.path().extension();
                if ((extension != ".mp3" && extension != ".audio") || item.path() == currentPath) continue;
                uint64_t size = item.file_size(error);
                if (error) { error.clear(); continue; }
                auto time = item.last_write_time(error);
                if (error) { error.clear(); continue; }
                total += size;
                if (!running || interrupt) return;
                if (files.size() < Batch) {
                    files.push_back({item.path(),time,size});
                    std::push_heap(files.begin(),files.end(),older);
                } else if (time < files.front().time) {
                    std::pop_heap(files.begin(),files.end(),older);
                    files.back() = {item.path(),time,size};
                    std::push_heap(files.begin(),files.end(),older);
                }
            }
            if (total <= RetainedCacheBytes) return;
            std::sort_heap(files.begin(),files.end(),older);
            bool removed = false;
            for (const auto& item : files) {
                if (total <= RetainedCacheBytes) break; // Plus at most one 512 MiB downloading track.
                if (std::filesystem::remove(item.path,error)) {
                    removed = true;
                    total -= item.bytes;
                    std::filesystem::remove(item.path.string()+".complete",error);
                }
            }
            if (!removed) return;
        }
    }

    void state(uint32_t value, const char* message, uint32_t error = 0) {
        std::lock_guard<std::mutex> lock(mutex);
        snapshot.state = value; snapshot.error_code = error; copyText(snapshot.message, message);
    }
    // Called under cacheMutex, or before starting the downloader. Capture the
    // native FS Result immediately: libnx maps most SD errors to plain EIO.
    void cacheFailure(const char *operation) {
        if (cacheError)
            return;
        const int error = errno ? errno : EIO;
        cacheOperation = operation;
#ifdef __SWITCH__
        cacheResult = error == EIO ? fsdevGetLastResult() : 0;
#else
        cacheResult = 0;
#endif
        cacheError = error;
    }
    void invalidateCache() {
        // The downloader must have stopped before invalidating its marker.
        if (managedCache)
            std::remove((currentPath + ".complete").c_str());
    }
    void reportCacheFailure() {
        closeTrack();
        invalidateCache();
        char message[96];
        std::snprintf(message, sizeof(message), "SD cache %s: errno=%d fs=%08X", cacheOperation,
                      cacheError.load(), cacheResult);
        // This bounded error diagnostic contains no account or URL.
        // If the SD card is unwritable, the status still carries the details.
        const auto path = config.directory + "/cache-errors.log";
        std::error_code ignored;
        const auto size = std::filesystem::file_size(path, ignored);
        if (FILE *log = std::fopen(path.c_str(), !ignored && size > 64 * 1024 ? "wb" : "ab")) {
            std::fprintf(log, "%s\n", message);
            std::fclose(log);
        }
        state(NcmPlayback_Error, message, 3);
    }
    void closeTrack() {
        recoveringOutput = false; outputAttempts = 0; healthyOutputSince = {};
        downloadCancel = true;
        if (downloader.joinable()) downloader.join();
        decoder.close();
        decoderOpen = false;
        if (input) std::fclose(input);
        input = nullptr;
        sink.close();
        havePair = false; reachedEnd = false;
        decodedCount = decodedPosition = 0; producedFrames = 0; phase = 0;
        cursor = 0; seekBase = 0;
    }
    static size_t writeDownload(char* data, size_t size, size_t count, void* context) {
        auto *self = static_cast<Impl *>(context);
        if (size && count > MaxDownloadBytes / size)
            return 0;
        size_t bytes = size * count;
        if (self->downloadCancel || self->interrupt || !self->running || self->cacheError ||
            self->downloaded + bytes > MaxDownloadBytes)
            return 0;
        std::lock_guard<std::mutex> lock(self->cacheMutex);
        if (self->cacheError)
            return 0;
        // fseek is also the required read->write transition for a C update
        // stream. Neither thread relies on the shared FILE position.
        errno = 0;
        if (std::fseek(self->input, static_cast<long>(self->downloaded.load()), SEEK_SET) != 0) {
            self->cacheFailure("seek");
            return 0;
        }
        size_t written = std::fwrite(data, 1, bytes, self->input);
        if (written != bytes) {
            self->cacheFailure("write");
            return 0;
        }
        if (std::fflush(self->input) != 0) {
            self->cacheFailure("flush");
            return 0;
        }
        // Never expose bytes to a decoder until the flush succeeded.
        self->downloaded.fetch_add(written);
        return written;
    }
    static int progress(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
        auto* self = static_cast<Impl*>(context);
        return self->downloadCancel || self->interrupt || !self->running || self->cacheError;
    }
    void fetch(std::string url, std::string path) {
        CURL* curl = curl_easy_init();
        if (!curl) {
            downloadError = CURLE_OUT_OF_MEMORY;
            downloadDone = true;
            return;
        }
        std::unique_ptr<CURL,decltype(&curl_easy_cleanup)> curlOwner(curl,curl_easy_cleanup);
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
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "CloudTuneNX/0.1");
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeDownload);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
        if (!config.ca_bundle.empty()) curl_easy_setopt(curl, CURLOPT_CAINFO, config.ca_bundle.c_str());
        const auto preferences = settings();
        const auto proxy = preferences.contains("proxy") && preferences["proxy"].is_string()
            ? preferences["proxy"].get<std::string>() : std::string{};
        if (!proxy.empty()) curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str());
        CURLcode rc = curl_easy_perform(curl);
        if (rc==CURLE_OK) {
            std::lock_guard<std::mutex> lock(cacheMutex);
            errno = 0;
            if (std::fflush(input) != 0)
                cacheFailure("flush");
            else if (::fsync(::fileno(input)) != 0 && errno != EINVAL && errno != ENOSYS)
                cacheFailure("sync");
            if (cacheError)
                rc = CURLE_WRITE_ERROR;
        }
        if (rc == CURLE_OK && !cacheError && !downloadCancel && !interrupt && running) {
            // Publish before downloadDone, so decoder rejection cannot race
            // with a late marker. Failure to cache the marker is nonfatal: the
            // complete audio still plays, and will be downloaded next time.
            FILE* marker = std::fopen((path + ".complete").c_str(), "wb");
            if (marker) std::fclose(marker);
        }
        downloadError = rc;
        downloadDone = true;
    }
    void startFetch(const std::string& url) {
        downloader=std::thread([this,url,path=currentPath] {
            try { fetch(url,path); }
            catch (...) { downloadError=CURLE_OUT_OF_MEMORY; downloadDone=true; }
        });
    }
    static size_t readStream(void* context, void* destination, size_t bytes) {
        auto* self = static_cast<Impl*>(context);
        bool buffering=false;
        while (self->running && !self->interrupt && !self->downloadDone && !self->cacheError &&
               self->downloaded < self->cursor + bytes) {
            if (self->decoderOpen && !self->paused && !buffering) {
                self->state(NcmPlayback_Buffering,"Buffering audio"); buffering=true;
            }
            std::this_thread::sleep_for(10ms);
        }
        if (!self->running || self->interrupt || self->cacheError)
            return 0;
        if (buffering) self->state(self->paused?NcmPlayback_Paused:NcmPlayback_Playing,
            self->paused?"Paused":self->preview?"Playing preview / 试听":"Playing");
        uint64_t available = self->downloaded > self->cursor ? self->downloaded - self->cursor : 0;
        size_t count = static_cast<size_t>(std::min<uint64_t>(bytes, available));
        if (!count)
            return 0;
        std::lock_guard<std::mutex> lock(self->cacheMutex);
        if (self->cacheError)
            return 0;
        errno = 0;
        clearerr(self->input);
        if (std::fseek(self->input, static_cast<long>(self->cursor), SEEK_SET) != 0) {
            self->cacheFailure("seek");
            return 0;
        }
        size_t got = std::fread(destination, 1, count, self->input);
        // EOF is only valid at the published byte count, not within it.
        if (got != count) {
            self->cacheFailure("read");
            return 0;
        }
        self->cursor += got;
        return got;
    }
    static bool seekStream(void* context, int64_t offset, AudioDecoder::Origin origin) {
        auto* self = static_cast<Impl*>(context);
        if (origin == AudioDecoder::Origin::End && !self->downloadDone) return false;
        int64_t base = origin == AudioDecoder::Origin::Start ? 0 : origin == AudioDecoder::Origin::Current ?
            static_cast<int64_t>(self->cursor) : static_cast<int64_t>(self->downloaded.load());
        // Check untrusted container offsets before addition: base+offset can
        // overflow int64 even though the resulting file position is rejected.
        if (base<0 || static_cast<uint64_t>(base)>MaxDownloadBytes || offset< -base ||
            offset>static_cast<int64_t>(MaxDownloadBytes)-base) return false;
        int64_t target = base + offset;
        while (self->running && !self->interrupt && !self->downloadDone && !self->cacheError &&
               self->downloaded < static_cast<uint64_t>(target))
            std::this_thread::sleep_for(10ms);
        if (!self->running || self->interrupt || self->cacheError ||
            static_cast<uint64_t>(target) > self->downloaded)
            return false;
        // readStream seeks to this logical cursor while holding cacheMutex.
        self->cursor = static_cast<uint64_t>(target); return true;
    }
    static bool tellStream(void* context, int64_t* position) {
        *position = static_cast<Impl*>(context)->cursor; return true;
    }
    std::string resolveUrl(const NcmQueueEntry& entry, bool force) {
        std::string url(entry.url);
        if (force || url.empty()) {
            ApiConfig c;
            c.ca_bundle = config.ca_bundle;
            c.timeout_seconds = 20;
            c.persist_cookies = false; // The GUI owns the durable authentication session.
            const auto preferences = settings();
            c.proxy = preferences.contains("proxy") && preferences["proxy"].is_string()
                ? preferences["proxy"].get<std::string>() : std::string{};
            ApiClient client(c);
            client.adopt_session(activeSession);
            auto response = client.song_urls({entry.id}, activeQuality);
            if (response.contains("data") && response["data"].is_array()) {
                for (const auto& row : response["data"]) {
                    if (row.contains("url") && row["url"].is_string()) {
                        // The decoder identifies the actual response bytes.
                        // NetEase can return MP3 or FLAC for the same quality
                        // depending on the recording and account entitlement.
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
    bool beginTrack(uint32_t index, bool retryInvalidCache = true) {
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
        const auto preferences = settings();
        activeQuality = preferences.contains("quality") && preferences["quality"].is_string()
            ? preferences["quality"].get<std::string>() : std::string{"standard"};
        if (activeQuality == "extreme") activeQuality = "exhigh";
        if (activeQuality != "standard" && activeQuality != "higher" && activeQuality != "exhigh" &&
            activeQuality != "lossless" && activeQuality != "hires" && activeQuality != "jyeffect" &&
            activeQuality != "sky" && activeQuality != "jymaster") activeQuality = "standard";
        const bool explicitLocal = url.rfind("sdmc:/",0)==0 || url.rfind("/",0)==0;
        const std::string cacheBase = config.directory + "/cache/" + std::to_string(entry.id) + "-" + cacheIdentity();
        std::string cachePath = cacheBase + ".audio";
        if (!std::filesystem::exists(cachePath+".complete") &&
            std::filesystem::is_regular_file(cacheBase+".mp3") &&
            std::filesystem::exists(cacheBase+".mp3.complete")) cachePath=cacheBase+".mp3";
        // A completed local copy remains playable when the account API or
        // network is unavailable. No authentication request precedes this hit.
        bool cacheHit = !explicitLocal && std::filesystem::exists(cachePath+".complete") &&
            std::filesystem::is_regular_file(cachePath);
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
        managedCache = !explicitLocal;
        cacheError = 0;
        cacheResult = 0;
        // cacheHit above requires both the marker and the data file. A marker
        // alone may survive SD/file loss and must not prevent a new download.
        const bool cached = local;
        downloadCancel = false; downloadError = 0; downloaded = 0; downloadDone = cached;
        if (cached) {
            std::error_code error;
            downloaded = std::filesystem::file_size(currentPath, error);
            if (error || downloaded > MaxDownloadBytes) {
                state(NcmPlayback_Error, error ? "Cached audio is missing" : "Audio exceeds 512 MiB limit", 3);
                return false;
            }
        } else {
            pruneCache();
            // An orphan marker must never bless a newly truncated partial
            // response after SD removal, a failed write, or a previous crash.
            if (std::remove((currentPath+".complete").c_str())!=0 && errno!=ENOENT) {
                state(NcmPlayback_Error,"Cannot replace SD audio cache marker",3); return false;
            }
        }
        input = std::fopen(currentPath.c_str(), cached ? "rb" : "w+b");
        if (!input) {
            cacheFailure("open");
            reportCacheFailure();
            return false;
        }
        if (!cached)
            startFetch(url);
        // Start after 32 KiB. The decoder blocks on the growing file only on
        // underrun, so RAM use is independent of the song's total length.
        while (running && !interrupt && !cacheError && !downloadDone && downloaded < 32768)
            std::this_thread::sleep_for(10ms);
        if (!running || interrupt) return false;
        if (cacheError) {
            reportCacheFailure();
            return false;
        }
        if (downloadDone && downloadError && !cached && downloadError != CURLE_WRITE_ERROR &&
            downloadError != CURLE_OUT_OF_MEMORY) {
            // Signed CDN URLs expire. Resolve once using this queue's session.
            if (downloader.joinable()) downloader.join();
            try {
                url = resolveUrl(entry, true);
                if (url.rfind("https://", 0) != 0) throw std::runtime_error("No playable URL");
                copyText(entry.url, url.c_str());
                std::fclose(input); input = nullptr;
                downloadDone = false; downloaded = 0; downloadError = 0;
                input = std::fopen(currentPath.c_str(), "w+b");
                if (!input) {
                    cacheFailure("open");
                    reportCacheFailure();
                    return false;
                }
                startFetch(url);
                while (running && !interrupt && !cacheError && !downloadDone && downloaded < 32768)
                    std::this_thread::sleep_for(10ms);
            } catch (const std::exception& e) { state(NcmPlayback_Error, e.what(), 4); return false; }
        }
        if (!running || interrupt)
            return false;
        if (cacheError) {
            reportCacheFailure();
            return false;
        }
        const bool opened = decoder.open(readStream, seekStream, tellStream, this);
        if (cacheError) {
            reportCacheFailure();
            return false;
        }
        if (!opened) {
            const bool limited = decoder.resource_limited();
            closeTrack();
            if (!limited)
                invalidateCache();
            // Older versions could leave a complete marker on unreadable
            // media. Recover once without making the user select it twice.
            if (cacheHit && !limited && retryInvalidCache && running && !interrupt)
                return beginTrack(index, false);
            state(NcmPlayback_Error, limited ? "Audio exceeds decoder memory budget" :
                  downloadError ? "Audio download failed" : "Unsupported or damaged MP3/FLAC/AAC",
                  limited ? 10 : 5);
            return false;
        }
        decoderOpen = true;
        const bool outputReady = sink.open();
        if (decoder.frames()) {
            const uint64_t duration=decoder.frames()*1000/decoder.sample_rate();
            if (duration && duration<UINT32_MAX) {
                std::lock_guard<std::mutex> lock(mutex);
                // Codec metadata reflects the actual entitled preview,
                // including when replayed offline from the SD cache.
                if (snapshot.duration_ms>duration+2000) preview=true;
                snapshot.duration_ms=duration;
            }
        }
        if (!outputReady) { recoverOutput(); return true; }
        state(NcmPlayback_Playing, preview ? "Playing preview / 试听" : "Playing");
        nextDiagnostic = {};
        diagnostic("start");
        return true;
    }
    bool readSample(int16_t& left, int16_t& right) {
        if (decodedPosition >= decodedCount) {
            decodedCount = decoder.read(decoded.data(), 4096);
            decodedPosition = 0;
            if (!decodedCount) return false;
        }
        left = decoded[decodedPosition * decoder.channels()];
        right = decoder.channels() == 2 ? decoded[decodedPosition * 2 + 1] : left;
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
        const double ratio = static_cast<double>(decoder.sample_rate()) / SampleRate;
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
        std::string session;
        try {
            // Cold Play/PlayPause from the overlay also reaches this path.
            // Never reactivate a saved queue through an unreadable registry's
            // cookie mirror. Already-loaded music and AirPlay are unaffected
            // until the user explicitly requests a new queue.
            ApiConfig sessionConfig;
            sessionConfig.persist_cookies=false;
            ApiClient canonical(sessionConfig);
            canonical.adopt_session(AccountStore::playback_cookies(this->config.directory));
            // libcurl normalizes legacy cookie syntax on import. Hash exactly
            // the same normalized jar that later API requests will consume.
            session=canonical.session_cookies();
        } catch (const std::exception&) {
            closeTrack(); queue.clear();
            { std::lock_guard<std::mutex> lock(mutex); snapshot.queue_count=0; }
            state(NcmPlayback_Error,"Saved accounts unavailable; open app to recover",7);
            return false;
        }
        FILE* file = std::fopen((config.directory + "/queue.nsq").c_str(),"rb");
        if (!file && errno == ENOENT)
            file = std::fopen((config.directory + "/queue.nsq.bak").c_str(),"rb");
        if (!file) {
            closeTrack(); queue.clear();
            { std::lock_guard<std::mutex> lock(mutex); snapshot.queue_count=0; }
            state(NcmPlayback_Error,"No queue; select a song in the app",7); return false;
        }
        std::unique_ptr<FILE,decltype(&std::fclose)> queueFile(file,std::fclose);
        NcmQueueHeader header{};
        bool valid = std::fread(&header,sizeof(header),1,file)==1 &&
            std::memcmp(header.magic,"NCMQUEUE",8)==0 && header.version==NCM_PROTOCOL_VERSION &&
            header.count<=NCM_MAX_QUEUE && (!header.count || index<header.count);
        std::vector<NcmQueueEntry> loaded;
        if (valid) {
            loaded.resize(header.count);
            if (header.count)
                valid=std::fread(loaded.data(),sizeof(NcmQueueEntry),header.count,file)==header.count;
        }
        queueFile.reset();
        if (!valid) {
            closeTrack(); queue.clear();
            { std::lock_guard<std::mutex> lock(mutex); snapshot.queue_count=0; }
            state(NcmPlayback_Error,"Invalid playback queue",7); return false;
        }
        for (auto& row : loaded) {
            row.title[sizeof(row.title)-1]=0; row.artist[sizeof(row.artist)-1]=0;
            row.album[sizeof(row.album)-1]=0; row.url[sizeof(row.url)-1]=0;
        }
        queue=std::move(loaded); activeSession=std::move(session); failures=0;
        { std::lock_guard<std::mutex> lock(mutex); snapshot.queue_count=queue.size(); }
        if (queue.empty()) {
            closeTrack();
            std::lock_guard<std::mutex> lock(mutex);
            snapshot.queue_index=UINT32_MAX; snapshot.track_id=0;
            snapshot.position_ms=0; snapshot.duration_ms=0; snapshot.error_code=0;
            snapshot.title[0]=0; snapshot.artist[0]=0;
            snapshot.state=NcmPlayback_Stopped;
            copyText(snapshot.message,"Select a song in the app");
            return true;
        }
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
    bool seekDecoder(uint32_t target) {
        if (!decoder.seek(static_cast<uint64_t>(target)*decoder.sample_rate()/1000))
            return false;
        decodedCount=decodedPosition=0; havePair=false; reachedEnd=false; phase=0;
        seekBase=target; producedFrames=0;
        if (recoveringOutput) recoveryPosition=target;
        std::lock_guard<std::mutex> lock(mutex); snapshot.position_ms=target;
        return true;
    }
    void pauseFailedOutput() {
        recoveringOutput = false;
        sink.close();
        if (externalMode) { externalPaused = true; external.pause(true); }
        else paused = true;
        state(NcmPlayback_Paused, "音频设备不可用，按播放重试", 6);
    }
    void recoverOutput() {
        recoveryPosition = getStatus().position_ms;
        sink.close();
        healthyOutputSince = {};
        if (externalMode) external.pause(true); // Discard old live audio during recovery.
        if (outputAttempts >= 3) { pauseFailedOutput(); return; }
        recoveringOutput = true;
        nextOutputRetry = std::chrono::steady_clock::now() + 250ms;
        state(NcmPlayback_Buffering, "正在恢复音频输出");
    }
    void retryOutput() {
        if (std::chrono::steady_clock::now() < nextOutputRetry) return;
        ++outputAttempts;
        if (!sink.open()) {
            if (outputAttempts >= 3) pauseFailedOutput();
            else nextOutputRetry = std::chrono::steady_clock::now() + 500ms;
            return;
        }
        if (externalMode) {
            external.pause(false);
            externalFrames = static_cast<uint64_t>(recoveryPosition) * SampleRate / 1000;
        } else if (!seekDecoder(recoveryPosition)) {
            closeTrack();
            resumeMusic = !queue.empty();
            state(NcmPlayback_Paused, "无法恢复播放位置，按播放重试", 6);
            return;
        }
        recoveringOutput = false;
        healthyOutputSince = std::chrono::steady_clock::now();
        state(NcmPlayback_Playing, externalMode ? "AirPlay receiving" :
              preview ? "Playing preview / 试听" : "Playing");
    }
    void outputProgress() {
        if (healthyOutputSince != std::chrono::steady_clock::time_point{} &&
            std::chrono::steady_clock::now() - healthyOutputSince >= 2s)
            outputAttempts = 0;
    }
    void endExternal() {
        if (!externalMode) return;
        recoveringOutput = false; outputAttempts = 0;
        sink.close(); externalMode = false; externalPaused = false;
        std::lock_guard<std::mutex> lock(mutex);
        const uint32_t volume = snapshot.volume_permille;
        const uint32_t repeat = snapshot.repeat, shuffle = snapshot.shuffle;
        snapshot = suspendedMusic;
        snapshot.volume_permille = volume; snapshot.repeat = repeat; snapshot.shuffle = shuffle;
        snapshot.state = queue.empty() ? NcmPlayback_Stopped : NcmPlayback_Paused;
        snapshot.error_code = 0;
        copyText(snapshot.message, "AirPlay ended; music paused");
        resumeMusic = !queue.empty();
    }
    bool processExternal(uint32_t cmd, uint32_t argument) {
        if (cmd == ExternalBegin) {
            if (!externalMode) suspendedMusic = getStatus();
            closeTrack(); externalMode = true; externalPaused = false;
            externalFrames = 0; resumeMusic = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                snapshot.track_id = 0; snapshot.position_ms = snapshot.duration_ms = 0;
                copyText(snapshot.title, "AirPlay 1");
                copyText(snapshot.artist, "CloudTune NX");
            }
            if (sink.open()) state(NcmPlayback_Playing, "AirPlay receiving");
            else recoverOutput();
            return true;
        }
        if (cmd == ExternalEnd) { endExternal(); return true; }
        if (cmd == ExternalFlush) {
            if (externalMode) {
                recoveringOutput = false; outputAttempts = 0;
                sink.close(); externalFrames = 0;
                { std::lock_guard<std::mutex> lock(mutex); snapshot.position_ms = 0; }
                external.pause(externalPaused);
                if (externalPaused) state(NcmPlayback_Paused, "AirPlay locally paused");
                else if (sink.open()) state(NcmPlayback_Playing, "AirPlay receiving");
                else recoverOutput();
            }
            return true;
        }
        if (!externalMode) return false;
        if (cmd == NcmCommand_PlayPause || cmd == NcmCommand_Play || cmd == NcmCommand_Pause) {
            if (recoveringOutput && cmd == NcmCommand_Play) return true;
            recoveringOutput = false; outputAttempts = 0;
            externalPaused = cmd == NcmCommand_Pause ||
                (cmd == NcmCommand_PlayPause && !externalPaused);
            external.pause(externalPaused); sink.close();
            if (externalPaused) state(NcmPlayback_Paused, "AirPlay locally paused");
            else if (sink.open()) state(NcmPlayback_Playing, "AirPlay receiving");
            else recoverOutput();
            return true;
        }
        if (cmd == NcmCommand_Seek) return true; // Live audio has no seekable file.
        if (cmd == NcmCommand_LoadQueue || cmd == NcmCommand_Next ||
            cmd == NcmCommand_Previous || cmd == NcmCommand_Stop) {
            external.close(); endExternal();
        }
        (void)argument;
        return false;
    }
    void process(uint32_t cmd, uint32_t argument) {
        if (processExternal(cmd, argument)) return;
        switch (cmd) {
        case NcmCommand_ClearQueue: {
            if (!externalMode) closeTrack();
            queue.clear(); activeSession.clear(); resumeMusic=false; failures=0;
            std::lock_guard<std::mutex> lock(mutex);
            suspendedMusic={};
            suspendedMusic.version=NCM_PROTOCOL_VERSION;
            suspendedMusic.queue_index=UINT32_MAX;
            snapshot.queue_count=0; snapshot.queue_index=UINT32_MAX;
            if (!externalMode) {
                snapshot.track_id=0; snapshot.position_ms=0; snapshot.duration_ms=0;
                snapshot.state=NcmPlayback_Stopped; snapshot.error_code=0;
                snapshot.title[0]=0; snapshot.artist[0]=0;
                copyText(snapshot.message,"Account playback cleared");
            }
            break;
        }
        case NcmCommand_LoadQueue: resumeMusic=false; loadQueue(argument); break;
        case NcmCommand_Next: failures=0; advance(true); break;
        case NcmCommand_Previous:
            if (getStatus().position_ms > 3000) beginTrack(getStatus().queue_index);
            else { failures=0; advance(true,true); }
            break;
        case NcmCommand_Stop: closeTrack(); sink.close(); state(NcmPlayback_Stopped,"Stopped"); break;
        case NcmCommand_PlayPause:
        case NcmCommand_Play:
        case NcmCommand_Pause:
            if (recoveringOutput && cmd == NcmCommand_Play) break;
            if (!decoderOpen) {
                if (cmd != NcmCommand_Pause) {
                    failures=0;
                    if (queue.empty()) loadQueue(0);
                    else {
                        const auto previous = getStatus();
                        const bool restore = resumeMusic;
                        resumeMusic = false;
                        if (beginTrack(std::min<uint32_t>(previous.queue_index,queue.size()-1)) &&
                            restore && previous.position_ms && !seekDecoder(previous.position_ms)) {
                            closeTrack();
                            state(NcmPlayback_Error, "Cannot restore music position", 6);
                        }
                    }
                }
                break;
            }
            {
                recoveringOutput = false; outputAttempts = 0;
                const bool desired=cmd==NcmCommand_Pause || (cmd==NcmCommand_PlayPause && !paused);
                if (desired && !paused) {
                    // Keep the audible cursor, not the decoder's buffered-ahead
                    // cursor. Reopening a fresh session avoids stale release
                    // tags after the platform destroys its stopped audio stream.
                    paused=true; sink.close();
                } else if (!desired && paused) {
                    if (!seekDecoder(getStatus().position_ms)) {
                        closeTrack(); resumeMusic = !queue.empty();
                        state(NcmPlayback_Paused,"无法恢复播放位置，按播放重试",6); break;
                    }
                    paused=false;
                    if (!sink.open()) { recoverOutput(); break; }
                }
            }
            state(paused?NcmPlayback_Paused:NcmPlayback_Playing,
                paused?"Paused":preview?"Playing preview / 试听":"Playing");
            break;
        case NcmCommand_Volume: { std::lock_guard<std::mutex> lock(mutex); snapshot.volume_permille=argument; } break;
        case NcmCommand_Repeat: { std::lock_guard<std::mutex> lock(mutex); snapshot.repeat=argument; } break;
        case NcmCommand_Shuffle: { std::lock_guard<std::mutex> lock(mutex); snapshot.shuffle=argument; } break;
        case NcmCommand_Seek:
            if (decoderOpen) {
                recoveringOutput = false; outputAttempts = 0;
                sink.close();
                const uint32_t duration=getStatus().duration_ms;
                const uint32_t target=duration?std::min(argument,duration):argument;
                if (!seekDecoder(target)) {
                    closeTrack(); resumeMusic = !queue.empty();
                    state(NcmPlayback_Paused,"无法跳转，按播放重试",6);
                } else if (!paused && !sink.open()) {
                    recoverOutput();
                } else {
                    state(paused ? NcmPlayback_Paused : NcmPlayback_Playing,
                          paused ? "Paused" : preview ? "Playing preview / 试听" : "Playing");
                }
            }
            break;
        default: break;
        }
    }
    void runLoop() {
        std::array<int16_t, FramesPerBuffer*2> pcm{};
        auto stalledSince=std::chrono::steady_clock::time_point{};
        auto drainingSince=std::chrono::steady_clock::time_point{};
        auto lastIteration=std::chrono::steady_clock::now();
        while (running) {
            const auto iteration = std::chrono::steady_clock::now();
            const bool longGap = iteration - lastIteration > 2s;
            lastIteration = iteration;
            PendingCommand next{};
            bool hasCommand=false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!commands.empty()) {
                    next=commands.front(); commands.erase(commands.begin()); hasCommand=true;
                    interrupt=false;
                }
            }
            if (hasCommand) {
                process(next.first,next.second);
                lastIteration = std::chrono::steady_clock::now();
                if (next.ticket) {
                    std::lock_guard<std::mutex> lock(mutex);
                    clearCompleted=next.ticket;
                    cv.notify_all();
                }
                if (next.first != NcmCommand_Volume && next.first != NcmCommand_Repeat &&
                    next.first != NcmCommand_Shuffle) {
                    stalledSince={}; drainingSince={};
                }
                continue;
            }
            if (interrupt) { std::this_thread::sleep_for(2ms); continue; }
            if (recoveringOutput) {
                stalledSince={}; drainingSince={};
                retryOutput();
                lastIteration = std::chrono::steady_clock::now();
                std::this_thread::sleep_for(4ms);
                continue;
            }
            if (longGap && getStatus().state == NcmPlayback_Playing &&
                (decoderOpen || externalMode)) {
                recoverOutput();
                stalledSince = {};
                continue;
            }
            if (externalMode) {
                if (!externalPaused && external.active() && !sink.canWrite()) {
                    const auto now = std::chrono::steady_clock::now();
                    if (stalledSince == std::chrono::steady_clock::time_point{}) stalledSince = now;
                    if (sink.error() || now - stalledSince > 5s) {
                        recoverOutput();
                        stalledSince = {};
                    }
                    std::this_thread::sleep_for(4ms);
                    continue;
                }
                if (!externalPaused && external.active()) {
                    stalledSince = {};
                    const auto volume = getStatus().volume_permille;
                    const size_t frames = external.render(pcm.data(), FramesPerBuffer,
                        static_cast<float>(volume) * externalGain.load() / 1000000.f);
                    if (frames) {
                        if (!sink.write(pcm.data(), frames)) {
                            recoverOutput();
                        } else {
                            outputProgress();
                            if (!externalFrames) diagnostic("airplay-first-pcm", pcm.data(), frames);
                            externalFrames += frames;
                            const uint64_t elapsed = externalFrames * 1000 / SampleRate;
                            std::lock_guard<std::mutex> lock(mutex);
                            snapshot.position_ms = static_cast<uint32_t>(
                                elapsed > sink.queuedMs() ? elapsed - sink.queuedMs() : 0);
                        }
                        continue;
                    }
                }
                std::this_thread::sleep_for(4ms);
                continue;
            }
            if (decoderOpen && !paused) {
                if (!sink.canWrite()) {
                    diagnostic("waiting-output");
                    if (sink.error()) { recoverOutput(); stalledSince = {}; }
                    else {
                        const auto now=std::chrono::steady_clock::now();
                        if (stalledSince==std::chrono::steady_clock::time_point{}) stalledSince=now;
                        if (now-stalledSince>5s) {
                            recoverOutput(); stalledSince = {};
                        } else std::this_thread::sleep_for(4ms);
                    }
                    continue;
                }
                stalledSince={};
                size_t frames=render(pcm.data(),FramesPerBuffer,getStatus().volume_permille);
                lastIteration = std::chrono::steady_clock::now();
                if (interrupt || !running) continue;
                if (!frames) {
                    // Drain the last queued PCM before changing tracks.
                    if (sink.queuedMs()) {
                        const auto now=std::chrono::steady_clock::now();
                        if (drainingSince==std::chrono::steady_clock::time_point{}) drainingSince=now;
                        if (now-drainingSince>5s) recoverOutput();
                        else std::this_thread::sleep_for(5ms);
                        continue;
                    }
                    drainingSince={};
                    if (cacheError) {
                        reportCacheFailure();
                    } else if (downloadError) {
                        const auto error=static_cast<uint32_t>(downloadError.load());
                        closeTrack();
                        state(NcmPlayback_Error,"Network interrupted; track skipped",error);
                    } else if (decoder.damaged()) {
                        closeTrack();
                        invalidateCache();
                        state(NcmPlayback_Error,"Damaged or truncated audio; track skipped",5);
                    } else {
                        failures = 0;
                        advance(false);
                    }
                } else if (!sink.write(pcm.data(),frames)) {
                    recoverOutput();
                } else {
                    drainingSince={};
                    outputProgress();
                    if (!producedFrames) diagnostic("first-pcm",pcm.data(),frames);
                    producedFrames+=frames;
                    uint64_t milliseconds=producedFrames*1000/SampleRate;
                    std::lock_guard<std::mutex> lock(mutex);
                    snapshot.position_ms=seekBase+static_cast<uint32_t>(milliseconds>sink.queuedMs()?milliseconds-sink.queuedMs():0);
                }
            } else if (getStatus().state == NcmPlayback_Error && getStatus().error_code != 10 &&
                       !queue.empty() && ++failures < queue.size()) {
                // Bounded skipping prevents an unavailable album spinning forever.
                closeTrack();
                std::this_thread::sleep_for(250ms);
                advance(true); // An unavailable repeat-one track must not loop forever.
            } else {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait_for(lock,50ms,[this]{return !commands.empty() || !running;});
            }
        }
        closeTrack(); sink.close();
    }
    void run() {
        while (running) {
            try { runLoop(); }
            catch (const std::bad_alloc&) {
                external.close(); endExternal();
                closeTrack();
                state(NcmPlayback_Error, "内存不足，请选择其他音源后重试", 10);
            }
        }
    }
};

Engine::Engine(EngineConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
Engine::~Engine() { stop(); }
bool Engine::start() {
    if (impl_->running.exchange(true)) return true;
    if (impl_->worker.joinable()) impl_->worker.join();
    std::error_code error;
    std::filesystem::create_directories(impl_->config.directory + "/cache",error);
    if (error) { impl_->running=false; return false; }
    try {
        const auto preferences = impl_->settings();
        impl_->diagnostics = preferences.contains("diagnostics") && preferences["diagnostics"].is_boolean()
            && preferences["diagnostics"].get<bool>();
        if (impl_->diagnostics) {
            if (FILE* file=std::fopen((impl_->config.directory+"/audio.log").c_str(),"wb")) std::fclose(file);
        }
        impl_->worker=std::thread([this]{
            try { impl_->run(); }
            catch (const std::exception& error) {
                impl_->closeTrack(); impl_->state(NcmPlayback_Error,error.what(),8);
                impl_->running=false;
            }
#ifndef __SWITCH__
            // std::thread does not run SDL_CreateThread's TLS destructor.
            SDL_TLSCleanup();
#endif
        });
    } catch (...) { impl_->running=false; return false; }
    return true;
}
void Engine::stop() {
    impl_->running=false; impl_->interrupt=true; impl_->downloadCancel=true; impl_->cv.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
    impl_->external.close();
    impl_->externalMode = false;
}
bool Engine::command(NcmCommand cmd,uint32_t argument) {
    if (!impl_->running || cmd < NcmCommand_LoadQueue || cmd > NcmCommand_ClearQueue ||
        (cmd==NcmCommand_Volume && argument>1000) ||
        (cmd==NcmCommand_Repeat && argument>NcmRepeat_All) ||
        (cmd==NcmCommand_Shuffle && argument>1) ||
        (cmd==NcmCommand_LoadQueue && argument>=NCM_MAX_QUEUE)) return false;
    std::unique_lock<std::mutex> lock(impl_->mutex);
    if (cmd==NcmCommand_ClearQueue) {
        // Supersede stale queued music commands before waiting for the active
        // decoder/download to stop. Keep AirPlay lifecycle and volume/modes.
        impl_->commands.erase(std::remove_if(impl_->commands.begin(),impl_->commands.end(),
            [](const auto& pending) {
                return pending.first>=NcmCommand_LoadQueue && pending.first<=NcmCommand_Stop &&
                    pending.first!=NcmCommand_Volume && pending.first!=NcmCommand_Repeat &&
                    pending.first!=NcmCommand_Shuffle;
            }),impl_->commands.end());
    }
    if (impl_->commands.size()>=CommandCapacity) return false;
    const uint64_t ticket=cmd==NcmCommand_ClearQueue ? ++impl_->clearRequested : 0;
    impl_->commands.emplace_back(cmd,argument,ticket);
    if (cmd==NcmCommand_LoadQueue || cmd==NcmCommand_Next || cmd==NcmCommand_Previous ||
        cmd==NcmCommand_Stop || cmd==NcmCommand_ClearQueue) impl_->interrupt=true;
    impl_->cv.notify_all();
    if (!ticket) return true;
    // Switching accounts must wait for the worker to relinquish the old song,
    // including an in-flight LoadQueue or download. Timeout leaves activation
    // to the caller; it must not replace the account's cookie jar on failure.
    return impl_->cv.wait_for(lock,5s,[this,ticket] {
        return impl_->clearCompleted>=ticket || !impl_->running;
    }) && impl_->clearCompleted>=ticket;
}
NcmPlaybackStatus Engine::status() const { return impl_->getStatus(); }
bool Engine::begin_external(uint32_t rate) {
    if (!impl_->running || (rate != 44100 && rate != 48000)) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->commands.size() >= CommandCapacity) return false;
    if (!impl_->external.open(rate)) return false;
    impl_->externalGain = 1000;
    // A new sender takes the audio source. Supersede pending music transport
    // commands, retaining volume/mode changes and earlier lifecycle barriers.
    impl_->commands.erase(std::remove_if(impl_->commands.begin(), impl_->commands.end(),
        [](const auto& command) {
            const auto kind = command.first;
            return kind >= NcmCommand_LoadQueue && kind <= NcmCommand_Stop &&
                kind != NcmCommand_Volume && kind != NcmCommand_Repeat &&
                kind != NcmCommand_Shuffle;
        }), impl_->commands.end());
    impl_->commands.emplace_back(ExternalBegin, rate);
    impl_->interrupt = true;
    impl_->cv.notify_one();
    return true;
}
bool Engine::write_external(const int16_t* stereo, size_t frames) {
    return impl_->running && impl_->external.push(stereo, frames);
}
void Engine::end_external() {
    impl_->external.close();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    // Repeated End is idempotent. Ordinary producers cannot fill the reserved
    // final slot, and consecutive End calls never grow the vector.
    if (impl_->commands.empty() || impl_->commands.back().first != ExternalEnd)
        impl_->commands.emplace_back(ExternalEnd, 0);
    impl_->interrupt = true;
    impl_->cv.notify_one();
}
void Engine::flush_external() {
    impl_->external.flush();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->commands.size() < CommandCapacity) impl_->commands.emplace_back(ExternalFlush, 0);
    impl_->cv.notify_one();
}
void Engine::external_volume(uint32_t permille) { impl_->externalGain = std::min(permille, 1000u); }
bool Engine::external_active() const { return impl_->external.active(); }
}
