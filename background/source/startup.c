/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "startup.h"
#include "boot.h"
#include "heap.h"
#include "netease/playback_protocol.h"
#include <stdio.h>
#include <string.h>

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 2;
void __libnx_init_time(void);

static const SocketInitConfig sockets = {
    .tcp_tx_buf_size = 0x800,
    .tcp_rx_buf_size = 0x1000,
    .tcp_tx_buf_max_size = 0x4000,
    .tcp_rx_buf_max_size = 0x8000,
    .udp_tx_buf_size = 64 * 1024,
    .udp_rx_buf_size = 64 * 1024,
    .sb_efficiency = 2,
    .num_bsd_sessions = 3,
    .bsd_service_type = BsdServiceType_User,
};
static Result startup_result;
static bool sm_ready, fs_ready, sd_ready, time_ready, sockets_ready;
static bool log_fs_ready, log_ready;
static FsFileSystem log_fs;
static FsFile log_file;
static s64 log_offset;
/* Raw FS IPC avoids fsdev's malloc and stdio buffers before HOME is launched.
 * Flush each line so a system fatal leaves usable evidence on the SD card. */
#define LOG_DIRECTORY "/config/netease-switch"
#define LOG_PATH LOG_DIRECTORY "/background-startup.log"
#ifndef CLOUDTUNE_VERSION
#define CLOUDTUNE_VERSION "development"
#endif
static char early_log[1024];
static size_t early_size;

void ncmStartupWrite(const char *line) {
    const size_t size = strlen(line);
    svcOutputDebugString(line, size);
    if (log_ready) {
        Result result = fsFileWrite(&log_file, log_offset, line, size, FsWriteOption_Flush);
        if (R_SUCCEEDED(result)) {
            log_offset += (s64)size;
            fsFileFlush(&log_file);
        }
    } else if (size < sizeof(early_log) - early_size) {
        memcpy(early_log + early_size, line, size + 1);
        early_size += size;
    }
}

void ncmStartupRecord(const char *stage, Result result) {
    char line[160];
    snprintf(line, sizeof(line), "%s: 0x%08lx\n", stage, (unsigned long)result);
    ncmStartupWrite(line);
}

static void open_log(void) {
    Result result = fsOpenSdCardFileSystem(&log_fs);
    ncmStartupRecord("log-sd-open", result);
    if (R_FAILED(result))
        return;
    log_fs_ready = true;
    fsFsCreateDirectory(&log_fs, "/config");
    fsFsCreateDirectory(&log_fs, LOG_DIRECTORY);
    fsFsDeleteFile(&log_fs, LOG_DIRECTORY "/background-ready.log");
    /* Create if absent, then truncate an existing file after opening it. */
    fsFsCreateFile(&log_fs, LOG_PATH, 0, 0);
    result = fsFsOpenFile(&log_fs, LOG_PATH, FsOpenMode_Write | FsOpenMode_Append, &log_file);
    ncmStartupRecord("log-open", result);
    if (R_FAILED(result))
        return;
    result = fsFileSetSize(&log_file, 0);
    if (R_FAILED(result)) {
        fsFileClose(&log_file);
        ncmStartupRecord("log-truncate", result);
        return;
    }
    log_ready = true;
    ncmStartupWrite("CloudTune NX " CLOUDTUNE_VERSION " startup\n");
    ncmStartupWrite(early_log);
    early_size = 0;
}

static bool initialized(const char *stage, Result result, bool *ready) {
    ncmStartupRecord(stage, result);
    if (R_FAILED(result)) {
        startup_result = result;
        return false;
    }
    if (ready)
        *ready = true;
    return true;
}

Result ncmStartupResult(void) {
    return startup_result;
}

void __appInit(void) {
    if (!initialized("sm", smInitialize(), &sm_ready))
        return;
    if (!hosversionGet()) {
        if (!initialized("set:sys", setsysInitialize(), NULL))
            return;
        SetSysFirmwareVersion firmware = {0};
        Result rc = setsysGetFirmwareVersion(&firmware);
        setsysExit();
        if (!initialized("firmware", rc, NULL))
            return;
        hosversionSet(MAKEHOSVERSION(firmware.major, firmware.minor, firmware.micro));
    }
    if (!initialized("fs", fsInitialize(), &fs_ready))
        return;
    open_log();
    if (!initialized("heap-address", ncmHeapStartupResult(), NULL))
        return;
    ncmStartupRecordResources("before-home");
    if (!initialized("boot-gate", ncmWaitForHomeMenu(), NULL))
        return;
    ncmStartupRecordResources("after-home");
    if (!initialized("sd-mount", fsdevMountSdmc(), &sd_ready))
        return;
    if (!initialized("time", timeInitialize(), &time_ready))
        return;
    __libnx_init_time();
    ncmStartupRecordResources("before-sockets");
    initialized("sockets", socketInitialize(&sockets), &sockets_ready);
    ncmStartupRecordResources("after-sockets");
}

void __appExit(void) {
    /* A failed initialization is an ordinary process exit, not a fatal report.
     * Only release resources acquired by this attempt, in reverse order. */
    if (sockets_ready) {
        sockets_ready = false;
        socketExit();
    }
    if (time_ready) {
        time_ready = false;
        timeExit();
    }
    if (sd_ready) {
        sd_ready = false;
        fsdevUnmountDevice("sdmc");
    }
    if (log_ready) {
        log_ready = false;
        fsFileClose(&log_file);
    }
    if (log_fs_ready) {
        log_fs_ready = false;
        fsFsClose(&log_fs);
    }
    if (fs_ready) {
        fs_ready = false;
        fsExit();
    }
    if (sm_ready) {
        sm_ready = false;
        smExit();
    }
}
