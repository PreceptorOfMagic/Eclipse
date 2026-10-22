Eclipse 1.3.5 fixes installing on older LG TVs, brings the SHIELD's controller-mouse fixes to every platform, adds a hold-Start shortcut wheel and gives the Windows and Linux app a new icon. Pair it with **Umbra 0.5.0** on the host PCs.

**New: Eclipse for Android TV and the NVIDIA SHIELD** (added to this release on 22 October). See [Android TV and NVIDIA SHIELD](#android-tv-and-nvidia-shield) below.

The webOS, Windows and Linux packages are built from `21718d0d` (tag `eclipse-v1.3.5`) and report version 1.3.5 and that commit. The Android APK is built from `433a50d4` (tag `eclipse-android-v1.3.5`); see below.

## Downloads

| Device | Asset | Notes |
|---|---|---|
| LG webOS TV | `com.aurora.gamestream_1.3.5_arm.ipk` | Install with webOS Dev Manager (Developer Mode). The app id stays `com.aurora.gamestream`, so an earlier install keeps its settings and pairings. |
| Windows x64 (installer) | `eclipse-windows-x64-setup-unsigned.exe` | Per-user install, no administrator rights, no background service. Upgrades an existing install in place. |
| Windows x64 (portable) | `eclipse-windows-x64-unsigned.zip` | Extract everything and run `moonlight-tv.exe` (inherited executable name). |
| Linux x86_64 | `eclipse-linux-x86_64.tar.gz` | Extract, then `./install.sh` (per-user) or run `./eclipse` in place. Needs glibc 2.35+ (Ubuntu 22.04 baseline). |
| Android TV / NVIDIA SHIELD | `eclipse-android.apk` | Sideload with the Downloader app or `adb install`. Android 9 or newer; one APK for arm64-v8a, armeabi-v7a and x86_64. |

Each asset has a matching `.sha256` file, and `SHA256SUMS` covers everything. Source archives are attached for the exact build (see **Source** below).

## What's in this release

- **Installs on older LG TVs again.** Every file inside the 1.3.4 TV package was dated 1 January 1970. The packaging tool lost the dates under Node 22, and LG's package check on webOS 5-era TVs rejects such files with "error -5". 1.3.5 packages carry real dates, and nothing else in the package changed.
- **Controller mouse, everywhere.** The fixes first made for the NVIDIA SHIELD now apply on webOS, Windows and Linux too:
  - In co-op, the controller's mouse pointer crosses the divider like the Magic Remote's. Past the middle it is the second PC's pointer, and clicks and scrolling go to the PC it is over.
  - The pointer stops when you let go of the stick, and a trigger that snaps back lets go of the mouse button.
  - On Windows and Linux, pulling a trigger all the way now clicks. Before, a full pull was read as no pull.
  - When this Windows PC is itself one of the two co-op PCs, the controller mouse keeps its old behaviour.
- **Hold Start for a shortcut wheel** on any controller: Menu, Mouse, Keyboard or Performance. Pick with a stick or the d-pad, then let go of Start. **B** goes back to Continue. The usual button combinations still work. Choose combos, the wheel, both or neither in **Settings → Input → Controller shortcuts**.
- **New Windows and Linux icon:** the Eclipse tile replaces the grey wheel inherited from Moonlight.

## Tested

- **Package contents:** every package reports version 1.3.5 and commit `21718d0d`. The TV package's 105 entries all carry real dates, and its app folders keep the same permissions as 1.3.4. Compared with 1.3.4, the Linux package's third-party libraries are byte-identical, and both packages' source manifests differ only in the Eclipse commit.
- **Automated tests:** the desktop suite passes (26 of 26). That includes 12 cases for the co-op controller mouse, one of which fails without the trigger fix on x86.
- **LG G5, webOS 26 (11.2.0):** a TV test build of the same code (the beta app id) installed over the earlier beta, kept its settings, and ran a live two-PC co-op stream of PC1 and PC2 with both desktops filling their halves.
- **Linux (WSL):** `install.sh` upgraded an existing 1.3.4 install. `hosts.ini` and `moonlight.ini` were unchanged by the upgrade, and the app started and ran without errors. On its first run it saved `moonlight.ini` with the new shortcut setting.
- **Windows:** the packages were built and checked as above; installing them over 1.3.4 has not been tested yet.
- **Not yet tried with a real controller on webOS, Windows or Linux:** the controller-mouse changes and the shortcut wheel. On the SHIELD the maintainer tried the wheel and the co-op pointer by hand; the stick-release fix has not been tried with a real controller anywhere.
- **Older LG TVs:** this release's date fix matches the published cause of the webOS 5 "-5" install failure. It has not been tried on a webOS 5 TV here.

## Android TV and NVIDIA SHIELD

The first release of Eclipse for Android TV. The NVIDIA SHIELD TV is the main target. It has ordinary streaming, two-PC co-op, the controller mouse and the hold-Start wheel, like the other platforms.

The APK is built from its own tag, `eclipse-android-v1.3.5` (`433a50d4`). That is the 1.3.5 code plus the Android port, and the port also changes code shared with the other platforms, including co-op video splicing and audio, controller mapping and fonts. The webOS, Windows and Linux 1.3.5 packages do not have those shared changes yet; they come to every platform in the next release.

**Install:**
1. Download `eclipse-android.apk` and its `.sha256`.
2. Sideload it, either with the **Downloader** app on the TV (allow it under **Unknown sources**) or from a computer with `adb connect <TV address>:5555`, then `adb install eclipse-android.apk`.
3. Launch Eclipse from the TV's apps row.

Full steps, remote keys and controller notes: [Android TV guide](https://github.com/PreceptorOfMagic/Eclipse/blob/main/docs/BUILD_ANDROID.md).

**Requirements:** Android 9 (API 28) or newer. On a SHIELD TV that means SHIELD Experience 8 or later. There is no Play Store listing.

**Signed:** unlike the other packages, the APK is signed, because Android installs only signed apps. It uses the project's own release key, and every future update is signed with the same key. The certificate's SHA-256 is `8C:31:18:05:69:36:39:EF:2A:F8:28:63:9A:31:DE:9B:F4:DB:CC:28:AA:2A:5B:98:89:F3:E6:04:75:26:EF:AC`. Check it with `apksigner verify --print-certs eclipse-android.apk`. An APK you built yourself is signed with a different key, so it cannot update this one, or the other way round, without uninstalling, which removes paired hosts and settings.

**Tested:**
- A 2019 SHIELD TV running SHIELD Experience 9 (Android 11), with live streams from the project's Umbra PCs. The tests covered single-PC streaming, two-PC co-op, the SHIELD remote, and Xbox and Nintendo Switch Pro controllers.
- The other SHIELD models and other Android TV devices run the same APK but have not been tried. Reports are welcome.
- Automated: the APK's host tests and device self-tests are described in the [guide](https://github.com/PreceptorOfMagic/Eclipse/blob/main/docs/BUILD_ANDROID.md#6-testing).

**Known issues (Android):**
- **The SHIELD keeps the Xbox button for itself.** Its system turns the Guide button into Home before any app sees it. Hold **View** for 2 seconds to send Guide to the PC, or use **Guide button** in the stream menu.
- **Nintendo Switch Pro Controller:** Eclipse follows the letters printed on the buttons, while the SHIELD's own menus go by position, so the two disagree on A/B and X/Y. **Settings → Input → Nintendo controllers** switches Eclipse to positions.
- **Co-op sound runs about 35 ms later than on the other platforms.** It is a temporary measure that stops crackling on the SHIELD while the audio filter is made cheaper to run.
- **4K at high bitrates:** in one test on 16 October, a single 4K stream at about 120 Mbps lost frames whenever the picture got busy. Later 4K co-op streams at about 107 Mbps ran at 60 fps. If a 4K stream stutters, lower the bitrate.

## Known issues

- **Windows packages are unsigned.** Windows SmartScreen may warn. Check the SHA-256 before running. Code signing through SignPath Foundation is being set up.
- **Some games pick music or other sound clips at random on each PC.** The two PCs then play different clips at the same moment, which the filter cannot merge, so you hear both. Halo: Combat Evolved does this with its music. Turn the game's music off on one PC if it sounds doubled.
- **Umbra 0.5.0 does not report its graphics card,** so Eclipse asks once for each PC after it is paired.
- **After a TV restart, until Eclipse next starts,** other Developer Mode apps could read or change its files. Eclipse locks them again when it starts.
- Only the LG G5 on webOS 26 has been physically tested. Other LG models and firmware versions need reports. Linux is tested under WSL; native Linux with AMD/Intel VA-API is not yet validated.

## Source

- `eclipse-project-source-21718d0d.tar.gz`: this repository at `21718d0d`, with its pinned dependency modules and their repository-owned patches already applied.
- `eclipse-assets-source.tar.gz`: original asset sources for this build, now including the SVG the new desktop icon is rendered from. Its project files are taken from `21718d0d`. Three files in the 1.3.4 copy (`moonlight.png`, `moonlight.h` and `build_portable.sh`) came from an earlier development revision; they now match the source this release was built from.
- `eclipse-linux-webos-external-source.tar.gz`, `eclipse-windows-external-source.tar.gz`: third-party library sources, unchanged from 1.3.4 because no third-party source input changed. Read each archive's manifest for build recipes and provenance limits.
- **Android APK:**
  - `eclipse-project-source-433a50d4.tar.gz`: this repository at `433a50d4` (tag `eclipse-android-v1.3.5`), with its pinned dependency modules and their patches applied, and `scripts/android` (the APK's build and its SDL patches).
  - `eclipse-android-external-source.tar.gz`: the upstream source archives of the eight third-party libraries compiled into the APK, each checked against the SHA-256 the build pins.
  - `eclipse-android-assets-source.tar.gz`: the vector source of the split-screen mark that the Android launcher icon and TV banner show.

The hold-Start shortcut wheel is a port of the Start gesture from [Moonlight V+](https://github.com/qiin2333/moonlight-vplus) by qiin2333 and contributors (GPL-3.0). The webOS, Windows and Linux packages' `ACKNOWLEDGEMENTS.md` does not yet name it (the Android APK's does); the repository's does, from commit `d56e3158`.

Eclipse is a fork of moonlight-tv (GPL-3.0). Upstream names that remain in identifiers and file names (Moonlight, `moonlight-tv.exe`, `com.aurora.gamestream`) are kept for compatibility. This project is developed with AI assistance: Claude and Codex write and debug code under the maintainer's direction and hands-on testing. See the Project Hub for details.
