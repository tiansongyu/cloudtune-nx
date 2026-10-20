#pragma once
/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NCM_SERVICE_NAME "ncm:bg"
#define NCM_PROGRAM_ID UINT64_C(0x42000000004E434D)
#define NCM_PROTOCOL_VERSION 1u
#define NCM_MAX_QUEUE 512u
#define NCM_CONFIG_DIRECTORY "sdmc:/config/netease-switch"
#define NCM_QUEUE_PATH NCM_CONFIG_DIRECTORY "/queue.nsq"

typedef enum NcmCommand {
    NcmCommand_Version = 0, NcmCommand_Status = 1, NcmCommand_Ping = 2,
    NcmCommand_LoadQueue = 10, NcmCommand_PlayPause = 11, NcmCommand_Play = 12,
    NcmCommand_Pause = 13, NcmCommand_Next = 14, NcmCommand_Previous = 15,
    NcmCommand_Volume = 16, NcmCommand_Seek = 17, NcmCommand_Repeat = 18,
    NcmCommand_Shuffle = 19, NcmCommand_Stop = 20
} NcmCommand;
typedef enum NcmPlaybackState {
    NcmPlayback_Stopped = 0, NcmPlayback_Buffering = 1,
    NcmPlayback_Playing = 2, NcmPlayback_Paused = 3, NcmPlayback_Error = 4
} NcmPlaybackState;
typedef enum NcmRepeatMode {
    NcmRepeat_Off = 0, NcmRepeat_One = 1, NcmRepeat_All = 2
} NcmRepeatMode;

/* CMIF inline status fits the 0x100-byte TLS command buffer (216 bytes). */
typedef struct NcmPlaybackStatus {
    uint32_t version;
    uint32_t state;
    uint64_t track_id;
    uint32_t position_ms;
    uint32_t duration_ms;
    uint32_t volume_permille;
    uint32_t queue_index;
    uint32_t queue_count;
    uint32_t repeat;
    uint32_t shuffle;
    uint32_t error_code;
    char title[64];
    char artist[48];
    char message[56];
} NcmPlaybackStatus;

/* Queue format is little endian on both the Switch and supported host builds.
 * Credentials are deliberately excluded; URL refresh reads the cookie jar. */
typedef struct NcmQueueHeader {
    char magic[8]; /* "NCMQUEUE" */
    uint32_t version;
    uint32_t count;
} NcmQueueHeader;
typedef struct NcmQueueEntry {
    uint64_t id;
    uint32_t duration_ms;
    uint32_t reserved;
    char title[128];
    char artist[128];
    char album[128];
    char url[1024]; /* HTTPS URL, or sdmc:/ local cached MP3 */
} NcmQueueEntry;

/* Publishes path.tmp. POSIX replacement is atomic; Horizon uses a recoverable
 * path.bak during replacement. 0 succeeds; -1 fails with errno preserved. */
int ncmWriteQueueFile(const char* path, const NcmQueueEntry* entries, uint32_t count);

#ifdef __cplusplus
}
static_assert(sizeof(NcmPlaybackStatus) == 216, "IPC status must remain 216 bytes");
static_assert(sizeof(NcmQueueEntry) == 1424, "Queue entry ABI changed");
#endif
