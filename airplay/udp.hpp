/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <cerrno>
#include <sys/socket.h>

namespace cloudtune::airplay {
// Horizon shares a bounded BSD transfer pool between discovery, RTSP and all
// three RAOP sockets. Reserve burst capacity for audio, not the timing socket.
constexpr int UdpSendBytes = 4 * 1024;
constexpr int UdpAudioBytes = 64 * 1024;
constexpr int UdpControlBytes = 16 * 1024;
constexpr int UdpTimingBytes = 4 * 1024;
constexpr int UdpDiscoveryBytes = 8 * 1024;

// Return the failing operation, or nullptr on success. A smaller receive
// queue is usable under temporary pressure; never silently ignore a failure.
inline const char *configureUdpBuffers(int fd, int &receive, int minimum) {
    const int send = UdpSendBytes;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &send, sizeof(send)) != 0)
        return "udp-send-buffer-error";
    for (;;) {
        if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive, sizeof(receive)) == 0)
            return nullptr;
        if ((errno != ENOBUFS && errno != ENOMEM) || receive <= minimum)
            return "udp-receive-buffer-error";
        receive /= 2;
        if (receive < minimum)
            receive = minimum;
    }
}
} // namespace cloudtune::airplay
