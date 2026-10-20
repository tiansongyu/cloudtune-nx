/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
extern "C" {
#include <libavutil/mem.h>
}
namespace cloudtune::audio {
inline constexpr size_t MaximumAllocation = 2 * 1024 * 1024;
inline void configure_limits() {
    /* FFmpeg's bound is process-wide. Initialize once before either AAC or
     * ALAC can allocate, never change it while another decoder is running. */
    static const bool configured = [] { av_max_alloc(MaximumAllocation); return true; }();
    (void)configured;
}
}
