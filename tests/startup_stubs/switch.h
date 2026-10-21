/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint32_t Result;
typedef uint32_t u32;
typedef unsigned Mutex;
#define R_FAILED(r) ((r) != 0)
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
