/*
 * "IS THE COMPANION HOST SOFTWARE THERE?" -- one answer, used by every setting that needs it.
 *
 * THE POLICY THIS EXISTS TO SERVE (standing, 2026-09-11): a setting that cannot work without
 * host-side software the user may not have is OFF BY DEFAULT, CARRIES A NOTE SAYING WHAT IT
 * NEEDS, and turns itself on when that software is detected. Eclipse is the platform and must
 * work against stock Apollo or stock Sunshine; anything beyond that is an accelerator the app
 * notices, never a prerequisite it assumes.
 *
 * WHAT COUNTS AS DETECTION, AND WHY IT IS NOT A VERSION STRING. Our Apollo build is recognised
 * by the serverinfo fields only it emits -- `EncoderVendor` and `CoopProfileCaps`. A host that
 * answers either one is running it; stock Apollo, stock Sunshine, GFE and an unpaired HTTP probe
 * all stay silent. That makes absence indistinguishable from "not asked yet", which is exactly
 * why every caller here treats false as "do not assume", never as "the user has the wrong host".
 */
#pragma once

#include <stdbool.h>

#include "uuidstr.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Does THIS host run the Eclipse/Umbra build? False for a host we have not polled. */
bool aurora_apollo_on_host(const uuidstr_t *uuid);

/** serverinfo UmbraCaps bits: 0 = wired pin (below), 1 = the host's own log for a paired client (/umbralog,
 *  fetched into the kept log at the end of each stream -- util/diag_logs.h). */
#define UMBRA_CAP_WIRED_PIN 0x1
#define UMBRA_CAP_HOST_LOG 0x2
/** Bit 2: Umbra understands Eclipse co-op (built-in hidden Co-op Pane, umbraCoop per-launch tuning) and
 *  reports bit 3; bit 3: its GPU HEVC co-op encoder is installed. Mirrored in coop_stream_mode.c, which
 *  is built into unit tests without this header. */
#define UMBRA_CAP_COOP_PANE 0x4
#define UMBRA_CAP_GPU_HEVC 0x8
/** Bit 4: Umbra switches its physical monitor(s) off for a stream on request (umbraMonitorOff,
 *  app_settings.h host_monitor_mode). */
#define UMBRA_CAP_MONITOR_OFF 0x10

/** Can THIS host switch its physical monitor off for a stream (UmbraCaps bit 4), and is it not this PC? */
bool umbra_monitor_off_on_host(const uuidstr_t *uuid);

/** Can THIS host keep a client's stream on the client's wired card (Umbra, serverinfo UmbraCaps bit 0)? */
bool umbra_wired_pin_on_host(const uuidstr_t *uuid);

/** The first paired host offering the wired pin, or NULL -- for the settings row's note. */
const char *umbra_wired_pin_host_name(void);

/** Does ANY paired host run it? The question a global setting has to ask, since a setting is
 *  not per-host and a two-machine feature can be enabled by either side having it. */
bool aurora_apollo_anywhere(void);

/** The name of the first host found running it, or NULL. For the note on a settings row:
 *  "detected on Skynet" is actionable, "detected" is not. */
const char *aurora_apollo_host_name(void);

#ifdef __cplusplus
}
#endif
