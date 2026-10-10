# Third-party components, licences, and attribution

> [!CAUTION]
> **Historical snapshot, superseded 2026-09-30.** Do not use this file as the public acknowledgement or a release-compliance checklist. In particular, its CTM-USBIP licence row predates the verified GPL-3.0 source, while its package-notice coverage predates the artifact audit. Use [`ACKNOWLEDGEMENTS.md`](../ACKNOWLEDGEMENTS.md) for the current system inventory and open release blockers; artifact-specific notices/SBOMs still need to be generated from each exact build.

**Compiled 2026-09-11 for public release.** Licences marked **READ** were verified by opening
the licence file in this working tree on that date. Anything marked **VERIFY** is a
well-known value that nobody here has checked against a primary source yet — treat those as
open, not as facts.

Aurora itself is **GPL-3.0** (`LICENSE.txt`), inherited from moonlight-tv. moonlight-common-c
is also GPL-3.0, so the client as a whole is GPL-3.0 and every bundled component must be
GPL-3.0 compatible.

## Aurora client — linked or vendored

| Component | Source | Licence | How used |
|---|---|---|---|
| moonlight-common-c | `core/moonlight-common-c` (submodule, moonlight-stream) | GPL-3.0 — **READ** | linked; the streaming protocol |
| SS4S | `third_party/ss4s` — **our fork** of mariotaku/ss4s, branch `aurora-coop` | LGPL-3.0 — **READ** | linked; media backends. Carries our NDL `SetDisplayArea` change |
| commons-c | `third_party/commons` (mariotaku) | MIT — **READ** | linked; utility + LVGL helpers |
| LVGL | `third_party/lvgl` (mariotaku fork) | MIT — **READ** (`LICENCE.txt`) | linked; the whole UI |
| libmicrodns | `third_party/libmicrodns` (videolabs) | LGPL-2.1 — **READ** | linked; mDNS host discovery |
| Unity | `third_party/Unity` (ThrowTheSwitch) | MIT — **READ** | test harness only, not shipped |
| SDL2 / SDL2_image | system package | zlib — **VERIFY** | window, input, image decode |
| mbedTLS | system package | Apache-2.0 — **VERIFY** | pairing + TLS |
| libopus | system package | BSD-3-Clause — **VERIFY** | audio decode |
| libcurl | system package | curl (MIT-like) — **VERIFY** | HTTP to the host |
| expat | system package | MIT — **VERIFY** | serverinfo XML |
| FreeType / Fontconfig | system package | FTL or GPLv2 / MIT-like — **VERIFY** | text rendering |
| inih | system package | BSD-3-Clause — **VERIFY** | `moonlight.ini` parsing |
| pbnjson | webOS system library | Apache-2.0 — **VERIFY** | luna payloads (webOS build only) |
| `gamecontrollerdb.txt` | gabomdq/SDL_GameControllerDB, fetched by `scripts/webos/gen_gamecontrollerdb.sh` | **VERIFY** — shipped in `conf/`, so its terms apply to us | controller mappings |
| webOS buildroot SDK | openlgtv/buildroot-nc4 `webos-b17b4cc` | **VERIFY** | build-time toolchain only, not redistributed |

## Host side

| Component | Licence | How used | Release scope |
|---|---|---|---|
| Apollo (fork of Sunshine) | GPL-3.0 — **READ** | the host streamer | optional "Aurora Apollo" build, now released as Umbra |
| `aurora-link` | ours, **no licence header** | compiled INTO the Apollo exe, so it must be GPL-3.0 compatible | with the Apollo build |
| `dispwake.exe` + `apollo-display.cmd` | ours, **no licence file** | display resume/pause via the stock `global_state_cmd` hook | **main release** |
| `arp-pin.ps1` | ours, **no licence file** | static ARP to pin the TV's network path; rig-specific workaround | optional |
| CTM-USBIP | **NO LICENCE FILE — GAP** | USB/IP agent for Pi-attached controllers | **Pi only, out of the main release** |
| usbip-win2 (vadimgrn) | BSD-2-Clause, signed installer redistributed unmodified — **READ** (CTM-USBIP README) | prerequisite driver for the above | Pi only |
| `ffmpeg.exe` (115 MB, dated 2024-06-18) | **PROVENANCE UNKNOWN — GAP** | mic audio transcode | Pi only |
| `mic_receiver.exe`, `audio_default.exe`, `agent_ctl.exe`, `xlink-host.exe` | ours, no licence headers; `xlink-host` vendors ViGEmClient (**VERIFY**) | Xbox mic + pad transport | Pi only |

## Gaps to close before anything is published

1. **`ffmpeg.exe` has unknown provenance.** Which build, from where, GPL or LGPL, and no source
   offer shipped beside it. Redistributing an ffmpeg binary carries obligations either way.
   **The Pi split removes it from the main installer**, which is the cheapest possible fix — but
   it must not come back without answering this.
2. **CTM-USBIP has no licence at all.** It is our own code, so this is ours to state, but until
   it says something it is "all rights reserved" and nobody may redistribute it. Pi-scoped, so
   not release-blocking today.
3. **Our own host binaries carry no licence headers** — `dispwake`, `arp-pin`, `aurora-link`,
   `xlink-host`. `aurora-link` is compiled into a GPL-3.0 exe and must be compatible; the others
   should simply say what they are.
4. **`gamecontrollerdb.txt` ships inside the ipk** and its terms have never been checked here.
5. **NOTICE file.** Nothing in the tree currently credits any of the above to a user. A single
   `NOTICE` shipped in both installers, generated from this table, is the fix.

## Verified clean, do not re-check

No embedded credentials anywhere in the tree (audited 2026-08-17/18 over all 655 tracked files;
re-confirmed for the shipped surfaces 2026-09-11 — every IP, UUID, MAC and hostname found in
`src/` is inside a comment, not a code path). Both forked submodules resolve anonymously and the
pinned SS4S SHA is published on `aurora-coop`, so a fresh recursive clone builds.
