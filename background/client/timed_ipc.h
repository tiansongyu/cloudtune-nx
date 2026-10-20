/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <switch.h>

/* Serialized by the playback client. Only inline CMIF data and one optional
 * moved handle are supported. No caller-owned memory enters a pending IPC. */
Result ncmIpcRequest(Handle session, u32 command, const void* input, size_t input_size,
                    void* output, size_t output_size, Handle* moved_handle,
                    bool send_pid, u64 timeout_ns);
/* Reap a completed late reply, closing any transferred handles. */
bool ncmIpcIdle(void);
/* Final process shutdown only. Never return to an NRO loader with a buffer
 * that the kernel could still write into after that loader reuses the heap. */
void ncmIpcShutdown(void);
