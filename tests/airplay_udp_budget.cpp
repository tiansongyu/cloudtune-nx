/* SPDX-License-Identifier: GPL-3.0-or-later */
// Fault/budget model for tests, not an emulation of Horizon's BSD allocator.
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sys/socket.h>

namespace {
struct Socket {
    bool udp = false;
    int send = 0, receive = 0;
};
std::array<Socket, 1024> sockets{};
std::mutex mutex;
char operation[24]{};
unsigned remaining = 0;
int receiveLimit = 0;
bool fail(const char *name) {
    if (remaining && !std::strcmp(name, operation) && !--remaining) {
        errno = EIO;
        return true;
    }
    return false;
}
bool fits(int oldBytes, int newBytes) {
    const char *budget = std::getenv("CLOUDTUNE_TEST_UDP_BUDGET");
    if (!budget)
        return true;
    int used = newBytes - oldBytes;
    for (const auto &entry : sockets)
        used += entry.send + entry.receive;
    if (used <= std::atoi(budget))
        return true;
    errno = ENOBUFS;
    return false;
}
} // namespace
bool airplayUdpTestCommand(const char *command) {
    std::lock_guard<std::mutex> lock(mutex);
    if (std::sscanf(command, "udp-fail %23s %u", operation, &remaining) == 2)
        return true;
    if (std::sscanf(command, "udp-limit %d", &receiveLimit) == 1)
        return true;
    return false;
}
extern "C" int __real_socket(int, int, int);
extern "C" int __wrap_socket(int domain, int type, int protocol) {
    std::lock_guard<std::mutex> lock(mutex);
    const bool udp = type == SOCK_DGRAM;
    // Match the production initial defaults. Tests also keep mDNS open.
    if (udp && (fail("socket") || !fits(0, 0x2400 + 16 * 1024)))
        return -1;
    const int fd = __real_socket(domain, type, protocol);
    if (fd >= int(sockets.size()))
        std::abort();
    if (fd >= 0)
        sockets[fd] = {udp, udp ? 0x2400 : 0, udp ? 16 * 1024 : 0};
    return fd;
}
extern "C" int __real_setsockopt(int, int, int, const void *, socklen_t);
extern "C" int __wrap_setsockopt(int fd, int level, int option, const void *value,
                                 socklen_t length) {
    std::lock_guard<std::mutex> lock(mutex);
    if (fd >= 0 && fd < int(sockets.size()) && sockets[fd].udp && level == SOL_SOCKET &&
        (option == SO_SNDBUF || option == SO_RCVBUF)) {
        if (fail(option == SO_SNDBUF ? "send-buffer" : "receive-buffer"))
            return -1;
        const int bytes = *static_cast<const int *>(value);
        if (option == SO_RCVBUF && receiveLimit && bytes > receiveLimit) {
            errno = ENOBUFS;
            return -1;
        }
        int &old = option == SO_SNDBUF ? sockets[fd].send : sockets[fd].receive;
        if (!fits(old, bytes))
            return -1;
        const int result = __real_setsockopt(fd, level, option, value, length);
        if (result == 0)
            old = bytes;
        return result;
    }
    return __real_setsockopt(fd, level, option, value, length);
}
extern "C" int __real_bind(int, const sockaddr *, socklen_t);
extern "C" int __wrap_bind(int fd, const sockaddr *address, socklen_t length) {
    std::lock_guard<std::mutex> lock(mutex);
    if (fd >= 0 && fd < int(sockets.size()) && sockets[fd].udp && fail("bind"))
        return -1;
    return __real_bind(fd, address, length);
}
extern "C" int __real_getsockname(int, sockaddr *, socklen_t *);
extern "C" int __wrap_getsockname(int fd, sockaddr *address, socklen_t *length) {
    std::lock_guard<std::mutex> lock(mutex);
    if (fd >= 0 && fd < int(sockets.size()) && sockets[fd].udp && fail("name"))
        return -1;
    return __real_getsockname(fd, address, length);
}
extern "C" int __real_close(int);
extern "C" int __wrap_close(int fd) {
    std::lock_guard<std::mutex> lock(mutex);
    if (fd >= 0 && fd < int(sockets.size()))
        sockets[fd] = {};
    return __real_close(fd);
}
