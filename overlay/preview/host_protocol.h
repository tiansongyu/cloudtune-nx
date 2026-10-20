#pragma once
/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "netease/playback_protocol.h"
#include <stddef.h>
#include <stdint.h>

/*
 * Linux-only preview transport, independent of Horizon CMIF. One request and
 * one response per AF_UNIX/SOCK_STREAM connection; then the server closes it.
 * All fields use the little-endian host ABI, as does the production queue.
 * Neither messages nor disconnects transfer ownership of the audio engine.
 */
#define NCM_HOST_MAGIC UINT32_C(0x484D434E) /* "NCMH" in little endian */
#define NCM_HOST_VERSION 1u
#define NCM_HOST_SOCKET_NAME "control.sock"
#define NCM_HOST_REQUEST_SIZE 16u
#define NCM_HOST_RESPONSE_SIZE 220u
#define NCM_HOST_SHUTDOWN UINT32_C(0xFFFF0001) /* Test service only. */
#define NCM_HOST_RESULT_OK 0u
#define NCM_HOST_RESULT_PROTOCOL 1u
#define NCM_HOST_RESULT_COMMAND 2u
#define NCM_HOST_RESULT_REJECTED 3u
#define NCM_HOST_RESULT_INTERNAL 4u

typedef struct NcmHostRequest {
    uint32_t magic;    /* NCM_HOST_MAGIC */
    uint32_t version;  /* NCM_HOST_VERSION */
    uint32_t command;  /* NcmCommand or NCM_HOST_SHUTDOWN */
    uint32_t argument;
} NcmHostRequest;

/*
 * Wire bytes 0..215: NcmPlaybackStatus. Bytes 216..219: result.
 * Packed explicitly: a native struct would acquire four bytes of tail padding.
 * Copy status to an aligned NcmPlaybackStatus before reading it in a client.
 * A successful command acknowledges enqueueing; poll Status for its outcome.
 * Version and Ping return the same complete status as Status.
 */
#pragma pack(push, 1)
typedef struct NcmHostResponse {
    NcmPlaybackStatus status;
    uint32_t result;
} NcmHostResponse;
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(NcmHostRequest) == NCM_HOST_REQUEST_SIZE, "Host request ABI changed");
static_assert(sizeof(NcmHostResponse) == NCM_HOST_RESPONSE_SIZE, "Host response ABI changed");
static_assert(offsetof(NcmHostResponse, result) == 216, "Host result offset changed");
#else
_Static_assert(sizeof(NcmHostRequest) == NCM_HOST_REQUEST_SIZE, "Host request ABI changed");
_Static_assert(sizeof(NcmHostResponse) == NCM_HOST_RESPONSE_SIZE, "Host response ABI changed");
_Static_assert(offsetof(NcmHostResponse, result) == 216, "Host result offset changed");
#endif
