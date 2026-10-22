/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Eden has no Atmosphere HasService or HBABI hint. All acquisition, TLS,
 * service commands and handle ownership still use the production client. */
#include <switch.h>
#undef tipcDispatchInOut
#define tipcDispatchInOut(service, command, input, output) ((output)=1, (Result)0)
#define hosversionIsAtmosphere() true
#include "../background/client/playback_client.c"
