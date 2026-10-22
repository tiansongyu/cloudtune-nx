/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint32_t Result;
typedef uint32_t u32;
typedef uint8_t u8;
typedef uint64_t u64;
typedef int64_t s64;
typedef uint32_t Handle;
typedef unsigned Mutex;
#define R_FAILED(r) ((r) != 0)
#define R_SUCCEEDED(r) ((r) == 0)
#define MAKERESULT(m, d) ((m) | ((d) << 9))
#define INVALID_HANDLE 0
enum { Module_Kernel = 1, Module_Libnx = 345, LibnxError_Timeout = 8, LibnxError_BadInput = 11 };
#define MAKEHOSVERSION(a, b, c) (((a) << 16) | ((b) << 8) | (c))
enum { AppletType_None, BsdServiceType_User };
typedef struct {
    uint8_t major, minor, micro;
} SetSysFirmwareVersion;
typedef struct {
    size_t tcp_tx_buf_size, tcp_rx_buf_size, tcp_tx_buf_max_size, tcp_rx_buf_max_size;
    size_t udp_tx_buf_size, udp_rx_buf_size, sb_efficiency;
    unsigned num_bsd_sessions, bsd_service_type;
} SocketInitConfig;
Result smInitialize(void);
void smExit(void);
u32 hosversionGet(void);
void hosversionSet(u32 version);
Result setsysInitialize(void);
Result setsysGetFirmwareVersion(SetSysFirmwareVersion *version);
void setsysExit(void);
Result fsInitialize(void);
void fsExit(void);
Result fsdevMountSdmc(void);
int fsdevUnmountDevice(const char *name);
Result timeInitialize(void);
void timeExit(void);
Result socketInitialize(const SocketInitConfig *config);
void socketExit(void);
Result svcOutputDebugString(const char *text, size_t size);
Result svcSetHeapSize(void **address, size_t size);
void mutexLock(Mutex *mutex);
void mutexUnlock(Mutex *mutex);
typedef struct {
    unsigned id;
} FsFileSystem;
typedef struct {
    unsigned id;
} FsFile;
typedef struct {
    unsigned id;
} Service;
enum { FsOpenMode_Write = 2, FsOpenMode_Append = 4, FsWriteOption_Flush = 1 };
Result fsOpenSdCardFileSystem(FsFileSystem *fs);
Result fsFsCreateDirectory(FsFileSystem *fs, const char *path);
Result fsFsCreateFile(FsFileSystem *fs, const char *path, s64 size, u32 option);
Result fsFsDeleteFile(FsFileSystem *fs, const char *path);
Result fsFsOpenFile(FsFileSystem *fs, const char *path, u32 mode, FsFile *file);
Result fsFileSetSize(FsFile *file, s64 size);
Result fsFileFlush(FsFile *file);
Result fsFileWrite(FsFile *file, s64 offset, const void *data, u64 size, u32 option);
void fsFileClose(FsFile *file);
void fsFsClose(FsFileSystem *fs);
Result pminfoInitialize(void);
void pminfoExit(void);
Service *pminfoGetServiceSession(void);
Result stubDispatch(Service *service, u32 command, u64 input, u8 *output);
#define serviceDispatchInOut(s, c, i, o) stubDispatch(s, c, i, &(o))
Result svcSleepThread(s64 ns);
void svcExitProcess(void);
enum { InfoType_ResourceLimit = 9 };
typedef enum {
    LimitableResource_Memory,
    LimitableResource_Threads,
    LimitableResource_Events,
    LimitableResource_TransferMemories,
    LimitableResource_Sessions
} LimitableResource;
Result svcGetInfo(u64 *out, u32 type, Handle handle, u64 sub);
Result svcGetResourceLimitLimitValue(s64 *out, Handle handle, LimitableResource resource);
Result svcGetResourceLimitCurrentValue(s64 *out, Handle handle, LimitableResource resource);
Result svcCloseHandle(Handle handle);
