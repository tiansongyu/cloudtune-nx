/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <switch.h>
#ifdef __cplusplus
extern "C" {
#endif
Result ncmStartupResult(void);
void ncmStartupRecord(const char *stage, Result result);
#ifdef __cplusplus
}
#endif
