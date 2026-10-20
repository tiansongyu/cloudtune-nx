#pragma once
/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Small Linux preview compatibility surface. Values match libnx; no Horizon
 * service, process, or input emulation is provided by this header.
 */
#include <stdbool.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef uint32_t Result;

#define R_SUCCEEDED(result) ((result) == 0)
#define R_FAILED(result) ((result) != 0)
#define R_MODULE(result) ((result) & 0x1FFu)
#define R_DESCRIPTION(result) (((result) >> 9) & 0x1FFFu)
#define R_VALUE(result) ((result) & 0x3FFFFFu)
#define MAKERESULT(module, description) \
    ((((uint32_t)(module)) & 0x1FFu) | ((((uint32_t)(description)) & 0x1FFFu) << 9))

enum {
    Module_Libnx = 345,
    LibnxError_NotInitialized = 8,
    LibnxError_NotFound = 9,
    LibnxError_IoError = 10,
    LibnxError_BadInput = 11,
    LibnxError_Timeout = 49
};

typedef struct HidTouchState {
    u64 delta_time;
    u32 attributes;
    u32 finger_id;
    u32 x;
    u32 y;
    u32 diameter_x;
    u32 diameter_y;
    u32 rotation_angle;
    u32 reserved;
} HidTouchState;

typedef struct HidAnalogStickState {
    s32 x;
    s32 y;
} HidAnalogStickState;

typedef enum {
    HidNpadButton_A = 1u << 0,
    HidNpadButton_B = 1u << 1,
    HidNpadButton_X = 1u << 2,
    HidNpadButton_Y = 1u << 3,
    HidNpadButton_StickL = 1u << 4,
    HidNpadButton_StickR = 1u << 5,
    HidNpadButton_L = 1u << 6,
    HidNpadButton_R = 1u << 7,
    HidNpadButton_ZL = 1u << 8,
    HidNpadButton_ZR = 1u << 9,
    HidNpadButton_Plus = 1u << 10,
    HidNpadButton_Minus = 1u << 11,
    HidNpadButton_Left = 1u << 12,
    HidNpadButton_Up = 1u << 13,
    HidNpadButton_Right = 1u << 14,
    HidNpadButton_Down = 1u << 15,
    HidNpadButton_StickLLeft = 1u << 16,
    HidNpadButton_StickLUp = 1u << 17,
    HidNpadButton_StickLRight = 1u << 18,
    HidNpadButton_StickLDown = 1u << 19,
    HidNpadButton_StickRLeft = 1u << 20,
    HidNpadButton_StickRUp = 1u << 21,
    HidNpadButton_StickRRight = 1u << 22,
    HidNpadButton_StickRDown = 1u << 23,
    HidNpadButton_AnyLeft =
        HidNpadButton_Left | HidNpadButton_StickLLeft | HidNpadButton_StickRLeft,
    HidNpadButton_AnyUp =
        HidNpadButton_Up | HidNpadButton_StickLUp | HidNpadButton_StickRUp,
    HidNpadButton_AnyRight =
        HidNpadButton_Right | HidNpadButton_StickLRight | HidNpadButton_StickRRight,
    HidNpadButton_AnyDown =
        HidNpadButton_Down | HidNpadButton_StickLDown | HidNpadButton_StickRDown
} HidNpadButton;

static inline Result smInitialize(void) { return 0; }
static inline void smExit(void) {}
