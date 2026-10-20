#pragma once
/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdint.h>

#define NCM_AIRPLAY_VERSION 1u
/* Independent commands preserve the original playback and overlay ABI. */
#define NCM_COMMAND_AIRPLAY_STATUS 30u
#define NCM_COMMAND_AIRPLAY_ENABLED 31u
#define NCM_COMMAND_AIRPLAY_DISCONNECT 32u

typedef enum NcmAirplayState {
    NcmAirplay_Disabled = 0, NcmAirplay_Starting = 1, NcmAirplay_Ready = 2,
    NcmAirplay_Connected = 3, NcmAirplay_Playing = 4, NcmAirplay_Error = 5
} NcmAirplayState;

typedef struct NcmAirplayStatus {
    uint32_t version;
    uint32_t enabled; /* User preference; not a claim that the listener is ready. */
    uint32_t state;
    uint32_t port;
    uint32_t received_packets;
    uint32_t lost_packets;
    uint32_t buffered_frames;
    uint32_t error_code;
    char name[48];
    char address[32];
    char title[48];
    char artist[24];
    char message[32];
} NcmAirplayStatus;

#ifdef __cplusplus
static_assert(sizeof(NcmAirplayStatus) == 216, "AirPlay status must fit CMIF inline output");
#endif
