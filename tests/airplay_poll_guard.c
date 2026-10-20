/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Eden rejects negative poll descriptors, although POSIX ignores them.
 * Preload this guard during the receiver wire suite to exercise that contract.
 */
#define _GNU_SOURCE
/* glibc annotates poll's array write-only; this interposer reads its fd fields. */
#define poll libc_poll_declaration
#include <poll.h>
#undef poll
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

int poll(struct pollfd *fds, nfds_t count, int timeout) {
    for (nfds_t i = 0; i < count; ++i) {
        if (fds[i].fd < 0) {
            static const char error[] = "AirPlay regression: negative descriptor submitted to poll\n";
            (void)write(STDERR_FILENO, error, sizeof(error) - 1);
            _exit(90);
        }
    }
#ifdef SYS_poll
    return (int)syscall(SYS_poll, fds, count, timeout);
#else
    /* Linux AArch64 provides ppoll instead of the legacy poll syscall. */
    struct timespec deadline = {timeout / 1000, (timeout % 1000) * 1000000L};
    return (int)syscall(SYS_ppoll, fds, count, timeout < 0 ? NULL : &deadline, NULL, 0);
#endif
}
