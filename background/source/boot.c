/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "boot.h"
#include "resources.h"
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
    if (R_FAILED(result))
        return result;

    /* A launch record only means CreateProcess completed. Require five seconds
     * of continuous resource headroom after that boundary; no playback heap or
     * workers are created while observing it. Other modules can still race us,
     * so heap growth and new work are also checked at their point of use. */
    unsigned healthy = 0;
    for (unsigned attempt = 0; attempt < 240; ++attempt) {
        result = ncmCheckStartupResources();
        if (R_FAILED(result) && result != MAKERESULT(Module_Kernel, 132))
            break;
        healthy = R_SUCCEEDED(result) ? healthy + 1 : 0;
        if (attempt % 20 == 0)
            ncmStartupRecordResources("waiting-resources");
        svcSleepThread(250000000);
        if (healthy >= 20) {
            /* Recheck after the final sleep, not before it. */
            result = ncmCheckStartupResources();
            if (R_SUCCEEDED(result))
                break;
            if (result != MAKERESULT(Module_Kernel, 132))
                break;
            healthy = 0;
        }
        result = MAKERESULT(Module_Libnx, LibnxError_Timeout);
    }
    ncmStartupRecord("boot-resource-gate", result);
    return result;
}
