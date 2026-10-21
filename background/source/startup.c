/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "startup.h"
#include "netease/playback_protocol.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

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
/* __appInit runs before C++ constructors. Keep this path allocation-free except
 * for libnx/stdio, and retain early diagnostics until SD becomes available. */
static char early_log[1024];
static size_t early_size;

void __libnx_initheap(void) {
    /* Runtime allocations and worker stacks share 16 MiB. The package verifier
     * also bounds the image's entire static footprint, including codec BSS. */
    static char heap[16 * 1024 * 1024];
    extern char *fake_heap_start;
    extern char *fake_heap_end;
    fake_heap_start = heap;
    fake_heap_end = heap + sizeof(heap);
}

void ncmStartupRecord(const char *stage, Result result) {
    char line[160];
    snprintf(line, sizeof(line), "%s: 0x%08lx\n", stage, (unsigned long)result);
    const size_t size = strlen(line);
    svcOutputDebugString(line, size);
    if (sd_ready) {
        FILE *file = fopen(NCM_CONFIG_DIRECTORY "/background-startup.log", "a");
        if (file) {
            fputs(line, file);
            fclose(file);
        }
    } else if (size < sizeof(early_log) - early_size) {
        memcpy(early_log + early_size, line, size + 1);
        early_size += size;
    }
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
    if (!initialized("sd-mount", fsdevMountSdmc(), &sd_ready))
        return;
    mkdir("sdmc:/config", 0755);
    mkdir(NCM_CONFIG_DIRECTORY, 0755);
    FILE *log = fopen(NCM_CONFIG_DIRECTORY "/background-startup.log", "w");
    if (log) {
        fputs("CloudTune NX startup\n", log);
        fputs(early_log, log);
        fclose(log);
    }
    /* A ready marker belongs to this process, never to an older successful run. */
    remove(NCM_CONFIG_DIRECTORY "/background-ready.log");
    if (!initialized("time", timeInitialize(), &time_ready))
        return;
    __libnx_init_time();
    initialized("sockets", socketInitialize(&sockets), &sockets_ready);
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
    if (fs_ready) {
        fs_ready = false;
        fsExit();
    }
    if (sm_ready) {
        sm_ready = false;
        smExit();
    }
}
