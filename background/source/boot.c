/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "boot.h"
#include "startup.h"

Result ncmWaitForHomeMenu(void) {
    Result result = pminfoInitialize();
    ncmStartupRecord("boot-pm-info", result);
    if (R_FAILED(result))
        return result;

    /* HOME's launch is recorded persistently, so a service started later from
     * a game does not wait for HOME to be the foreground application. A missing
     * or unsupported query must not silently enable eager boot initialization.
     * Bound the wait: recovery boot without HOME should release this process. */
    for (unsigned attempt = 0; attempt < 480; ++attempt) {
        bool launched = false;
        result = ncmBootHasHomeMenu(&launched);
        if (R_FAILED(result) || launched)
            break;
        if (attempt % 20 == 0)
            ncmStartupRecordResources("waiting-home");
        svcSleepThread(250000000);
        result = MAKERESULT(Module_Libnx, LibnxError_Timeout);
    }
    pminfoExit();
    ncmStartupRecord("boot-home-menu", result);
    return result;
}
