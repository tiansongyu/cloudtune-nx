/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "netease/playback_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static NcmPlaybackStatus reply;
static NcmAirplayStatus airplay_reply;
static Result reply_result;
SmServiceName smEncodeName(const char* name) { (void)name; return 1; }
Result testServiceCall(Service* service, u32 command, void* data, size_t size) {
    (void)service;
    if (command == NcmCommand_Version) {
        const u32 version = NCM_PROTOCOL_VERSION;
        memcpy(data, &version, sizeof(version));
    } else if (command == NcmCommand_Status && size == sizeof(reply))
        memcpy(data, &reply, size);
    else if (command == NCM_COMMAND_AIRPLAY_STATUS && size == sizeof(airplay_reply))
        memcpy(data, &airplay_reply, size);
    return reply_result;
}
#include "../background/client/playback_client.c"
static void check(bool value, const char* reason) {
    if (!value) { fprintf(stderr, "FAIL: %s\n", reason); exit(1); }
}
int main(void) {
    check(ncmPlaybackInitialize() == 0, "mock service initialization failed");
    NcmPlaybackStatus status;
    memset(&reply, 'x', sizeof(reply));
    reply.version = NCM_PROTOCOL_VERSION;
    check(ncmPlaybackGetStatus(&status) == 0, "valid status rejected");
    check(status.title[sizeof(status.title)-1] == 0 &&
          status.artist[sizeof(status.artist)-1] == 0 &&
          status.message[sizeof(status.message)-1] == 0,
          "fixed-size status strings were not terminated");
    reply.version = NCM_PROTOCOL_VERSION + 1;
    check(R_FAILED(ncmPlaybackGetStatus(&status)), "incompatible status version accepted");
    const NcmPlaybackStatus zero = {0};
    check(memcmp(&status, &zero, sizeof(status)) == 0, "invalid status was exposed");
    reply_result = 1;
    check(R_FAILED(ncmPlaybackGetStatus(&status)), "transport failure swallowed");
    check(memcmp(&status, &zero, sizeof(status)) == 0, "failed status left stale data");
    check(R_FAILED(ncmPlaybackGetStatus(NULL)), "NULL output accepted");
    NcmAirplayStatus airplay;
    memset(&airplay_reply, 'y', sizeof(airplay_reply));
    airplay_reply.version = NCM_AIRPLAY_VERSION;
    reply_result = 0;
    check(ncmAirplayGetStatus(&airplay) == 0, "valid AirPlay status rejected");
    check(!airplay.name[sizeof(airplay.name)-1] && !airplay.address[sizeof(airplay.address)-1] &&
          !airplay.title[sizeof(airplay.title)-1] && !airplay.artist[sizeof(airplay.artist)-1] &&
          !airplay.message[sizeof(airplay.message)-1], "AirPlay strings were not terminated");
    reply_result = 1;
    const NcmAirplayStatus empty_airplay = {0};
    check(R_FAILED(ncmAirplayGetStatus(&airplay)), "AirPlay transport failure swallowed");
    check(memcmp(&airplay, &empty_airplay, sizeof(airplay)) == 0,
          "Failed AirPlay IPC exposed a partial response");
    reply_result = 0;
    airplay_reply.version = NCM_AIRPLAY_VERSION + 1;
    check(R_FAILED(ncmAirplayGetStatus(&airplay)) &&
          memcmp(&airplay, &empty_airplay, sizeof(airplay)) == 0,
          "Invalid AirPlay version exposed a response");
    check(R_FAILED(ncmAirplayGetStatus(NULL)), "NULL AirPlay output accepted");
    ncmPlaybackExit();
    puts("PASS: playback client status version, bounded strings and failure clearing");
    return 0;
}
