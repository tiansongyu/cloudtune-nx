/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *failure;
static bool known_version, home_confirmed, heap_failed, resource_handle;
static unsigned acquired, released, queries, sleeps, home_after;
static size_t committed;
static char diagnostics[32768], disk_log[32768];
static bool stale_ready = true, stale_ipc = true;
static bool process_exited;
void svcExitProcess(void) {
    process_exited = true;
}
static void check(bool ok, const char *message) {
    if (!ok) {
        fprintf(stderr, "FAIL [%s]: %s\n", failure, message);
        exit(1);
    }
}
static bool fails(const char *stage) {
    return !strcmp(failure, stage);
}
static Result init(const char *stage, unsigned bit) {
    if (fails(stage))
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
    return init("sm", 1);
}
void smExit(void) {
    close_resource(1);
}
u32 hosversionGet(void) {
    return known_version ? MAKEHOSVERSION(20, 4, 0) : 0;
}
void hosversionSet(u32 version) {
    check(version == MAKEHOSVERSION(20, 4, 0), "Firmware lost");
}
Result setsysInitialize(void) {
    return init("set:sys", 2);
}
Result setsysGetFirmwareVersion(SetSysFirmwareVersion *v) {
    *v = (SetSysFirmwareVersion){20, 4, 0};
    return fails("firmware") ? 0x1234 : 0;
}
void setsysExit(void) {
    close_resource(2);
}
Result fsInitialize(void) {
    return init("fs", 4);
}
void fsExit(void) {
    close_resource(4);
}
Result fsOpenSdCardFileSystem(FsFileSystem *fs) {
    (void)fs;
    return init("log-sd-open", 8);
}
void fsFsClose(FsFileSystem *fs) {
    (void)fs;
    close_resource(8);
}
Result fsFsCreateDirectory(FsFileSystem *fs, const char *path) {
    (void)fs;
    (void)path;
    return 0;
}
Result fsFsCreateFile(FsFileSystem *fs, const char *path, s64 size, u32 option) {
    (void)fs;
    (void)path;
    (void)size;
    (void)option;
    return 0;
}
Result fsFsDeleteFile(FsFileSystem *fs, const char *path) {
    (void)fs;
    if (strstr(path, "background-ready.log")) stale_ready = false;
    else if (strstr(path, "ipc.log")) stale_ipc = false;
    else check(false, "Unexpected deletion");
    return 0;
}
Result fsFsOpenFile(FsFileSystem *fs, const char *path, u32 mode, FsFile *file) {
    (void)fs;
    (void)file;
    check(!strcmp(path, "/config/netease-switch/background-startup.log"), "Wrong log path");
    check(mode == (FsOpenMode_Write | FsOpenMode_Append), "Log must grow");
    return init("log-open", 16);
}
Result fsFileSetSize(FsFile *file, s64 size) {
    (void)file;
    check(size == 0, "Log not reset");
    return fails("log-truncate") ? 0x1234 : 0;
}
Result fsFileWrite(FsFile *file, s64 offset, const void *data, u64 size, u32 option) {
    (void)file;
    check(option == FsWriteOption_Flush, "Diagnostic not flushed");
    check(offset == (s64)strlen(disk_log), "Log offset lost");
    if (fails("log-write"))
        return 0x1234;
    check(offset + size < sizeof(disk_log), "Log overflow");
    memcpy(disk_log + offset, data, size);
    disk_log[offset + size] = 0;
    return 0;
}
void fsFileClose(FsFile *file) {
    (void)file;
    close_resource(16);
}
Result fsFileFlush(FsFile *file) {
    (void)file;
    check((acquired & 16) && !(released & 16), "Flush of closed log");
    return fails("log-flush") ? 0x1234 : 0;
}
Result pminfoInitialize(void) {
    return init("pm-info", 32);
}
void pminfoExit(void) {
    close_resource(32);
}
Service *pminfoGetServiceSession(void) {
    static Service session;
    return &session;
}
Result stubDispatch(Service *service, u32 command, u64 input, u8 *output) {
    (void)service;
    check(command == 65001 && input == 0x0100000000001000ull, "Unsafe/wrong boot query");
    check((acquired & 32) && !(released & 32), "PM session lifetime");
    check(committed == 0, "Heap allocated before HOME");
    ++queries;
    if (fails("home-query"))
        return 0x1234;
    *output = queries > home_after;
    home_confirmed = *output != 0;
    return 0;
}
Result svcSleepThread(s64 ns) {
    check(ns == 250000000 && committed == 0, "Boot wait allocated/blocked incorrectly");
    ++sleeps;
    return 0;
}
Result fsdevMountSdmc(void) {
    check(home_confirmed, "SD malloc before HOME");
    committed = 2u * 1024u * 1024u;
    return init("sd-mount", 64);
}
int fsdevUnmountDevice(const char *name) {
    check(!strcmp(name, "sdmc"), "Wrong mount");
    close_resource(64);
    return 0;
}
Result timeInitialize(void) {
    check(home_confirmed, "Clock before HOME");
    return init("time", 128);
}
void timeExit(void) {
    close_resource(128);
}
void __libnx_init_time(void) {
    check(acquired & 128, "Realtime used before time service");
}
Result socketInitialize(const SocketInitConfig *config) {
    check(home_confirmed && (acquired & 128), "Sockets before HOME/clock");
    check(config->num_bsd_sessions == 3, "Socket budget changed");
    return init("sockets", 256);
}
void socketExit(void) {
    close_resource(256);
}
Result svcOutputDebugString(const char *text, size_t size) {
    check(strlen(diagnostics) + size < sizeof(diagnostics), "Diagnostics overflow");
    strncat(diagnostics, text, size);
    return 0;
}
Result ncmHeapStartupResult(void) {
    return heap_failed ? 0x10801 : 0;
}
size_t ncmHeapCommittedSize(void) {
    return committed;
}
Result svcGetInfo(u64 *out, u32 type, Handle handle, u64 sub) {
    check(type == InfoType_ResourceLimit && handle == INVALID_HANDLE && sub == 0,
          "Wrong resource handle query (must use INVALID_HANDLE)");
    if (fails("resource-handle"))
        return 0x1234;
    check(!resource_handle, "Resource handle leaked");
    resource_handle = true;
    *out = 99;
    return 0;
}
Result svcGetResourceLimitCurrentValue(s64 *out, Handle handle, LimitableResource resource) {
    check(resource_handle && handle == 99 && resource <= LimitableResource_Sessions,
          "Wrong resource");
    *out = resource == LimitableResource_Memory ? 200000000 : 100;
    const bool low = !strncmp(failure, "low-", 4) && failure[4] - '0' == (int)resource;
    const bool recovering =
        !strncmp(failure, "recover-", 8) && failure[8] - '0' == (int)resource && sleeps < 4;
    if (low || recovering)
        *out = resource == LimitableResource_Memory ? 249999999 : 200;
    if (resource == LimitableResource_Memory) {
        if (fails("compact") || (fails("compact-switch") && sleeps >= 8) ||
            (fails("compact-final") && sleeps >= 20))
            *out = 250000000 - 5431296;
        if (fails("compact-short"))
            *out = 250000000 - 5u * 1024u * 1024u + 1;
        if (fails("normal-switch") && sleeps < 8)
            *out = 250000000 - 5668864;
    }
    return fails("resource-current") ? 0x1234 : 0;
}
Result svcGetResourceLimitLimitValue(s64 *out, Handle handle, LimitableResource resource) {
    check(resource_handle && handle == 99, "Missing resource handle");
    *out = resource == LimitableResource_Memory ? 250000000 : 200;
    return fails("resource-limit") ? 0x1234 : 0;
}
Result svcCloseHandle(Handle handle) {
    check(resource_handle && handle == 99, "Wrong handle close");
    resource_handle = false;
    return 0;
}
#include "../background/source/boot.c"
#include "../background/source/boot_query.c"
#include "../background/source/resources.c"
#include "../background/source/startup.c"

static void scenario(const char *fail, bool known, unsigned delay) {
    failure = fail;
    known_version = known;
    home_after = delay;
    heap_failed = fails("heap");
    __appInit();
    const bool diagnostic_failure = !strncmp(fail, "log-", 4);
    const bool recovering = !strncmp(fail, "recover-", 8);
    const bool compact = !strncmp(fail, "compact", 7) && !fails("compact-short");
    const bool success =
        (!*fail || diagnostic_failure || recovering || compact || fails("normal-switch")) &&
        delay < 480;
    check(R_SUCCEEDED(ncmStartupResult()) == success, "Wrong startup outcome");
    if (compact) {
        check(ncmMemoryHeapLimit() == 2u * 1024u * 1024u &&
                  ncmSystemMemoryMargin() == 3u * 1024u * 1024u,
              "Compact policy missing");
        check(strstr(disk_log, "memory-policy: compact"), "Compact diagnostics missing");
    }
    if (success && !compact)
        check(ncmMemoryHeapLimit() == 16u * 1024u * 1024u, "Normal budget regressed");
    if (fails("compact-switch") || fails("normal-switch"))
        check(sleeps == 28, "Policy change did not restart continuous observation");
    if (fails("compact-final"))
        check(sleeps == 40, "Final recheck ignored a policy change");
    if (fails("compact-short"))
        check(sleeps == 240, "Compact shortage timeout changed");
    check(!resource_handle, "Diagnostic handle leaked");
    check(success == ((acquired & 256) != 0), "Failed gate still started playback services");
    check(process_exited == !success, "Failed startup would still run C++ global constructors");
    if (queries) {
        if (success)
            check(sleeps >= delay + 20, "Missing post-HOME resource observation");
        if (recovering)
            check(sleeps >= 24, "Shortage did not reset healthy observation");
        if (!strncmp(fail, "low-", 4))
            check(sleeps == 240, "Resource wait was not bounded");
        if (delay >= 480)
            check(sleeps == 480, "HOME timeout was not bounded");
        if (!fails("home-query") && delay < 480)
            check(queries == delay + 1, "Unexpected polling");
    }
    if (log_ready && !fails("log-write")) {
        check(strstr(disk_log, "CloudTune NX") && strstr(disk_log, "sm: 0x00000000"),
              "Early log lost");
        check(!stale_ready && !stale_ipc, "Stale ready/IPC marker retained");
        if (queries)
            check(strstr(disk_log, "before-home"), "Pre-boot resource snapshot missing");
        if (heap_failed)
            check(strstr(disk_log, "heap-address: 0x00010801"), "Heap error lost");
    }
    __appExit();
    check(acquired == released, "Startup leaked resources");
    __appExit();
}
int main(void) {
    const char *failures[] = {"",
                              "sm",
                              "set:sys",
                              "firmware",
                              "fs",
                              "log-sd-open",
                              "log-open",
                              "log-truncate",
                              "log-write",
                              "log-flush",
                              "heap",
                              "pm-info",
                              "home-query",
                              "sd-mount",
                              "time",
                              "sockets",
                              "resource-handle",
                              "resource-current",
                              "resource-limit",
                              "compact",
                              "compact-switch",
                              "compact-final",
                              "compact-short",
                              "normal-switch"};
    const unsigned count = sizeof(failures) / sizeof(*failures);
    for (unsigned i = 0; i < count + 13; ++i) {
        pid_t child = fork();
        check(child >= 0, "fork failed");
        if (!child) {
            if (i < count)
                scenario(failures[i], false, 0);
            else if (i < count + 3)
                scenario("", true, i == count ? 3 : i == count + 1 ? 480 : 0);
            else {
                char name[20];
                snprintf(name, sizeof(name), "%s-%u", i < count + 8 ? "low" : "recover",
                         (i - count - 3) % 5);
                scenario(name, true, 0);
            }
            _exit(0);
        }
        int status;
        check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "Startup scenario failed");
    }
    puts("Startup: HOME gating, no eager allocation, timeout/query failures, raw flushed SD log, "
         "resource diagnostics and cleanup passed");
    return 0;
}
