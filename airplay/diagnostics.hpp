#pragma once
/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <sys/stat.h>

namespace cloudtune::airplay {
// Sparse stage/counter diagnostics only. Callers must use fixed event names,
// never sender headers, metadata, keys, addresses or audio data. Each path has
// one writer; no diagnostic failure may interrupt playback or replace errno.
inline void diagnostic(const std::string &path, const char *event, uint64_t a = 0,
                       uint64_t b = 0) noexcept {
    if (path.empty())
        return;
    const int saved = errno;
    FILE *file = std::fopen(path.c_str(), "ab");
    if (file) {
        struct stat info{};
        if (fstat(fileno(file), &info) == 0 && info.st_size > 32 * 1024) {
            std::fclose(file);
            file = std::fopen(path.c_str(), "wb");
        }
        if (file) {
            std::setvbuf(file, nullptr, _IONBF, 0);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
            std::fprintf(file, "%lld %s %llu %llu\n", static_cast<long long>(ms), event,
                         static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
            std::fclose(file);
        }
    }
    errno = saved;
}
} // namespace cloudtune::airplay
