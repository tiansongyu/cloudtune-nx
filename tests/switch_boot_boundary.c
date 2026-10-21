/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Emulator-only boundary for the excluded native runtime probe. Standalone
 * Eden has no Atmosphere HOME launch record. Production never links this. */
#include "heap.h"
#include "startup.h"

Result __wrap_ncmBootHasHomeMenu(bool *launched) {
    static unsigned queries;
    if (ncmHeapCommittedSize() != 0)
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    *launched = ++queries > 3;
    if (*launched)
        ncmStartupRecord("probe-simulated-home-zero-heap", 0);
    return 0;
}
