# Eclipse

**The screen-side app: everyday game streaming and two-host couch co-op on LG webOS, Android TV (NVIDIA SHIELD), Windows and Linux.**

[Project Hub](https://github.com/PreceptorOfMagic/Eclipse-Umbra-Project-Hub) · [Installation Guide](https://github.com/PreceptorOfMagic/Eclipse-Umbra-Project-Hub/blob/main/docs/installation.md) · [Eclipse](https://github.com/PreceptorOfMagic/Eclipse) · [Umbra](https://github.com/PreceptorOfMagic/Umbra) · [Development](https://github.com/PreceptorOfMagic/Eclipse-Umbra-Project-Hub/blob/main/docs/development.md) · [Acknowledgements](https://github.com/PreceptorOfMagic/Eclipse-Umbra-Project-Hub/blob/main/ACKNOWLEDGEMENTS.md)

New here? [The Project Hub](https://github.com/PreceptorOfMagic/Eclipse-Umbra-Project-Hub) explains the setup, co-op, hardware coverage and both applications. This repository owns Eclipse source and client release files. Eclipse descends from Moonlight, Moonlight TV and Aurora.

## Downloads and installation

Open [Eclipse releases](https://github.com/PreceptorOfMagic/Eclipse/releases), choose the latest compatible release and expand **Assets**. Download the attached package for your device—not GitHub’s generated **Source code** archives. If no release/package is listed, that platform’s installer has not been published.

| Device | Asset to choose | Install |
|---|---|---|
| LG webOS TV | Standard `com.aurora.gamestream_<version>_arm.ipk` (not beta) | [LG TV steps](#lg-webos) |
| Windows x64 | Installer `eclipse-windows-x64-setup-unsigned.exe` (or portable `eclipse-windows-x64-unsigned.zip`) + checksum | [Windows steps](#windows-x64) |
| Linux x86_64 | `eclipse-linux-x86_64.tar.gz` + checksum | [Linux steps](#linux-x86_64) |
| Android TV / NVIDIA SHIELD | `eclipse-android.apk` + checksum | [Android TV steps](#android-tv-and-nvidia-shield) |

### LG webOS

1. Follow [LG’s Developer Mode setup](https://webostv.developer.lge.com/develop/getting-started/developer-mode-app) on the TV.
2. Install [webOS Dev Manager](https://github.com/webosbrew/dev-manager-desktop/releases/latest) on your computer (on webOS 26, version 1.99.20 or newer) and connect the TV using its Developer Mode address/passphrase.
3. Download the standard Eclipse IPK above, choose **Apps → Install** in Dev Manager and select the file.
4. Launch Eclipse on the TV. Keep the Developer Mode session active; close/reopen Eclipse after updates.

[Full LG installation and prerequisites](https://github.com/PreceptorOfMagic/Eclipse-Umbra-Project-Hub/blob/main/docs/installation.md#eclipse-webos). Physical testing: LG G5 on webOS 26; other models/versions need reports.

### Windows x64

1. Download the installer and its matching SHA-256 file from the same release; compare with `Get-FileHash .\eclipse-windows-x64-setup-unsigned.exe -Algorithm SHA256`.
2. Run it as your normal Windows user. It installs Eclipse for your user only, without administrator access, adds Start menu entries and an uninstaller, and keeps settings and pairings when installed over an earlier version.
3. For portable use instead, extract `eclipse-windows-x64-unsigned.zip` with **Extract All**, keep every DLL/data folder together and run `moonlight-tv.exe` (the inherited executable name is expected).

Both packages are unsigned; check its provenance and checksum before responding to a Windows warning. Signing through SignPath Foundation is being set up: see the [Code signing policy](CODE_SIGNING_POLICY.md). Windows 11 is tested. The source minimum is Windows 10 version 1803, but its coverage is not equivalent. A compatible D3D11VA GPU/driver is required.

[Full Windows installation](https://github.com/PreceptorOfMagic/Eclipse-Umbra-Project-Hub/blob/main/docs/installation.md#eclipse-windows).

### Linux x86_64

1. Download the archive and its matching checksum; compare with `sha256sum eclipse-linux-x86_64.tar.gz`.
2. Extract it with `tar -xzf eclipse-linux-x86_64.tar.gz` and enter `eclipse-linux-x86_64`.
3. Run `./install.sh` to install Eclipse for your user and add it to the applications menu; installing a newer version the same way keeps settings and pairings.
4. Or run `./eclipse` in place (keep the directory together; `./install-desktop-entry.sh` adds that copy to the menu).

Requires glibc 2.35+ (Ubuntu 22.04 baseline), a graphical desktop and working GPU drivers. Current Linux tests are WSL-based with NVDEC; native Linux and AMD/Intel VAAPI still need validation.

[Full Linux installation](https://github.com/PreceptorOfMagic/Eclipse-Umbra-Project-Hub/blob/main/docs/installation.md#eclipse-linux).

### Android TV and NVIDIA SHIELD

1. Download the APK and its matching SHA-256 file from the same release.
2. Sideload it: with the **Downloader** app on the TV (allow it under **Unknown sources**), or from a computer with `adb connect <TV address>:5555` then `adb install eclipse-android.apk`.
3. Launch Eclipse from the TV's apps row. Install updates the same way; your hosts and settings are kept.

Requires Android 9 or newer (on a SHIELD TV, SHIELD Experience 8 or later). The SHIELD TV is the primary target: tested on a 2019 SHIELD TV running SHIELD Experience 9 (Android 11) with streaming, two-PC co-op, the SHIELD remote and Xbox and Nintendo Switch Pro controllers. The other SHIELD models run the same software; reports are welcome. There is no Play Store listing.

[Android TV installation, remote keys and building](docs/BUILD_ANDROID.md).

## What Eclipse provides

- Discover or add an Umbra host, complete PIN pairing and launch its games or desktop.
- Choose decoder-aware 720p, 1080p, 1440p, 3584×2016 “3.6K”, 4K, native or custom resolutions.
- Select frame rates from 30 to 240 fps where the active decoder permits them.
- Use automatic bitrate or a manual 5–400 Mbps selector; the negotiated protocol path may impose a lower effective limit.
- Use H.264 or HEVC on desktop targets, with capability-gated AV1 and HDR10 paths on compatible webOS hardware and HDR10 on Android TV devices whose decoder and TV support it.
- Send gamepad, keyboard and mouse input, including supported rumble, trigger rumble, touchpad, gyro, accelerometer and RGB capabilities.
- Open the on-screen keyboard, use the gamepad virtual mouse and inspect live codec, size, frame rate, latency, drop and bitrate statistics.
- Hold Start on any controller for the shortcut wheel (stream menu, virtual mouse, keyboard, performance); the button combinations still work, and either can be turned off in Settings > Input.

## First stream and co-op

1. Install [Umbra](https://github.com/PreceptorOfMagic/Umbra) on the gaming PC. Discover/add it in Eclipse and approve Eclipse’s PIN on the host.
2. Launch a normal game or desktop and verify picture, audio and input.
3. For co-op, install a compatible Umbra release on a second PC and test it individually.
4. Choose **Co-op** in Eclipse, select both PCs and layout, assign players/controllers and start. Matching host/client releases negotiate the encoder path; no manual Co-op Pane application or hidden encoder settings are part of setup.

Each PC runs its own game session. See the [Project Hub](https://github.com/PreceptorOfMagic/Eclipse-Umbra-Project-Hub) for the full system map and current hardware matrix.

## Reporting and contributing

Use **Settings → About → Keep diagnostic logs**, restart, reproduce the issue, then **Share logs…**. Follow the [log-export instructions](https://github.com/PreceptorOfMagic/Eclipse-Umbra-Project-Hub/blob/main/docs/installation.md#diagnostics), review private details and attach the bundle to [Eclipse issues](https://github.com/PreceptorOfMagic/Eclipse/issues/new/choose). Include exact versions, hardware and whether a normal stream works.

[Development and build entry points](https://github.com/PreceptorOfMagic/Eclipse-Umbra-Project-Hub/blob/main/docs/development.md) · [Acknowledgements](ACKNOWLEDGEMENTS.md)

[Eclipse/Umbra licence & notices](LICENSES.md) · [GPL-3.0](LICENSE.txt)
