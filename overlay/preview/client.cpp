/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "netease/playback_client.h"
#include "host_protocol.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
using Clock = std::chrono::steady_clock;
constexpr auto kRpcTimeout = std::chrono::milliseconds(350);
constexpr Result kProtocolError = MAKERESULT(421, 1);
std::atomic<bool> active{false};

class Socket {
  public:
    explicit Socket(int descriptor) : fd(descriptor) {}
    ~Socket() { if (fd >= 0) ::close(fd); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    const int fd;
};

Result transport_error(int error) {
    active.store(false);
    if (error == ETIMEDOUT || error == EAGAIN || error == EWOULDBLOCK)
        return MAKERESULT(Module_Libnx, LibnxError_Timeout);
    if (error == ENOENT || error == ECONNREFUSED)
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    return MAKERESULT(Module_Libnx, LibnxError_IoError);
}

bool wait_ready(int fd, short events, Clock::time_point deadline) {
    for (;;) {
        const auto remaining = deadline - Clock::now();
        if (remaining <= Clock::duration::zero()) {
            errno = ETIMEDOUT;
            return false;
        }
        const auto milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count() + 1;
        pollfd descriptor{fd, events, 0};
        const int result = ::poll(&descriptor, 1, static_cast<int>(milliseconds));
        if (result > 0) {
            // A peer can close immediately after writing its complete response.
            // Let recv consume any remaining bytes before interpreting EOF.
            if (descriptor.revents & (events | POLLHUP))
                return true;
            errno = EIO;
            return false;
        }
        if (result == 0) {
            errno = ETIMEDOUT;
            return false;
        }
        if (errno != EINTR)
            return false;
    }
}

bool transfer(int fd, void* buffer, size_t size, bool sending, Clock::time_point deadline) {
    auto* bytes = static_cast<unsigned char*>(buffer);
    size_t offset = 0;
    while (offset < size) {
        if (!wait_ready(fd, sending ? POLLOUT : POLLIN, deadline))
            return false;
        const ssize_t count = sending
            ? ::send(fd, bytes + offset, size - offset, MSG_NOSIGNAL)
            : ::recv(fd, bytes + offset, size - offset, 0);
        if (count > 0) {
            offset += static_cast<size_t>(count);
        } else if (count == 0) {
            errno = ECONNRESET;
            return false;
        } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            return false;
        }
    }
    return true;
}

Result exchange(NcmCommand command, uint32_t argument, NcmPlaybackStatus* output = nullptr) {
    const char* directory = std::getenv("CLOUDTUNE_PREVIEW_DIRECTORY");
    std::string path = directory && *directory ? directory : ".runtime/overlay-preview";
    if (path.back() != '/')
        path += '/';
    path += NCM_HOST_SOCKET_NAME;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) {
        active.store(false);
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);

    const Socket socket(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (socket.fd < 0)
        return transport_error(errno);
    const timeval timeout{0, 350000};
    if (::setsockopt(socket.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        ::setsockopt(socket.fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0)
        return transport_error(errno);

    const auto deadline = Clock::now() + kRpcTimeout;
    if (::connect(socket.fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        if (errno != EINPROGRESS)
            return transport_error(errno);
        if (!wait_ready(socket.fd, POLLOUT, deadline))
            return transport_error(errno);
        int error = 0;
        socklen_t length = sizeof(error);
        if (::getsockopt(socket.fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0)
            return transport_error(errno);
        if (error != 0)
            return transport_error(error);
    }

    NcmHostRequest request{NCM_HOST_MAGIC, NCM_HOST_VERSION,
                           static_cast<uint32_t>(command), argument};
    NcmHostResponse response{};
    if (!transfer(socket.fd, &request, sizeof(request), true, deadline) ||
        !transfer(socket.fd, &response, sizeof(response), false, deadline))
        return transport_error(errno);

    // The server's result is returned unchanged; a rejected playback command
    // does not imply that the transport itself disconnected.
    if (response.result != NCM_HOST_RESULT_OK)
        return response.result;
    NcmPlaybackStatus status{};
    std::memcpy(&status, &response.status, sizeof(status));
    if (status.version != NCM_PROTOCOL_VERSION) {
        active.store(false);
        return kProtocolError;
    }
    status.title[sizeof(status.title) - 1] = '\0';
    status.artist[sizeof(status.artist) - 1] = '\0';
    status.message[sizeof(status.message) - 1] = '\0';
    if (output)
        *output = status;
    return 0;
}

Result command(NcmCommand kind, uint32_t argument = 0) {
    if (!active.load())
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    return exchange(kind, argument);
}
} // namespace

extern "C" {
Result ncmPlaybackInitialize(void) {
    active.store(false);
    Result result = exchange(NcmCommand_Version, 0);
    if (R_SUCCEEDED(result))
        result = exchange(NcmCommand_Status, 0);
    active.store(R_SUCCEEDED(result));
    return result;
}

void ncmPlaybackExit(void) {
    // This client never owns the independently running playback process.
    active.store(false);
}
void ncmPlaybackShutdown(void) { ncmPlaybackExit(); }

bool ncmPlaybackIsActive(void) { return active.load(); }

Result ncmPlaybackGetStatus(NcmPlaybackStatus* status) {
    if (!status)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    std::memset(status, 0, sizeof(*status));
    if (!active.load())
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    return exchange(NcmCommand_Status, 0, status);
}

Result ncmPlaybackLoadQueue(uint32_t index) { return command(NcmCommand_LoadQueue, index); }
Result ncmPlaybackPlayPause(void) { return command(NcmCommand_PlayPause); }
Result ncmPlaybackPlay(void) { return command(NcmCommand_Play); }
Result ncmPlaybackPause(void) { return command(NcmCommand_Pause); }
Result ncmPlaybackNext(void) { return command(NcmCommand_Next); }
Result ncmPlaybackPrevious(void) { return command(NcmCommand_Previous); }
Result ncmPlaybackSetVolume(uint32_t permille) { return command(NcmCommand_Volume, permille); }
Result ncmPlaybackSeek(uint32_t milliseconds) { return command(NcmCommand_Seek, milliseconds); }
Result ncmPlaybackSetRepeat(NcmRepeatMode mode) {
    return command(NcmCommand_Repeat, static_cast<uint32_t>(mode));
}
Result ncmPlaybackSetShuffle(bool enabled) { return command(NcmCommand_Shuffle, enabled ? 1u : 0u); }
Result ncmPlaybackStop(void) { return command(NcmCommand_Stop); }
Result ncmPlaybackLaunchBackground(void) {
    // The preview launcher script owns process creation.
    return ncmPlaybackInitialize();
}
} // extern "C"
