#pragma once
/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
#include "netease/playback_protocol.h"
#include "netease/airplay_protocol.h"
#include <switch.h>
#ifdef __cplusplus
extern "C" {
#endif
Result ncmPlaybackInitialize(void);
/* Last connection failure, including its handshake stage. This reports
 * historical startup diagnostics as such; a file is never a liveness check. */
void ncmPlaybackConnectionMessage(char* output, size_t size);
/* PM lookup distinguishes a live, still-starting process from an absent IPC
 * service. MAKERESULT(15, 1) means no such process; other failures are unknown. */
Result ncmPlaybackGetBackgroundProcessId(u64* pid);
void ncmPlaybackExit(void);
/* Final exit only, after all client users stop. May terminate the process if
 * an IPC worker is still blocked, preventing unsafe NRO reload. */
void ncmPlaybackShutdown(void);
bool ncmPlaybackIsActive(void);
Result ncmPlaybackGetStatus(NcmPlaybackStatus* status);
/* LoadQueue starts playback at index after reading NCM_QUEUE_PATH. */
Result ncmPlaybackLoadQueue(uint32_t index);
Result ncmPlaybackPlayPause(void);
Result ncmPlaybackPlay(void);
Result ncmPlaybackPause(void);
Result ncmPlaybackNext(void);
Result ncmPlaybackPrevious(void);
Result ncmPlaybackSetVolume(uint32_t permille);
Result ncmPlaybackSeek(uint32_t milliseconds);
Result ncmPlaybackSetRepeat(NcmRepeatMode mode);
Result ncmPlaybackSetShuffle(bool enabled);
Result ncmPlaybackStop(void);
Result ncmPlaybackClearQueue(void);
/* Separate tool commands leave NcmPlaybackStatus and the overlay ABI intact. */
Result ncmAirplayGetStatus(NcmAirplayStatus* status);
Result ncmAirplaySetEnabled(bool enabled);
Result ncmAirplayDisconnect(void);
/* Launch the installed Atmosphere program if it has not booted yet. */
Result ncmPlaybackLaunchBackground(void);
#ifdef __cplusplus
}
#endif
