# webOS 26 stream-start failure (2026-09-26)

After the LG G5 updated to **webOS 26**, every stream stopped at "Connecting…". The
RTSP handshake, control stream and video stream all started, and then
moonlight-common-c's `Starting audio stream...` never returned. First seen failing
2026-09-26 09:28; the update date itself is unknown (the image was built 2026-09-11).

**Cause (recorded in a diagnostic build):** Aurora's executable exported a data symbol named
`g_log` (the CTM HID-passthrough log buffer). An executable's export comes first in symbol
lookup, so every library that calls `g_log()` **through its own PLT** resolved it to that
buffer. libgstreamer does; libglib's internal calls are bound locally and were unaffected.
During the first media load on 11.2.0, GStreamer printed a plugin-loader warning through
`g_log()`, and the call jumped into the buffer. The production build hung at the same point
with the same export at the same address, so it is attributed to the same cause, but no fault
was recorded for it.

**Fix:** the buffer is renamed `ctm_log`, and a post-link check fails the webOS build if the
executable exports anything beyond `_init`, `_fini` and the linker markers (plus the four
PmLog entry points in a diagnostic build) — `cmake/CheckExecutableExports.cmake`. The check is
skipped, with a warning, when `CMAKE_NM` is unset.

## Why it broke now, and why it stayed hidden

The name `g_log` dates from `f9dda9ce` (2026-06-18). The **export** came later: archived builds
from 07-17, 07-21 and 07-23 do not export it, and builds from 08-03 on do. It started when the
link line gained glib-2.0 / playerAPIs (`2d01a674`, 07-28) and luna-service2 (`b9717e8b`, 07-30),
because GNU ld exports an executable global that a linked library references.

**The export only matters on one kind of launch: an app's first media load that has to rebuild
GStreamer's plugin state.** That load prints "External plugin loader failed" through
libgstreamer's `g_log()` PLT slot. Later loads do not print it. Measured on 11.2.0, 2026-09-26
(evidence `rescan-ab/` and `logs2/`):

| app id | binary | GStreamer state for that app | result |
|---|---|---|---|
| live | fixed, diagnostic (10:43) | stale after the update | streams; warning **printed**; audio open 656 ms |
| live | fixed, diagnostic rebuild of the same tree (12:57, 12:58) | rebuilt at 10:43 | streams; no warning; 165 / 136 ms |
| live | **unfixed** `80287f60` (exports `g_log`) (13:27) | rebuilt | **streams**; audio open 238 ms |
| beta, fresh install | **unfixed** `b1a3def5` (the 09-09 beta build) (13:28) | none | **stops at `Starting audio stream...`**; process gone within a minute |
| beta | fixed `55c140d4` (13:30) | none | streams; first audio open 756 ms |
| beta | **the same unfixed** `b1a3def5` (13:31) | rebuilt by the fixed build | **streams**; audio open 142 ms |

The probe app (`com.aurora.ndlprobe`) matched: it printed the warning on its first run (10:10)
and not on its next four. The state is kept **per app** (or per app uid), not once per TV. Where
it is kept was not found: there is no registry file readable from the dev shell and no
`/usr/libexec/gstreamer-1.0`. Upstream GStreamer rescans when plugin files change, keeps its
registry per user, and scans in-process when its helper cannot run. That is upstream's
behaviour, not measured here.

This explains the whole history:
- **Live app, after the update:** the plugin files changed, so the first load rescanned and hit
  the export. All three pre-fix launches stopped inside that load (09:28 hung; two diagnostic
  runs died). None finished the rescan, so each one repeated it.
- **Live app, 07-30 to the update, on 33.31.68:** it streamed for about two months while
  exporting `g_log`. That fits a plugin state built before the export existed and never rebuilt.
  It was not measured on the old side, and the TV cannot be downgraded.
- **Beta app, 09-07 to 09-16, on 33.31.68:** `com.aurora.gamestream.beta` was first installed
  after the export existed. It stopped at `Starting audio stream...` on every recorded launch
  (09-09, 09-15, 09-16), while the live app streamed the same source. That was recorded at the
  time as "the beta app id cannot open the TV's media pipeline". The same `b1a3def5` build does
  the same on 11.2.0 until a fixed build rebuilds that app's plugin state, and then it streams.
  So the beta failure was almost certainly this bug, not an app-identity limit. It was not
  re-run on 33.31.68.

**Consequence:** this class surfaces on the first media load of any app id without current
GStreamer state: after a firmware update, on a new app id (the beta variant), or on a first
install on another TV. Those are the launches to test.

## What changed in the firmware

The "after" column was measured 2026-09-26. The "before" column comes from earlier records and
the 09-15 library pulls.

| | before | after |
|---|---|---|
| firmware | 33.31.68 | 43.21.77 (built 2026-09-11) |
| `webos_release` | — (not recorded) | 11.2.0, platform code 11 |
| kernel | 5.4.268 | 6.12.44, aarch64 kernel, 32-bit userland |
| `libNDL_directmedia_impl` | `1.0.0-44.webos4tv.17-r9`, 120 284 B | `1.0.0-44.webos4media.6`, 120 420 B, needs `GLIBCXX_3.4.32` (old: 3.4.22) |
| `libplayerAPIs` (SMP client) | `starfish-media-pipeline 1.0.0-660.ptl4tv.18-r35`, 760 808 B | 814 064 B |
| GStreamer in the app process | yes (same 18 `gst_*` imports) | yes, **1.24.10** |

Vendor behaviour on 11.2.0, measured with `scripts/webos/ndl-probe` (n=1 per case):

- **The exported NDL API is unchanged** by name (signatures not compared).
  `NDL_DirectMediaInit` calls `SDL_webOSCreateExportedWindow`. The old library imports the same
  function, so this is not shown to be new. Without SDL video it logs an error and still
  returns rc=0.
- **When the probe presented no frames**, uMS reported it background (`isForeground[0]`), and
  the load had not returned after 15 s. It was not seen to unblock later. **When presenting**
  (foreground), the load returned in 89–107 ms, including with early init, a blocked main
  thread, or Aurora's SDL 2.30.12.
- **All five probe-app runs logged** `GLib-GObject-CRITICAL: g_object_set: assertion 'G_IS_OBJECT
  (object)'`. It is harmless here: it is raised inside libglib, which binds `g_log` locally, so
  it could never reach an executable export. Only GStreamer's plugin-loader warning goes through
  libgstreamer's PLT. Why the scanner fails was not measured.

## How the cause was proven

1. `/tmp/aurora-diag.log` stopped at `Starting audio stream...`, which is the first
   `NDL_DirectMediaLoad`. The load is deferred until audio opens because Aurora sets
   `SS4S_PlayerSetWaitAudioVideoReady(true)` (`session_worker.c:108`). That setting belongs to
   upstream ss4s, and its register row cites ss4s's own source:
   `modules/webos/ndl/webos5/ndl_player.c` `LoadMedia`.
2. A standalone probe app (`scripts/webos/ndl-probe`) running Aurora's NDL call order (Init,
   then Load with H.265 + PCM) **loaded successfully**, so the problem was specific to Aurora's
   process.
3. A diagnostic Aurora build (`-DAURORA_VENDOR_DIAG=ON`) captured the vendor logs and a
   fatal-signal record:
   `SIGSEGV code 2 (SEGV_ACCERR) pc == addr == 0x041d8920, lr = libgstreamer+0x7f53b`.
   The disassembly at the return address is `blx g_log@plt` with `r1 = 16`
   (`G_LOG_LEVEL_WARNING`), domain "GStreamer" and format "External plugin loader failed…".
   The PLT slot is `R_ARM_JUMP_SLOT g_log`. 0x041d8920 lies in the executable's bss, which is not
   executable, hence `SEGV_ACCERR`.
4. The Sep 22 production binary (`be88a4b5`) exports exactly one symbol besides
   `_init`/`_fini`: `041d8920 B g_log`. The rebuilt binaries place `ctm_log` at the same address.
   The crashing diagnostic binary itself was not archived.
5. With the buffer renamed, the diagnostic build connected a live desktop stream from Skynet.
   `SS4S_PlayerAudioOpen` returned OK after 656 ms, and the same GStreamer warning was printed
   harmlessly to the captured stderr (`logs2/aurora-stderr-all-diag-runs.log`, pid 28684).
6. The **release** ipk (`50982e6d…`, diagnostics OFF — the binary that ships) then streamed
   live from Skynet:
   - `Starting audio stream... done` in 118 ms.
   - TV screenshots at about 60 s and about 170 s showed the live host desktop, with the taskbar
     clock advancing.
   - `ok=` rose to 7596 with `errors=0` and `idr_throttled=0`.

**Established:** the production build *hung*; the process was alive 304 s later. **Not
established:** whether it faulted at all. Aurora's primary installs no fatal-signal handler,
and no fault record exists for that run. One unverified candidate: libplayerAPIs imports
`sigaction` and contains "end starfish-media-pipeline using SIGNAL[%d]". The other threads kept
running (`Received first video packet` was logged), which is why the UI stayed on "Connecting…".

## What else was run on 11.2.0 after the fix

| test | result |
|---|---|
| Co-op, SKYNET primary + VEGA peer, horizontal tiles (`"coop":true`, peer from `/tmp/coop-spawn`, no host config applied) | streams. Audio open 189 ms; the secondary's audio and input links came up; compose 153–160 fps; VEGA pane ~32 fps (a static Big Picture page, so not a rate verdict); `mirrored=0`, `errors=0` (`logs2/coop-and-vega-counters.log`). Panel at ~3 min (VEGA's clock 12:41, session from 12:38:11): SKYNET's desktop on top, VEGA's Steam Big Picture below; no seam artefact visible at 960x540 |
| Solo, VEGA, HEVC 8-bit SDR (HDR off for the run, then restored) | streams. Audio open 169 ms; `ok=` 27 → 2006; `errors=0` (read live). Panel to +75 s: decoded cleanly, host clock ticking 12:46 → 12:47 |
| Solo, VEGA, HDR on | **fails, but not because of the firmware.** VEGA's Apollo logs `hevc_amf: dynamic range not supported` five times and sends no packet. Aurora ends the session after 10 s ("lack of video traffic", `reason=4098`). A solo session requests HDR from the user's settings without checking whether the host advertises 10-bit; only co-op has that gate (`coop_stream_plan()`). VEGA advertises `ServerCodecModeSupport=257` (no Main10 bit 0x200), so a host-capability check would have caught it. The TV opened video and audio normally (`logs2/vega-apollo-1244-hdr-refused.log`) |
| Audio decrypt (moonlight-common-c `09dfc5a`, new since Sep 22) | `Failed to decrypt audio packet` (how a failed decrypt shows; the new padding call fails silently) appears 0 times in the TV's diag log across every session today. Not checked by ear |

## Does the old firmware still work?

The TV cannot be downgraded, so nothing was re-run on 33.31.68. What was measured instead:

- **Loader requirements are unchanged.** The fixed release binary has the same 29 `NEEDED`
  entries in the same order, the same RPATH and the same symbol-version needs as `e81d8c67`
  (which ran live on 09-17/18) and `be88a4b5` (Sep 22). It imports three new names:
  - `SDL_GetHint` and `SDL_SetHintWithPriority` (commit `b0828a57`), defined by the bundled
    `lib/backports/libSDL2`.
  - `mbedtls_cipher_set_padding_mode` (moonlight-common-c `09dfc5a` via `4edde4ba`), defined by
    the bundled `lib/libmbedcrypto.so.14`.

  It no longer imports `stpncpy`. Both bundled libraries are byte-identical to the Sep 22
  install's. Caveat: whether 33.31.68 resolves `libmbedcrypto.so.14` to the app's copy or a
  system copy was not measured.
- **Nothing the Sep 22 build contained is missing.** Once build paths are normalised, every
  string in `be88a4b5` except the dropped `stpncpy@GLIBC_2.4` import is also in `50982e6d`; the
  extra strings come from commits after Sep 22.
- **The fix adds no firmware-dependent branch.** The rename keeps the buffer's address, and
  removing the export only changes what a PLT-resolved `g_log` reaches. ss4s chooses its media
  module by `os_version` (`ss4s_modules.ini`), and it chose `ndl-webos5` on both firmwares.

**Not established:** the runtime behaviour on 33.31.68 of the ~10 commits made after the Sep 22
build (UI renderer hints, focus/pointer, audio CBC padding, default stream size). They never
ran on the old firmware. They run correctly on 11.2.0.

## Next firmware update: what to run first

The first media load of each app id after an update, or after that app id is first installed,
rebuilds GStreamer's plugin state inside Aurora's process (see above). Treat that launch as the
test, for the live AND the beta app id.

1. **`scripts/webos/symbol_clash_scan.sh <unpacked-app> [crash-log]`** (~10 s) — checks whether
   any library Aurora ships into the global scope defines a name that a TV library also
   defines. That is the `g_log` class for bundled libraries; the executable is covered at build
   time. Both modes add every `/usr/lib/gstreamer-1.0` plugin. Exit 1 lists each clash, and exit
   2 means it could not read the TV side, including any library it failed to pull.
   - On 11.2.0 it is clean. With the run-2 maps snapshot (taken before any plugin loaded) plus the
     plugins, that is 262 TV libraries; with the NEEDED closure plus the plugins, 318. Either way
     it checks 6 Aurora libraries.
   - A planted `g_log` in a fake `lib/libinih.so.1` makes it report
     `CLASH … g_log /usr/lib/libglib-2.0.so.0.7800.6` (exit 1).
   - A maps entry that cannot be pulled makes it exit 2.
   - Without a crash log, `dlopen()`ed libraries other than the plugins are not covered.
2. **`scripts/webos/ndl-probe/run.sh`** — checks whether the vendor media stack loads at all,
   outside Aurora (~1 min). If it fails here, the problem is outside Aurora's process, and the
   probe log says where.
3. **A diagnostic Aurora build:**
   `CI=1 TOOLCHAIN_FILE=… CMAKE_BINARY_DIR=build/webos-diag ./scripts/webos/easy_build.sh
   -DCMAKE_BUILD_TYPE=Release -DWEBOS_APPINFO_ID=com.aurora.gamestream -DAURORA_VENDOR_DIAG=ON`.
   Then read, on the TV:
   - `/tmp/aurora-vendor.log` — NDL/SMP/AV-connector logs, which the TV otherwise discards.
   - `/tmp/aurora-crash.log` — ARM/DONE per vendor call, fatal-signal records, and hang dumps
     after 8 s and 30 s. Addresses are raw; symbolise them with the maps block and the
     TV's own libraries.
   - `/tmp/aurora-stderr.log` — dynamic-loader errors and GLib/GStreamer warnings.

   Never ship a diagnostic build; `build_for_lg.sh` forces the option OFF. The files are owned
   by the app's uid in a sticky `/tmp`, so `prisoner` cannot delete them; each run appends. Paths
   carry the `-beta` suffix on the beta variant. Limits of the diagnostic:
   - Only the primary player's opens and `SS4S_PostInit` are armed.
   - A co-op secondary re-installs its own fatal-signal handlers.
   - A hang dump signals every thread, so treat anything after the first dump as perturbed.
4. The build fails after linking if the executable exports symbols. If it does, rename the
   symbol or make it `static`; do not widen the allowlist.

## Residual risks (known, not fixed)

- **`lib/libcurl.so.4` is the upstream "curl ABI fix" shim** for webOS ≤4, which shipped curl as
  `libcurl.so.5`. The shim needs `libcurl.so.5`, which this firmware lacks. Measured on 11.2.0:
  the process maps `/usr/lib/libcurl.so.4.8.0` and `/usr/lib/libopus.so.0.10.1`, while
  libraries only the app has (libinih, libmbedcrypto, ss4s) load from its `lib/`. So `/usr/lib`
  is searched before the app's `lib/`, and the shim is inert. The app's `lib/` is not on the
  RPATH (`$ORIGIN/lib/backports:$ORIGIN/../lib/backports:$ORIGIN`, `$ORIGIN` = `bin/`), so it
  must come from the launch environment, which was not read. If a future jail put the app's `lib/` first, the
  app would fail at launch with `libcurl.so.5: cannot open shared object file`.
- **Vendor libraries in Aurora's process already bind to Aurora's bundled SDL2 2.30.12**, the
  only libSDL2 mapped (it is on the RPATH); `libNDL_directmedia_impl` does so today and works. A
  future vendor library or GStreamer plugin that needs newer SDL symbols would fail inside
  Aurora. libopus resolves to the system copy.
- **A load started while Aurora is not foreground** did not return within 15 s (n=1, probe).
  Aurora has no timeout around it (`ndl_player.c` makes a synchronous call).

Evidence: `~/aurora-tv-runs/webos26-20260926/README.txt` (logs, logs2, shots, shots2, compat).
