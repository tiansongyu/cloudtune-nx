/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay/mdns.hpp"
#include "airplay/receiver.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <poll.h>
#include <sys/socket.h>
#include <thread>

static thread_local bool denyAllocations;
static std::atomic<int> openSockets{0};
static std::atomic<bool> failWorker{false};
extern "C" void *__real__Znwm(size_t);
extern "C" void *__wrap__Znwm(size_t size) {
    if (denyAllocations)
        throw std::bad_alloc();
    return __real__Znwm(size);
}
// Exercise the real worker and cleanup without emitting network traffic.
extern "C" int __wrap_socket(int, int, int) {
    return 40 + ++openSockets;
}
extern "C" int __wrap_bind(int, const sockaddr *, socklen_t) {
    return 0;
}
extern "C" int __wrap_setsockopt(int, int, int, const void *, socklen_t) {
    return 0;
}
extern "C" int __wrap_fcntl(int, int, ...) {
    return 0;
}
extern "C" int __wrap_listen(int, int) {
    return 0;
}
extern "C" ssize_t __wrap_sendto(int, const void *, size_t size, int, const sockaddr *, socklen_t) {
    return static_cast<ssize_t>(size);
}
extern "C" int __wrap_close(int) {
    --openSockets;
    return 0;
}
extern "C" int __wrap_poll(pollfd *, nfds_t, int) {
    if (failWorker) {
        denyAllocations = true;
        errno = EIO;
        return -1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return 0;
}
static void check(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
int main() {
    {
        cloudtune::airplay::MdnsAdvertiser advertiser;
        check(advertiser.start("OOM Probe", 5000, "020000000001", htonl(INADDR_LOOPBACK)), "start");
        denyAllocations = true;
        advertiser.stop();
        check(advertiser.fd() == -1 && openSockets == 0, "OOM stop leaked socket");
        advertiser.stop();
        denyAllocations = false;
    }
    {
        auto *advertiser = new cloudtune::airplay::MdnsAdvertiser;
        check(advertiser->start("OOM Probe", 5000, "020000000001", htonl(INADDR_LOOPBACK)),
              "restart");
        denyAllocations = true;
        delete advertiser; // Previously invokes std::terminate.
        denyAllocations = false;
        check(openSockets == 0, "OOM destructor leaked socket");
    }
    {
        cloudtune::airplay::Config config;
        config.address = "127.0.0.1";
        cloudtune::airplay::Receiver receiver(config, {});
        check(receiver.start(), "receiver start");
        check(openSockets == 2, "listener and advertiser must both be exercised");
        failWorker = true;
        for (unsigned i = 0; i < 500 && openSockets; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        check(receiver.status().state == NcmAirplay_Error, "OOM worker did not report error");
        receiver.stop();
        check(openSockets == 0, "OOM worker cleanup leaked sockets");
        failWorker = false;
        check(receiver.start(), "receiver cannot restart after OOM");
        receiver.stop();
    }
    check(openSockets == 0, "final socket leak");
    std::puts("PASS: OOM stop/destructor/worker report and cleanup, idempotence and restart");
}
