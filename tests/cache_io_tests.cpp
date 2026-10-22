/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "engine.hpp"
#include "netease/crypto.hpp"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;
using netease::playback::Engine;
namespace {
std::mutex filesMutex;
std::condition_variable opened;
struct Handle {
    std::string path;
    bool write;
};
std::map<FILE *, Handle> handles;
std::map<std::string, unsigned> writes;
std::string fault;
std::atomic<unsigned> rejected{0}, injected{0};
std::atomic<bool> armFault{true};
bool cachePath(const char *path) {
    const std::string name(path);
    return name.find("/cache/") != std::string::npos &&
           (std::filesystem::path(name).extension() == ".audio" ||
            std::filesystem::path(name).extension() == ".mp3");
}
bool fail(const char *operation, FILE *file) {
    std::lock_guard<std::mutex> lock(filesMutex);
    if (armFault && fault == operation && handles.count(file)) {
        ++injected;
        errno = fault == "write" || fault == "flush" || fault == "sync" ? ENOSPC : EIO;
        return true;
    }
    return false;
}
void check(bool condition, const char *reason) {
    if (!condition)
        throw std::runtime_error(reason);
}
template <class Predicate>
void waitFor(Engine &engine, Predicate predicate, const char *reason, int timeout = 10000,
             bool failFast = true) {
    for (int i = 0; i < timeout / 10; ++i) {
        auto status = engine.status();
        if (predicate(status))
            return;
        if (failFast && status.state == NcmPlayback_Error) {
            std::fprintf(stderr, "status=%u code=%u message=%s\n", status.state, status.error_code,
                         status.message);
            throw std::runtime_error(reason);
        }
        std::this_thread::sleep_for(10ms);
    }
    throw std::runtime_error(reason);
}
void queue(const std::string &directory, const std::string &url, uint64_t id) {
    NcmQueueEntry track{};
    track.id = id;
    track.duration_ms = 30000;
    std::snprintf(track.url, sizeof(track.url), "%s", url.c_str());
    check(ncmWriteQueueFile((directory + "/queue.nsq").c_str(), &track, 1) == 0, "queue write");
}
std::string bytes(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
} // namespace

extern "C" {
void *__real_av_malloc(size_t);
void *__wrap_av_malloc(size_t size) {
    if (fault.rfind("memory-", 0) == 0 &&
        (fault == "memory-permanent" || armFault.exchange(false))) {
        ++injected;
        return nullptr;
    }
    return __real_av_malloc(size);
}
FILE *__real_fopen(const char *, const char *);
int __real_fclose(FILE *);
size_t __real_fread(void *, size_t, size_t, FILE *);
size_t __real_fwrite(const void *, size_t, size_t, FILE *);
int __real_fflush(FILE *);
int __real_fseek(FILE *, long, int);
int __real_fsync(int);
FILE *__wrap_fopen(const char *path, const char *mode) {
    if (!cachePath(path))
        return __real_fopen(path, mode);
    std::unique_lock<std::mutex> lock(filesMutex);
    bool write = std::strpbrk(mode, "wa+") != nullptr;
    // Schedule the old truncate/reader/downloader race consistently. The FS
    // contract under test allows one writer handle, including read/write mode,
    // but forbids a second handle to a file already open for writing.
    if (!write && writes[path] == 1 && handles.empty())
        opened.wait_for(lock, 500ms, [&] { return !handles.empty(); });
    for (const auto &item : handles) {
        if (item.second.path == path && (write || item.second.write)) {
            ++rejected;
            errno = EBUSY;
            return nullptr;
        }
    }
    if (fault == "open" && armFault) {
        ++injected;
        errno = EACCES;
        return nullptr;
    }
    FILE *file = __real_fopen(path, mode);
    if (file) {
        handles[file] = {path, write};
        if (write)
            ++writes[path];
        opened.notify_all();
    }
    return file;
}
int __wrap_fclose(FILE *file) {
    std::lock_guard<std::mutex> lock(filesMutex);
    handles.erase(file);
    return __real_fclose(file);
}
size_t __wrap_fread(void *data, size_t size, size_t count, FILE *file) {
    return fail("read", file) ? 0 : __real_fread(data, size, count, file);
}
size_t __wrap_fwrite(const void *data, size_t size, size_t count, FILE *file) {
    // A short write must not publish even its prefix as readable audio.
    if (fail("write", file))
        return count ? count - 1 : 0;
    return __real_fwrite(data, size, count, file);
}
int __wrap_fflush(FILE *file) {
    return fail("flush", file) ? EOF : __real_fflush(file);
}
int __wrap_fseek(FILE *file, long offset, int origin) {
    return fail("seek", file) ? -1 : __real_fseek(file, offset, origin);
}
int __wrap_fsync(int fd) {
    std::lock_guard<std::mutex> lock(filesMutex);
    for (const auto &item : handles) {
        if (fileno(item.first) == fd && armFault && fault == "sync") {
            ++injected;
            errno = ENOSPC;
            return -1;
        }
    }
    return __real_fsync(fd);
}
}

int main(int argc, char **argv) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    try {
        check(argc == 6, "usage: cache-io DIRECTORY URL CA SOURCE SCENARIO");
        std::string directory = argv[1], scenario = argv[5];
        if (scenario != "stream" && scenario != "cancel" && scenario != "invalid" &&
            scenario != "corrupt-cache")
            fault = scenario;
        std::filesystem::create_directories(directory);
        // Failed local storage must not try to renew the CDN URL. A closed
        // proxy makes an accidental renewal deterministic and completely local.
        std::ofstream(directory + "/settings.json") << "{\"proxy\":\"http://127.0.0.1:1\"}";
        const auto cache =
            directory + "/cache/900-" + netease::crypto::md5("anonymous\nstandard") + ".audio";
        if (scenario == "corrupt-cache") {
            std::filesystem::create_directories(directory + "/cache");
            std::ofstream(cache, std::ios::binary) << "fLaCdamaged metadata";
            std::ofstream(cache + ".complete").close();
        }
        queue(directory, argv[2], 900);
        Engine engine({directory, argv[3]});
        check(engine.start() && engine.command(NcmCommand_LoadQueue), "engine start");
        if (fault.rfind("memory-", 0) == 0) {
            waitFor(
                engine,
                [](auto s) {
                    return s.state == NcmPlayback_Buffering && std::strstr(s.message, "Memory low");
                },
                "no low-memory download fallback");
            check(injected == 1 && !std::filesystem::exists(cache + ".complete"),
                  "test did not inject allocation failure during download");
            if (scenario == "memory-cancel") {
                const auto begin = std::chrono::steady_clock::now();
                check(engine.command(NcmCommand_ClearQueue), "low-memory wait cannot cancel");
                check(std::chrono::steady_clock::now() - begin < 1s,
                      "low-memory cancellation blocked");
                check(!std::filesystem::exists(cache + ".complete"),
                      "cancel blessed partial cache");
            } else if (scenario == "memory-permanent") {
                waitFor(
                    engine,
                    [](auto s) { return s.state == NcmPlayback_Error && s.error_code == 10; },
                    "persistent OOM not reported", 10000, false);
                check(injected == 2, "memory retry was not bounded to one attempt");
                check(std::filesystem::exists(cache + ".complete"), "OOM invalidated valid cache");
            } else {
                waitFor(
                    engine,
                    [](auto s) { return s.state == NcmPlayback_Playing && s.position_ms > 100; },
                    "completed download did not recover memory-limited playback");
                check(injected == 1 && bytes(cache) == bytes(argv[4]),
                      "memory recovery corrupted or replaced the transfer");
            }
            engine.stop();
        } else if (!fault.empty() || scenario == "invalid") {
            waitFor(
                engine, [](auto s) { return s.state == NcmPlayback_Error; }, "expected failure");
            auto status = engine.status();
            if (!fault.empty()) {
                check(injected > 0, "fault did not reach cache");
                check(status.error_code == 3 && std::strstr(status.message, "SD cache"),
                      "storage failure misreported as network/codec failure");
                check(std::strstr(status.message, "errno="), "missing SD error details");
                check(bytes(directory + "/cache-errors.log").find("errno=") != std::string::npos,
                      "no persistent storage diagnostic");
            }
            engine.stop();
            check(!std::filesystem::exists(cache + ".complete"), "failed cache marked complete");
            if (!fault.empty()) {
                armFault = false;
                check(engine.start() && engine.command(NcmCommand_LoadQueue), "recovery start");
                waitFor(
                    engine,
                    [](auto s) { return s.state == NcmPlayback_Playing && s.position_ms > 100; },
                    "recovery after SD failure", 10000, false);
                engine.stop();
            }
        } else {
            waitFor(
                engine,
                [](auto s) { return s.state == NcmPlayback_Playing && s.position_ms > 100; },
                "progressive playback failed");
            check(!std::filesystem::exists(cache + ".complete"), "waited for complete download");
            if (scenario == "cancel") {
                check(engine.command(NcmCommand_ClearQueue), "cancel failed");
                check(!std::filesystem::exists(cache + ".complete"),
                      "cancelled cache marked complete");
            } else {
                engine.command(NcmCommand_Pause);
                waitFor(engine, [](auto s) { return s.state == NcmPlayback_Paused; }, "pause");
                engine.command(NcmCommand_Seek, 2000);
                waitFor(
                    engine, [](auto s) { return s.position_ms == 2000; }, "seek while downloading");
                engine.command(NcmCommand_Play);
                waitFor(engine, [](auto s) { return s.position_ms > 2150; }, "resume");
                waitFor(
                    engine, [&](auto) { return std::filesystem::exists(cache + ".complete"); },
                    "completion");
                engine.command(NcmCommand_Stop);
                waitFor(engine, [](auto s) { return s.state == NcmPlayback_Stopped; }, "stop");
                check(bytes(cache) == bytes(argv[4]), "concurrent reads/seeks corrupted download");
                // A cached replay must not contact the unreachable origin.
                queue(directory, "https://127.0.0.1:1/unreachable", 900);
                engine.command(NcmCommand_LoadQueue);
                waitFor(
                    engine,
                    [](auto s) { return s.state == NcmPlayback_Playing && s.position_ms > 100; },
                    "offline replay");
            }
            engine.stop();
        }
        check(handles.empty(), "cache handle leaked");
        check(rejected == 0, "attempted conflicting cache opens");
        std::printf("PASS cache-io %s\n", scenario.c_str());
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL cache-io: %s (conflicting opens=%u)\n", e.what(),
                     rejected.load());
        curl_global_cleanup();
        return 1;
    }
    curl_global_cleanup();
}
