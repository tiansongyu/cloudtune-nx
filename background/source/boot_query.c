/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "boot.h"

Result ncmBootHasHomeMenu(bool *launched) {
    const u64 home_menu_id = 0x0100000000001000ull;
    u8 value = 0;
    Result result = serviceDispatchInOut(pminfoGetServiceSession(), 65001, home_menu_id, value);
    *launched = R_SUCCEEDED(result) && (value & 1);
    return result;
}
