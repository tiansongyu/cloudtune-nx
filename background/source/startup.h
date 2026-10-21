/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <switch.h>
#ifdef __cplusplus
extern "C" {
#endif
Result ncmStartupResult(void);
void ncmStartupRecord(const char *stage, Result result);
void ncmStartupWrite(const char *line);
void ncmStartupRecordResources(const char *stage);
#ifdef __cplusplus
}
#endif
