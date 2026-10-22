/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <switch.h>

/* Serialized by the playback client. Only inline CMIF data and one optional
 * moved handle are supported. No caller-owned memory enters a pending IPC.
 * replied is true only for a validated CMIF reply, including service errors. */
Result ncmIpcRequest(Handle session, u32 command, const void* input, size_t input_size,
                    void* output, size_t output_size, Handle* moved_handle,
                    bool send_pid, u64 timeout_ns, bool* replied);
/* Reap a completed late reply, closing any transferred handles. */
bool ncmIpcIdle(void);
/* Keep a timed-out worker's handle alive until its syscall returns. */
void ncmIpcCloseSession(Handle session);
/* Final process shutdown only. Never return to an NRO loader while a blocked
 * worker or the kernel could still access memory recycled by that loader. */
void ncmIpcShutdown(void);
