/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Independent Linux playback service for the production overlay's host preview.
 * It owns the real Engine/SDL audio device. Closing or restarting the preview
 * client does not terminate playback. This is a test tool, not a Switch service.
 */
#include "engine.hpp"
#include "../../overlay/preview/host_protocol.h"
#include <curl/curl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using netease::playback::Engine;
volatile sig_atomic_t interrupted = 0;
void onSignal(int) { interrupted = 1; }

class Descriptor {
public:
    explicit Descriptor(int value = -1) : value_(value) {}
    ~Descriptor() { if (value_ >= 0) ::close(value_); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const { return value_; }
private:
    int value_;
};

[[noreturn]] void fail(const char* operation) {
    throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
}

bool waitSocket(int fd, short events, Clock::time_point deadline) {
    while (!interrupted) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (remaining <= 0) return false;
        pollfd descriptor{fd, events, 0};
        const int result = ::poll(&descriptor, 1, static_cast<int>(remaining));
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) return false;
        // POLLIN may accompany POLLHUP when a peer sends a frame then closes.
        if (descriptor.revents & events) return true;
        if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) return false;
    }
    return false;
}

bool receiveFrame(int fd, void* bytes, size_t size) {
    const auto deadline = Clock::now() + 500ms;
    auto* output = static_cast<unsigned char*>(bytes);
    size_t received = 0;
    while (received < size && waitSocket(fd, POLLIN, deadline)) {
        const ssize_t count = ::recv(fd, output + received, size - received, 0);
        if (count > 0) received += static_cast<size_t>(count);
        else if (count == 0) return false;
        else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return false;
    }
    return received == size;
}

bool sendFrame(int fd, const void* bytes, size_t size) {
    const auto deadline = Clock::now() + 500ms;
    const auto* input = static_cast<const unsigned char*>(bytes);
    size_t sent = 0;
    while (sent < size && waitSocket(fd, POLLOUT, deadline)) {
        const ssize_t count = ::send(fd, input + sent, size - sent, MSG_NOSIGNAL);
        if (count > 0) sent += static_cast<size_t>(count);
        else if (count == 0) return false;
        else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return false;
    }
    return sent == size;
}

class LocalSocket {
public:
    explicit LocalSocket(const std::string& directory)
        : path_(directory + "/" NCM_HOST_SOCKET_NAME),
          lock_(::open((directory + "/.service.lock").c_str(),
                       O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600)),
          listener_(::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)) {
        if (lock_.get() < 0) fail("open service lock");
        if (::flock(lock_.get(), LOCK_EX | LOCK_NB) != 0)
            throw std::runtime_error("A preview playback service already owns this directory");
        if (listener_.get() < 0) fail("create local socket");
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (path_.size() >= sizeof(address.sun_path))
            throw std::runtime_error("Preview directory is too long for a Unix socket path");
        std::memcpy(address.sun_path, path_.c_str(), path_.size() + 1);
        struct stat existing{};
        if (::lstat(path_.c_str(), &existing) == 0) {
            if (!S_ISSOCK(existing.st_mode) || existing.st_uid != ::geteuid())
                throw std::runtime_error("Refusing to replace a non-owned socket path");
            Descriptor probe(::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
            if (probe.get() < 0) fail("create socket probe");
            if (::connect(probe.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 ||
                errno != ECONNREFUSED)
                throw std::runtime_error("An existing playback socket is active or inaccessible");
            if (::unlink(path_.c_str()) != 0) fail("remove stale socket");
        } else if (errno != ENOENT) fail("inspect socket path");
        if (::bind(listener_.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
            fail("bind local socket");
        // Remember the created inode before any further operation can fail.
        if (::lstat(path_.c_str(), &identity_) != 0) fail("inspect bound socket");
        bound_ = true;
        if (::chmod(path_.c_str(), 0600) != 0 || ::listen(listener_.get(), 8) != 0) {
            const int saved = errno;
            removeOwnedSocket();
            errno = saved;
            fail("prepare local socket");
        }
    }
    ~LocalSocket() { removeOwnedSocket(); }
    int get() const { return listener_.get(); }
    const std::string& path() const { return path_; }
private:
    void removeOwnedSocket() {
        struct stat current{};
        if (bound_ && ::lstat(path_.c_str(), &current) == 0 &&
            current.st_dev == identity_.st_dev && current.st_ino == identity_.st_ino)
            ::unlink(path_.c_str());
        bound_ = false;
    }
    std::string path_;
    Descriptor lock_;
    Descriptor listener_;
    struct stat identity_{};
    bool bound_ = false;
};

std::string privateDirectory(const std::string& requested) {
    std::error_code error;
    auto path = std::filesystem::absolute(requested, error).lexically_normal();
    if (error) throw std::runtime_error("Cannot resolve preview directory");
    std::filesystem::create_directories(path, error);
    if (error) throw std::runtime_error("Cannot create preview directory: " + error.message());
    Descriptor directory(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (directory.get() < 0) fail("open preview directory");
    struct stat info{};
    if (::fstat(directory.get(), &info) != 0) fail("inspect preview directory");
    if (info.st_uid != ::geteuid())
        throw std::runtime_error("Preview directory must belong to the current user");
    if (::fchmod(directory.get(), 0700) != 0) fail("secure preview directory");
    return path.string();
}

// The demo queue is deliberately local-only. A missing URL would otherwise
// invoke the account API; this tool must never implicitly load login data.
bool checkLocalQueue(const std::string& directory) {
    const auto path = directory + "/queue.nsq";
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        if (errno == ENOENT) return false;
        fail("open demo queue");
    }
    NcmQueueHeader header{};
    bool valid = std::fread(&header, sizeof(header), 1, file) == 1 &&
        std::memcmp(header.magic, "NCMQUEUE", 8) == 0 &&
        header.version == NCM_PROTOCOL_VERSION && header.count > 0 &&
        header.count <= NCM_MAX_QUEUE;
    for (uint32_t i = 0; valid && i < header.count; ++i) {
        NcmQueueEntry entry{};
        valid = std::fread(&entry, sizeof(entry), 1, file) == 1 &&
            entry.url[0] == '/' && std::memchr(entry.url, 0, sizeof(entry.url)) != nullptr;
        if (valid) {
            std::error_code error;
            valid = std::filesystem::is_regular_file(entry.url, error) && !error;
        }
    }
    if (valid) valid = std::fgetc(file) == EOF && !std::ferror(file);
    std::fclose(file);
    if (!valid) throw std::runtime_error("Demo queue must contain existing absolute local MP3 paths");
    return true;
}

void prepareEngine(Engine& engine, bool haveQueue) {
    if (!engine.start() || !engine.command(NcmCommand_Volume, 0))
        throw std::runtime_error("Cannot start the preview playback engine");
    // LoadQueue in the production engine also starts the track. Pause is queued
    // immediately, before the decode loop consumes audio, and volume is already
    // zero. Seek then restores an exact zero cursor before accepting clients.
    if (haveQueue && (!engine.command(NcmCommand_LoadQueue, 0) ||
                      !engine.command(NcmCommand_Pause)))
        throw std::runtime_error("Cannot preload the demo queue");
    const auto deadline = Clock::now() + 5s;
    while (!interrupted && Clock::now() < deadline) {
        const auto status = engine.status();
        if (status.state == NcmPlayback_Error)
            throw std::runtime_error(std::string("Cannot preload demo audio: ") + status.message);
        if (status.volume_permille == 0 && (!haveQueue || status.state == NcmPlayback_Paused)) {
            if (haveQueue && !engine.command(NcmCommand_Seek, 0))
                throw std::runtime_error("Cannot reset the initial demo cursor");
            return;
        }
        std::this_thread::sleep_for(5ms);
    }
    if (!interrupted) throw std::runtime_error("Timed out preloading demo audio");
}

uint32_t dispatch(Engine& engine, const NcmHostRequest& request,
                  const std::string& directory, bool& shutdown) {
    if (request.magic != NCM_HOST_MAGIC || request.version != NCM_HOST_VERSION)
        return NCM_HOST_RESULT_PROTOCOL;
    switch (request.command) {
    case NcmCommand_Version:
    case NcmCommand_Status:
    case NcmCommand_Ping: return NCM_HOST_RESULT_OK;
    case NCM_HOST_SHUTDOWN: shutdown = true; return NCM_HOST_RESULT_OK;
    default:
        if (request.command < NcmCommand_LoadQueue || request.command > NcmCommand_Stop)
            return NCM_HOST_RESULT_COMMAND;
        // Validate every explicit reload; Play may load an initially absent
        // queue too. This keeps the preview isolated from network/account use.
        if (request.command == NcmCommand_LoadQueue ||
            ((request.command == NcmCommand_Play || request.command == NcmCommand_PlayPause) &&
             engine.status().queue_count == 0)) {
            try { if (!checkLocalQueue(directory)) return NCM_HOST_RESULT_REJECTED; }
            catch (const std::exception&) { return NCM_HOST_RESULT_REJECTED; }
        }
        return engine.command(static_cast<NcmCommand>(request.command), request.argument)
            ? NCM_HOST_RESULT_OK : NCM_HOST_RESULT_REJECTED;
    }
}
} // namespace

int main(int argc, char** argv) {
    std::string requestedDirectory = ".runtime/overlay-preview";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--directory") == 0 && i + 1 < argc)
            requestedDirectory = argv[++i];
        else {
            std::fprintf(stderr, "usage: %s [--directory PATH]\n"
                "Place a local-only queue.nsq in PATH before starting. Initial volume is zero.\n",
                argv[0]);
            return std::strcmp(argv[i], "--help") == 0 ? 0 : 2;
        }
    }
    if (requestedDirectory.empty()) {
        std::fputs("Preview directory cannot be empty\n", stderr);
        return 2;
    }
    ::umask(0077);
    struct sigaction action{};
    action.sa_handler = onSignal;
    ::sigemptyset(&action.sa_mask);
    ::sigaction(SIGTERM, &action, nullptr);
    ::sigaction(SIGINT, &action, nullptr);
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        std::fputs("Cannot initialize libcurl\n", stderr);
        return 1;
    }
    int result = 0;
    try {
        const auto directory = privateDirectory(requestedDirectory);
        LocalSocket server(directory);
        const bool haveQueue = checkLocalQueue(directory);
        Engine engine({directory, ""});
        prepareEngine(engine, haveQueue);
        std::fprintf(stdout, "READY %s (initial volume 0; queue %s)\n",
                     server.path().c_str(), haveQueue ? "paused" : "empty");
        std::fflush(stdout);
        bool shutdown = false;
        while (!interrupted && !shutdown) {
            if (!waitSocket(server.get(), POLLIN, Clock::now() + 100ms)) continue;
            Descriptor client(::accept4(server.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
            if (client.get() < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
                fail("accept preview client");
            }
            struct ucred peer{};
            socklen_t size = sizeof(peer);
            if (::getsockopt(client.get(), SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 ||
                peer.uid != ::geteuid()) continue;
            NcmHostRequest request{};
            if (!receiveFrame(client.get(), &request, sizeof(request))) continue;
            NcmHostResponse response{};
            try { response.result = dispatch(engine, request, directory, shutdown); }
            catch (const std::exception&) { response.result = NCM_HOST_RESULT_INTERNAL; }
            const auto status = engine.status();
            std::memcpy(&response.status, &status, sizeof(status));
            // Failure to deliver a reply never stops or pauses playback.
            sendFrame(client.get(), &response, sizeof(response));
        }
        engine.stop();
        std::fputs("STOPPED preview playback service\n", stdout);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Preview playback service: %s\n", error.what());
        result = 1;
    }
    curl_global_cleanup();
    return result;
}
