/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned stage, fail_at, acquired, released, time_setup;
static bool known_version;
static char diagnostics[2048];
char *fake_heap_start, *fake_heap_end;
static void check(bool ok, const char *message) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}
static Result init(unsigned bit) {
    ++stage;
    if (stage == fail_at)
        return 0x1234;
    check(!(acquired & bit), "Resource acquired twice");
    acquired |= bit;
    return 0;
}
static void close_resource(unsigned bit) {
    check((acquired & bit) && !(released & bit), "Uninitialized/double resource cleanup");
    released |= bit;
}
Result smInitialize(void) {
    return init(1);
}
void smExit(void) {
    close_resource(1);
}
u32 hosversionGet(void) {
    return known_version ? MAKEHOSVERSION(20, 1, 0) : 0;
}
void hosversionSet(u32 version) {
    check(version == MAKEHOSVERSION(20, 1, 0), "Firmware version lost");
}
Result setsysInitialize(void) {
    return init(2);
}
Result setsysGetFirmwareVersion(SetSysFirmwareVersion *v) {
    *v = (SetSysFirmwareVersion){20, 1, 0};
    ++stage;
    return stage == fail_at ? 0x1234 : 0;
}
void setsysExit(void) {
    close_resource(2);
}
Result fsInitialize(void) {
    return init(4);
}
void fsExit(void) {
    close_resource(4);
}
Result fsdevMountSdmc(void) {
    return init(8);
}
int fsdevUnmountDevice(const char *name) {
    check(!strcmp(name, "sdmc"), "Wrong mount");
    close_resource(8);
    return 0;
}
Result timeInitialize(void) {
    return init(16);
}
void timeExit(void) {
    close_resource(16);
}
void __libnx_init_time(void) {
    check(acquired & 16, "Realtime used before time service");
    ++time_setup;
}
Result socketInitialize(const SocketInitConfig *config) {
    check(config->num_bsd_sessions == 3, "Socket session budget changed");
    return init(32);
}
void socketExit(void) {
    close_resource(32);
}
Result svcOutputDebugString(const char *text, size_t size) {
    check(strlen(diagnostics) + size < sizeof(diagnostics), "Unbounded diagnostics");
    strncat(diagnostics, text, size);
    return 0;
}
#include "../background/source/startup.c"

static void scenario(unsigned failure, bool version, bool writable) {
    fail_at = failure;
    known_version = version;
    char directory[] = "/tmp/cloudtune-startup-XXXXXX";
    check(mkdtemp(directory) && chdir(directory) == 0, "Temporary directory failed");
    if (writable)
        mkdir("sdmc:", 0755);
    __libnx_initheap();
    check(fake_heap_end - fake_heap_start == 16 * 1024 * 1024, "Heap budget changed");
    __appInit();
    check(ncmStartupResult() == (failure ? 0x1234 : 0), "Startup error lost");
    check(stage == (failure ? failure : version ? 5 : 7), "Startup continued after failure");
    check(time_setup == ((acquired & 16) ? 1u : 0u), "Realtime initialization missing");
    check(strstr(diagnostics, "0x00001234") != NULL || !failure, "Failed stage not diagnosed");
    if (writable && (acquired & 8)) {
        FILE *log = fopen(NCM_CONFIG_DIRECTORY "/background-startup.log", "r");
        check(log != NULL, "Startup log missing");
        char contents[2048] = {0};
        fread(contents, 1, sizeof(contents) - 1, log);
        fclose(log);
        check(strstr(contents, "sm: 0x00000000") != NULL, "Early diagnostics missing");
        check(!failure || strstr(contents, "0x00001234"), "Failure missing from SD log");
    }
    __appExit();
    check(acquired == released, "Startup leaked a resource");
    __appExit(); // Cleanup must also be safe when there was nothing to release.
    remove(NCM_CONFIG_DIRECTORY "/background-startup.log");
    rmdir(NCM_CONFIG_DIRECTORY);
    rmdir("sdmc:/config");
    rmdir("sdmc:");
    chdir("/");
    rmdir(directory);
}
int main(void) {
    for (unsigned version = 0; version < 2; ++version)
        for (unsigned writable = 0; writable < 2; ++writable)
            for (unsigned failure = 0; failure <= (version ? 5u : 7u); ++failure) {
                pid_t child = fork();
                check(child >= 0, "fork failed");
                if (!child) {
                    scenario(failure, version, writable);
                    _exit(0);
                }
                int status;
                check(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
                          WEXITSTATUS(status) == 0,
                      "Startup failure injection aborted or failed");
            }
    puts("Startup: all service failures, firmware paths, SD log failures and cleanup passed");
    return 0;
}
