/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <switch.h>

/* Uses Atmosphere's read-only launch record, never pm:shell's recovery-only
 * GetBootFinishedEventHandle or NotifyBootFinished commands. */
Result ncmBootHasHomeMenu(bool *launched);
Result ncmWaitForHomeMenu(void);
