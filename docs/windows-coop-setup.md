# Windows Eclipse client and Umbra co-op host setup

This guide contains no machine-specific addresses, UUIDs, application IDs, display IDs, or
install paths. Eclipse discovers those values from each paired host. Do not copy them from
another PC: Umbra application IDs and display identities belong to the host that created them.

## Install the client

The x64 client requires Windows 10 version 1803 or newer (Windows 11 is supported). Co-op uses
Windows' native `AF_UNIX` inter-process transport, which is not available on older releases.
Regular and co-op streaming also require a physical GPU and driver that support D3D11VA for the
selected H.264 or HEVC stream. Eclipse fails closed when hardware decoding is unavailable; it
does not silently turn a hardware test into a software-decoder test.

For packaged installation, use the [Windows installation guide](https://github.com/PreceptorOfMagic/Eclipse-Umbra-Project-Hub/blob/main/docs/installation.md#eclipse-windows). The following steps are for a developer source build:

1. Check out the [current Eclipse integration branch](https://github.com/PreceptorOfMagic/Eclipse/tree/diag/feed-accounting) with its submodules.
2. Run that branch's [portable Windows build script](https://github.com/PreceptorOfMagic/Eclipse/blob/diag/feed-accounting/scripts/windows/build_portable.ps1). It records the source state, assembles the runtime dependencies, licences, notices and manifest, and writes an unsigned ZIP plus adjacent SHA-256 file under `dist/`.
3. Keep the generated directory intact. Do not run the executable from inside its archive or separate it from its DLLs, runtime data, licence files, notices, source manifest, or CA bundle.
4. Verify the adjacent archive hash produced by the build, then launch the client as a normal user. Administrator privileges are not required.

The internal Windows artifact is **not code-signed**, so Windows SmartScreen can show an unrecognised-app warning. A future public release must identify that status explicitly and provide the build information, source manifest, third-party notices, per-file hashes and adjacent archive hash from the same reviewed build.

Eclipse stores each user's settings, host pairings, keys, cache, and logs in that user's normal
Windows application-data directories. The extracted package is not expected to be writable and
does not depend on MSYS2, a compiler, Git, or the directory in which it was built.
The diagnostic log is `%APPDATA%\com.limelight\moonlight-tv\aurora-diag.log`; the legacy
`aurora` name is retained in this compatibility path so existing users keep their data.

The current renderer decodes through D3D11VA on the GPU, then performs a GPU-to-CPU frame
readback and converts the result to I420 for SDL presentation. It is hardware decoding, but it is
not yet a zero-copy D3D11 presentation path. Before exposing a codec, Eclipse checks the selected
physical adapter for the H.264 or HEVC D3D11 decoder profile and required NV12 output format. A
Moonlight-style embedded trial-bitstream decode has not landed yet. The first live decoded frame
must still carry the required D3D11 hardware format and hardware-frames context; otherwise the
stream fails closed instead of falling back to software.

`ECLIPSE_DIAGNOSTIC_VIDEO_DECODER=software` is an explicit troubleshooting override only. A run
made with that environment variable is not a valid Windows functional test or performance result;
remove it before testing regular or co-op streaming.

## Prepare each streaming host

Follow the [Umbra installer instructions](https://github.com/PreceptorOfMagic/Eclipse-Umbra-Project-Hub/blob/main/docs/installation.md#umbra-windows) on both PCs. The installer includes the required device/display components.

The release workflow uses separate working co-op defaults and creates the Co-op Pane through first use. Do not manually create this application, copy host IDs or edit encoder/display settings to complete normal installation. If a development build still requires those steps, it has not met the automatic-setup release requirement.

## Pair and start a session

1. Put the client and both hosts on reachable networks and allow Umbra through Windows
   Firewall. Pair each host in Eclipse once using the PIN shown by the client.
2. Verify regular streaming from each host before attempting co-op. This isolates host capture,
   encoder, audio, firewall, and controller issues from co-op composition.
3. From Eclipse's server screen choose **Co-op**, select two different paired hosts, choose the
   orientation and primary side, review controller/audio assignments, then start the session.
4. If more than two hosts are paired, choose the pair by their displayed host names. Eclipse
   persists host identities internally; it does not rely on a fixed host order or address.

Do not select the same host twice. The PC that is running Eclipse CAN be one of the two co-op
hosts (tested 2026-09-27 with an NVIDIA PC running Eclipse plus an AMD PC, both orientations,
either PC primary). Eclipse tells that PC's display hook the session is local: the physical
monitor stays on for Eclipse, the game runs on the host's virtual display (made primary), and
Eclipse's own window is returned to its monitor if Windows moves it -- so the pane never captures
Eclipse itself. Current limits of a local-host session:

- Input to the local pane: Eclipse does not forward input to its own PC (that would loop). A
  controller for the local pane is read by the game directly; keyboard and mouse for the local
  pane need the game focused on the virtual display (a focus hand-off is not built yet). Keyboard
  and mouse DO follow the pointer into the OTHER PC's pane.
- Windows may move your other open windows onto the virtual display for the length of the session
  (they return when it ends).
- Developer integration requirement: the installer must wire the local-session display hook;
  without it the physical monitor can be switched off as for a TV session. This is not an extra
  user installation step.

## Troubleshooting

- **A host is absent:** make sure Umbra is running and the firewall permits it. If network
  discovery is filtered by a VPN or router, use Eclipse's manual add control with that host's
  current name or address; do not edit the preferences file.
- **Co-op says the app is missing:** confirm both hosts use the matching Umbra release,
  reconnect and retry. If automatic pane creation fails, collect logs and report it; do not
  create or rename an application entry as part of installation.
- **A pane is black or shows the wrong desktop:** test a normal Desktop session on that host,
  then collect logs from the failing co-op attempt. Include whether Eclipse runs on a host PC.
- **Resolution or refresh is wrong:** report the requested mode, both releases and the
  virtual-display result. Installed defaults should be client-driven; do not copy display
  GUIDs or output numbers from another machine.
- **Input reaches the wrong host:** review Eclipse's co-op controller assignments. Each
  controller is assigned to a paired host identity, not to a machine name or IP address.
- **The stream fails at decoder startup:** inspect `aurora-diag.log` for the D3D11VA adapter and
  fail-closed message. Confirm the GPU driver is installed and remove the diagnostic software
  override. A codec absent from Settings failed the adapter profile/NV12 preflight; a later
  startup failure is reported by the first-live-hardware-frame gate.
