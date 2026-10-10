# Release defaults — Eclipse (client) and Umbra (host)

Updated 2026-10-08 against the release source. "Default" means a fresh profile; an existing profile
keeps its own values. Values below are source-reviewed unless a dated measurement is explicitly cited.
Historical measurements do not constitute acceptance of the current release packages.

Capability-dependent features check the host's advertised support (`aurora_host_caps.h`). Their controls
are described individually below: some use Automatic / Off / On, while the host-monitor control offers
a choice at launch and co-op host tuning is an on/off setting.

Parameters are tracked in `docs/parameter-register.md` (§40 and §47 cover the additions discussed here).

## Eclipse (client)

| Setting | Windows | Linux | webOS | Why |
|---|---|---|---|---|
| Resolution | panel's own | panel's own | panel's own | A constant (720p, later 1080p) asked a 1440p/4K panel's host for less than it could show. Filled from SDL at `app_init`; webOS asks the panel directly. |
| Frame rate | 60 | 60 | 60 nominal / 59.94 requested | The webOS preset sets the fractional client refresh to 5994 for nominal 60 fps. This is preset reconciliation, not panel-refresh detection (`app_settings.c`). |
| Bitrate | automatic (Moonlight Qt curve) | automatic | automatic | Automatic bitrate follows resolution/fps and is not persisted. A manually selected positive bitrate is persisted and remains in an existing profile (`app_settings.c`). |
| HEVC / AV1 / HDR | on / off / off | on / off / off | on / off / off | HDR needs an HDR host display; AV1 decode is not universal. Users opt in. |
| Audio | stereo, played here | stereo, played here | stereo, played here | **Exception (item 1):** a session whose host is THIS PC always plays the sound on the host (`localAudioPlayMode=1`), so the host never switches this PC's speakers to its virtual sink. Windows only (`local_host_win.c`); the picker offers this PC only as a co-op host. |
| Fullscreen | on | on | (always) | |
| Keep diagnostic logs | **off** | **off** | **off** | Writes every start's log to disk; turned on when someone asks for logs (About > "Keep diagnostic logs", from the next start). **Also on by itself for one start after a run that did not end cleanly** (crash, frozen app closed by force, failed start, power loss: `logs/running.txt` left behind), so an app that dies before its UI still leaves a log; on desktops that start also rewrites the one file to send. A normal close removes the marker (measured on all three, 2026-09-30). How to get and read the file: `docs/support-logs.md`. |
| Share logs… | one bundle file + "Open folder" | bundle file + network page | network page | The bundle `<settings>/logs/eclipse-logs.txt` = summary, kept logs, co-op diagnostics, settings with PIN/password/token/secret removed. **No network page on Windows:** the first listen raised Windows Firewall's prompt over the fullscreen app and left the dialog dead (measured on VEGA 2026-09-29); the TV has no reachable files, so the page is its only way out. |
| Nintendo controllers | Buttons as printed (A = A) | same | same | Detected per controller (SDL type, else VID 057e). An existing profile migrates: `swap_abxy` on → "Xbox positions (B = A)". "Don't detect them" restores the old swap-everything behaviour. |
| Swap A/B X/Y (other pads) | off | off | off | |
| Remote controls as controllers | last slots, no setting | last slots | ignored (as before) | A remote-like device (remote/rcu/air mouse/media center/ir receiver) takes controller numbers from the TOP, so a game that reads only the first 1–4 pads gets the real pads. |
| Use the wired network (USB Ethernet) | — | — | **Automatic** | Asks only a host that advertises it (Umbra, `UmbraCaps` bit 0) and only while a wired card is up. On = ask any host (stock hosts ignore it). Replaces the rig's `arp-pin.ps1` hook. VERIFIED on the TV (webOS 26) with PC2: both TV addresses pinned to the adapter, the stream arrived on it (1568 packets vs 12 on Wi-Fi), released at the end. The TV's Wi-Fi stays ON (user rule) -- this is the fix that works with it on. |
| Optimize game settings / host audio / quit app after | on / off / off | same | same | Moonlight defaults. |
| Co-op | horizontal, audio mixed, stream mode Automatic, pads alternate panes | same | same | Automatic selects the codec path using both hosts' capabilities. Advanced configuration and historical tests are recorded in `docs/coop-modes.md`; they are not current-package acceptance. |
| Co-op encoder / host tuning / audio deduplication | Automatic / on / on | same | same | Source defaults in `app_settings.c`. Co-op launches send `umbraCoop=1` with host tuning enabled, or `0` when disabled; ordinary launches do not request co-op tuning. |
| Host monitor while streaming | Choose when starting a stream | same | same | The start controls offer the monitor choice; the host must support the requested action (`host.pane.c`, `app.c`). |
| PIN auto-entry | off (no PIN stored) | off | off | The PIN is the user's Windows PIN in plain text in the ini; it is never in a shared log. |
| Stats overlay, deadzone, mouse | off, 7 %, absolute | same | same | |

Ship the **release** variant, not `AURORA_VARIANT=beta` (webOS app id `com.aurora.gamestream.beta`), for
the public package.

**webOS compatibility target:** support the current and previous webOS versions. Historical tests used
webOS 26 after the available TV was updated. Neither source review nor those tests establishes current
package compatibility with the previous firmware. Fresh install, playback, close and uninstall acceptance
on both versions remains a release gate.

## Umbra (host)

| Setting | Windows | Linux | Why |
|---|---|---|---|
| Apps | Desktop, Steam Big Picture; built-in Co-op Pane when the virtual display driver is ready | Desktop, Steam Big Picture; no built-in Co-op Pane | Windows `process.cpp` adds Co-op Pane for fresh and existing profiles unless an app with that name already exists. It is hidden from ordinary client lists unless running or explicitly requested. Disconnect leaves it resumable (`terminate_on_pause=false`); Quit ends it. Linux co-op remains untested. The hardware-specific Low Res Desktop sample is removed from the release source. |
| Encoder | automatic | automatic | The rig pins `nvenc` / `amdvce`; a shipped host must probe. |
| Restore the default speakers after a session | built in | built in | Item 1: by id, else by name, else retried for 60 s; the last real default is remembered across a crash (`audio_restore_sink.txt`). Makes the rig's session-end audio hook redundant. Verified on VEGA (tests A–E) and on Linux (PulseAudio `auto_null`, restored after 1 s). |
| Wired pin | built in; `UmbraCaps` bit 0 set | not supported; bit 0 clear | `UmbraCaps` is a feature bitmask, so its total value is not simply 1 or 0. Linux neighbor pinning needs privileges the package does not grant. With bit 0 clear, Automatic does not request pinning. |
| Co-op seam knobs (`coop_pad_rows`, `coop_top_pad`, `gop_length`, `nvenc_h264_main`, `nvenc_constrained`, `amd_intra_refresh_mb`) | neutral defaults (zero / off / automatic or unset) | same | Selected controls are now scoped per launch; see below. No rig-specific values should ship in a fresh profile. |
| FEC / minimum fps target / double refresh | 20 % / 0 (= stream rate) / off | same | Stock. The rig's `minimum_fps_target = 60` is a rig choice. |
| Display wake hook (`apollo-display.cmd`) | not shipped | not shipped | Rig-specific hook. |
| Bubbles server command (`server_cmd`) | Added when a missing `sunshine.conf` is first created: `Bubbles`, `bubbles.scr`, `elevated=false` | Not generated | Inherited [Apollo default](https://github.com/ClassicOldSong/Apollo/commit/83dfea5d1eb8ce380f8474008b4acba7e0696109), source-reviewed in `config.cpp`; separate from `apps.json`. An existing empty configuration file does not trigger this initialization. Command registration is not evidence of execution. |
| mDNS | on | on — **needs avahi-daemon** | Without it: "Failed to create client: Daemon not running" and clients must add the host by address. The .deb should Recommend `avahi-daemon`. |
| Web UI | LAN only, port 47990 | same | Stock. |

### Co-op control scope (source-reviewed)

- `coop_pad_rows` applies only to co-op launches and is disabled for GPU HEVC
  (`process.cpp`). `gop_length` is co-op-only in the avcodec and x265 paths (`video.cpp`).
- Automatic `nvenc_constrained=-1` enables constrained encoding for multiple slices, configured slice
  rows, or a co-op launch, subject to GPU support. Explicit 0/1 still overrides it globally
  (`nvenc/nvenc_base.cpp`).
- `coop_top_pad`, `nvenc_h264_main` and `amd_intra_refresh_mb` still have global consumers. Do not
  describe every seam control as launch-scoped (`display_vram.cpp`, `nvenc_base.cpp`, `video.cpp`).
- An explicit `umbraCoop=1` or `0` controls launch/resume tuning. If the argument is absent, Umbra
  recognizes Co-op Pane by its built-in identity or exact name (`nvhttp.cpp`). This implementation
  review does not establish that every encoder combination works.

### Linux package status

The current release package and its final dependency/hardware acceptance are pending. Record its source
revision, artifact hash, supported distribution baseline and build features in the release report after
packaging. Do not use the older smoke test below as acceptance of a new binary.

Historical evidence from 2026-09-29: an Ubuntu 24.04 build from `ae235282` plus then-current changes
produced `umbra-linux-ae235282-dirty-ubuntu24.04-amd64.deb` (9.8 MB, package `apollo`). A throwaway WSL
distro with private Xvfb and PulseAudio streamed Desktop to Eclipse Linux using X11 capture and libx264;
the client showed a live H.264 picture and received audio. That build was software/VA-API only, without
CUDA/NVENC, and required glibc 2.39. These are historical artifact properties, not the current release's
compatibility promises.
