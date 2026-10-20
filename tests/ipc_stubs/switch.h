/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
/* Host fixture for the fixed-width HIPC layouts in libnx's sf/hipc.h.
 * It models wire offsets only; no Horizon kernel behavior is simulated. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef uint32_t Result;
typedef uint32_t Handle;
typedef uint64_t SmServiceName;
#define MAX_WAIT_OBJECTS 64
#define INVALID_HANDLE 0
#define Module_Libnx 345
enum { LibnxError_OutOfMemory=1, LibnxError_BadInput, LibnxError_NotFound,
       LibnxError_IncompatSysVer, LibnxError_Timeout };
#define MAKERESULT(module, error) ((Result)((module) | ((error) << 9)))
#define R_FAILED(value) ((value) != 0)
#define R_SUCCEEDED(value) ((value) == 0)
#define KERNELRESULT(name) KernelResult_##name
enum { KernelResult_TimedOut=0xea01, KernelResult_ConnectionClosed=0xf601,
       KernelResult_Cancelled=0xec01 };
#define CMIF_IN_HEADER_MAGIC 0x49434653u
#define CMIF_OUT_HEADER_MAGIC 0x4f434653u
enum { CmifCommandType_Close=2, CmifCommandType_Request=4, CmifCommandType_Control=5 };
typedef struct {
    u32 type:16, num_send_statics:4, num_send_buffers:4, num_recv_buffers:4, num_exch_buffers:4;
    u32 num_data_words:10, recv_static_mode:4, padding:6, recv_list_offset:11, has_special_header:1;
} HipcHeader;
typedef struct { u32 send_pid:1, num_copy_handles:4, num_move_handles:4, padding:23; } HipcSpecialHeader;
typedef struct {
    u32 type, num_send_statics, num_send_buffers, num_recv_buffers, num_exch_buffers;
    u32 num_data_words, num_recv_statics, send_pid, num_copy_handles, num_move_handles;
} HipcMetadata;
typedef struct {
    u32* data_words;
    Handle* copy_handles;
    Handle* move_handles;
} HipcRequest;
typedef struct { HipcMetadata meta; HipcRequest data; u64 pid; } HipcParsedRequest;
typedef struct { u32 magic, version, result, token; } CmifOutHeader;
static inline HipcParsedRequest hipcParseRequest(void* data) {
    HipcHeader header; memcpy(&header, data, sizeof(header));
    HipcParsedRequest parsed = {0};
    parsed.meta.type = header.type;
    parsed.meta.num_data_words = header.num_data_words;
    parsed.meta.num_send_statics = header.num_send_statics;
    parsed.meta.num_send_buffers = header.num_send_buffers;
    parsed.meta.num_recv_buffers = header.num_recv_buffers;
    parsed.meta.num_exch_buffers = header.num_exch_buffers;
    parsed.meta.num_recv_statics = header.recv_static_mode;
    uintptr_t cursor = (uintptr_t)data + sizeof(header);
    if (header.has_special_header) {
        HipcSpecialHeader special; memcpy(&special, (void*)cursor, sizeof(special));
        cursor += sizeof(special) + (special.send_pid ? 8 : 0);
        parsed.meta.send_pid = special.send_pid;
        parsed.meta.num_copy_handles = special.num_copy_handles;
        parsed.meta.num_move_handles = special.num_move_handles;
        parsed.data.copy_handles = (Handle*)cursor;
        cursor += special.num_copy_handles * sizeof(Handle);
        parsed.data.move_handles = (Handle*)cursor;
        cursor += special.num_move_handles * sizeof(Handle);
    }
    cursor += header.num_send_statics * 8;
    cursor += (header.num_send_buffers + header.num_recv_buffers + header.num_exch_buffers) * 12;
    parsed.data.data_words = header.num_data_words ? (u32*)cursor : NULL;
    return parsed;
}
static inline void* cmifGetAlignedDataStart(u32* words, void* base) {
    return (void*)((uintptr_t)base + (((uintptr_t)words - (uintptr_t)base + 15) & ~(uintptr_t)15));
}
static inline HipcRequest hipcMakeRequest(void* base, HipcMetadata metadata) {
    HipcHeader header = {0};
    header.type = metadata.type;
    header.num_data_words = metadata.num_data_words;
    memcpy(base, &header, sizeof(header));
    return (HipcRequest){.data_words=(u32*)((u8*)base+sizeof(header))};
}
#define hipcMakeRequestInline(base, ...) hipcMakeRequest(base, (HipcMetadata){ __VA_ARGS__ })
typedef struct { u32 request_id, data_size, send_pid; } CmifRequestFormat;
typedef struct { void* data; } CmifRequest;
static inline CmifRequest cmifMakeRequest(void* base, CmifRequestFormat format) {
    HipcHeader header = {0};
    header.type = CmifCommandType_Request;
    header.num_data_words = (32 + format.data_size + 3) / 4;
    header.has_special_header = format.send_pid != 0;
    memcpy(base, &header, sizeof(header));
    size_t offset = sizeof(header);
    if (format.send_pid) {
        const HipcSpecialHeader special = {.send_pid=1};
        memcpy((u8*)base + offset, &special, sizeof(special));
        offset += sizeof(special) + 8;
    }
    offset = (offset + 15) & ~(size_t)15;
    const u32 cmif[4] = {CMIF_IN_HEADER_MAGIC, 0, format.request_id, 0};
    memcpy((u8*)base + offset, cmif, sizeof(cmif));
    return (CmifRequest){(u8*)base + offset + sizeof(cmif)};
}
bool envIsSyscallHinted(unsigned);
u64 armGetSystemTick(void);
static inline u64 armTicksToNs(u64 ticks) { return ticks; }
Result svcSendAsyncRequestWithUserBuffer(Handle*, void*, u64, Handle);
Result svcConnectToNamedPort(Handle*, const char*);
_Noreturn void svcExitProcess(void);
void* armGetTls(void);
Result svcCloseHandle(Handle handle);
Result svcAcceptSession(Handle*, Handle);
Result svcReplyAndReceive(s32*, const Handle*, s32, Handle, u64);
Result svcWaitSynchronization(s32*, const Handle*, s32, u64);
Result smRegisterServiceCmif(Handle*, SmServiceName, bool, s32);
Result smUnregisterServiceCmif(SmServiceName);
SmServiceName smEncodeName(const char*);

/* Service-client fixture hooks. No actual process/service can be contacted. */
typedef struct { bool active; } Service;
typedef struct { u64 program_id; u8 storageID; } NcmProgramLocation;
enum { NcmStorageId_None=0 };
static inline bool serviceIsActive(Service* service) { return service->active; }
static inline void serviceClose(Service* service) { service->active = false; }
static inline bool hosversionIsAtmosphere(void) { return true; }
static inline void* smGetServiceSessionTipc(void) { return NULL; }
static inline Result smGetService(Service* service, const char* name) {
    (void)name; service->active = true; return 0;
}
static inline Result pmshellInitialize(void) { return 0; }
static inline void pmshellExit(void) {}
static inline Result pmshellLaunchProgram(u32 flags, const NcmProgramLocation* location, u64* pid) {
    (void)flags; (void)location; *pid=1; return 0;
}
Result testServiceCall(Service*, u32, void*, size_t);
#define serviceDispatchOut(service, command, output) \
    testServiceCall(service, command, &(output), sizeof(output))
#define serviceDispatchIn(service, command, input) \
    testServiceCall(service, command, (void*)&(input), sizeof(input))
#define serviceDispatch(service, command) testServiceCall(service, command, NULL, 0)
#define tipcDispatchInOut(service, command, input, output) \
    ((void)(service), (void)(command), (void)(input), (output)=1, (Result)0)
