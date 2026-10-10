# Hardware test coverage — what is verified, and what is not

This file exists because a release that says "supports AMD" without saying *which parts were
actually run on AMD silicon* is making a claim it cannot back. Everything below is stated as
either MEASURED on real hardware, or UNTESTED with the reason.

Last updated 2026-08-23.

## The rig this was developed on

| role | hardware | notes |
|---|---|---|
| primary host | NVIDIA RTX 4070 | native NVENC path (`src/nvenc/`), bypasses avcodec |
| peer host | NVIDIA RTX 3070 | same |
| AMD test host | **Radeon RX Vega** (VCE 4.0) | encoder-behaviour box only — see the limitation below |
| client | LG G5, webOS 33.31.68 | wired UE300 USB Ethernet |

## Coverage matrix

| combination | status |
|---|---|
| NVIDIA host, single stream, 10-bit HDR | MEASURED, in daily use |
| NVIDIA + NVIDIA co-op, 10-bit HDR | MEASURED |
| AMD host, HEVC 8-bit, encoder behaviour | MEASURED (see below) |
| **AMD host, 10-bit / HDR10** | **UNTESTED — and untestable on this rig** |
| AMD + NVIDIA co-op | NOT YET RUN |
| Intel QSV / VAAPI any configuration | NEVER TESTED |

## The AMD limitation, precisely

**Vega cannot encode 10-bit HEVC. This is a silicon limit, not a driver one.** VCE 4.0 decodes
Main10 but only *encodes* 8-bit; 10-bit HEVC encode arrives with Navi (RX 5000) and later. No
driver — AMD's, or any open-source stack — can add an encoder capability the die does not have.

Two things follow, and both were confirmed rather than assumed:

- The installed driver reports Radeon Software `22.20.27.09`. ffmpeg's `amfenc.c` refuses
  `AV_PIX_FMT_P010` below `AMF_MAKE_FULL_VERSION(1,4,32,0)` ("10-bit ... lower than 23.30"),
  because 10-bit *worked incorrectly* on older AMF. That guard is not the root cause, but it is
  the error you will see.
- AMD froze Polaris/Vega on a maintenance branch whose driver version is **23.19.x** even in the
  newest release (26.5.2). It never crosses 23.30, so no upgrade path exists on this card.

**Do not bypass ffmpeg's version check.** It guards a path upstream found to produce incorrect
output, and on Vega there is no working 10-bit encoder behind it regardless.

## What the 8-bit AMD testing DOES cover

Bitstream structure is not bit-depth dependent, so the 8-bit runs exercise most of what differs
between vendors. Measured on the Vega box with `hevc_amf` (ffmpeg `N-126242`):

- **AMF honours `gop_size` exactly** — `-g 15/30/120` produced IDR intervals of 15, 30 and
  one-GOP respectively.
- **AMF emits TRUE IDRs** (`IDR_N_LP`), zero `CRA_NUT`, zero RASL. AMD needs **no** open-GOP
  flag. `libx265` is the badly-behaved one: at `-g 30` it emits CRA + RASL — an open GOP that
  does not reset the reference chain — unless `open-gop=0` is also set.
- **AMF emits an access-unit delimiter (NAL 35) per picture. NVENC and x265 emit none.** Our
  splicer is safe against this by construction: it classifies by NAL type and indexes slices by
  `first_slice_segment_in_pic_flag` rather than position, and copies every non-VCL NAL through
  untouched. Anything that changes that indexing to be positional would break on AMD.

## What 8-bit testing does NOT cover

The untested cell is specifically **AMF's 10-bit output shape** — not "10-bit" in general, since
the client already decodes and splices Main10 from NVENC every day. The residual risks are:

1. Main10 SPS fields (`bit_depth_luma_minus8`, profile 2) from AMF — small; the syntax is
   vendor-independent and already handled.
2. HDR10 SEI (mastering display / content light level) inventory and placement from AMF —
   copied through as non-VCL, but never observed.
3. An AMD-specific bug in the 10-bit path — the ffmpeg guard is evidence this class has existed.

**Closing it needs an RX 5000-series or newer Radeon.** Nothing else will do it.

## Headless hosts — no monitor, no dummy plug

A host can run with **only a network connection**. Apollo bundles an *indirect* display driver
(SudoMaker SudoVDA), and an indirect display is root-enumerated (`ROOT\DISPLAY\0000`) — it
needs no GPU output, no cable and no EDID, so nothing has to be plugged into the card.

Without one you get the failure signature seen on 2026-08-23: the only display is
`WinDisc 1024x768` and the streamed pane arrives as **flat grey**.

**Apollo's bundled VDD installer is BROKEN as shipped** (found 2026-08-23, still true in this
fork). `drivers/sudovda/` contains `install.bat`, `SudoVDA.inf`, `sudovda.cat`, `sudovda.cer`
— and **not `SudoVDA.dll`**, the actual driver binary. So `pnputil /add-driver` fails with
"The system cannot find the file specified", and the bundled `install.bat` cannot work anyway
because it drives `nefconc.exe`, which is also not shipped.

Installing it needs two things the package does not give you:

1. **`SudoVDA.dll`** (83,216 bytes) alongside the INF. It is covered by the shipped `.cat`, so
   once present the package validates and `pnputil /add-driver <inf> /install` succeeds.
2. **A root device node**, which `pnputil` cannot create — it installs driver PACKAGES, not
   device NODES. `nefconc`/`devcon` do this; with neither available, SetupAPI does it directly:
   `SetupDiCreateDeviceInfo` → set `SPDRP_HARDWAREID` to `Root\SudoMaker\SudoVDA` (REG_MULTI_SZ,
   double-NUL terminated) → `SetupDiCallClassInstaller(DIF_REGISTERDEVICE)`.

Verify with `Get-PnpDevice -Class Display`: the adapter should read **OK / CM_PROB_NONE**.
`pnputil /remove-device "ROOT\DISPLAY\000n"` removes a duplicate node if one gets created.

**Fix for release:** ship `SudoVDA.dll` in `drivers/sudovda/`, and replace the `nefconc`
dependency in `install.bat` — otherwise every user installing on a headless box hits this.

## Recommended posture for a release

Advertise AMD support as **verified at 8-bit SDR**. Gate HDR-on-AMD behind a capability token
that defaults OFF, whose absent-branch is "SDR co-op" rather than an HDR attempt that may fail
in the field. That follows the project's own rule: one owning box per capability, every
cross-boundary dependency negotiated with a named absent-branch, and never silently wrong.
