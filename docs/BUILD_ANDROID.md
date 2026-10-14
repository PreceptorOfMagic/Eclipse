# Eclipse for Android TV and NVIDIA SHIELD

Eclipse for Android TV is the same program as on webOS, Windows and Linux: the C client, its LVGL interface,
co-op and every setting are built from this repository with the Android NDK. The only Android-specific parts are
platform glue (`src/app/platform/android`), a small activity (`deploy/android`) and a video module that drives
Android's MediaCodec (`third_party/ss4s/modules/android`, carried in `patches/client-deps/ss4s.patch`).

It is distributed by **sideloading** only, like the rest of the project: there is no Play Store listing, no
developer account and no fee. Google's developer-verification requirement for sideloaded apps does not apply to
Android TV devices, and installs over ADB are exempt everywhere.

> **Status.** The port is new. Its host tests pass; the APK build and emulator smoke tests run in CI
> (`.github/workflows/build-android.yml`). It has **not yet been tested on SHIELD hardware**: please report results
> (see [Reporting](#reporting)).

---

## Contents

1. [Devices](#1-devices)
2. [Installing (sideloading)](#2-installing-sideloading)
3. [Using Eclipse on a TV](#3-using-eclipse-on-a-tv)
4. [Building](#4-building)
5. [Signing](#5-signing)
6. [Testing](#6-testing)
7. [How it works](#7-how-it-works)
8. [Troubleshooting](#8-troubleshooting)

---

## 1. Devices

- **Android 9 (API 28) or newer**, Android TV or Google TV. It also runs on phones and tablets with a game
  controller, keyboard or mouse, but it is designed for TVs.
- **NVIDIA SHIELD TV** (2015, 2017, 2019 and Pro) on SHIELD Experience 9 (Android 11) is the primary target.
- One APK holds `arm64-v8a`, `armeabi-v7a` (many TV sticks run 32-bit Android on a 64-bit CPU) and `x86_64`.
- Video: H.264 and HEVC through the device's hardware decoder. HDR10 is available when the decoder supports Main 10
  and the TV reports HDR10, as on webOS.

## 2. Installing (sideloading)

Download `eclipse-android.apk` and its `.sha256` from the
[releases](https://github.com/PreceptorOfMagic/Eclipse/releases). If no APK is attached to a release, the Android
package has not been published yet; build it yourself ([section 4](#4-building)).

### With the Downloader app (on the TV, no computer)

1. On the TV, install **Downloader** (by AFTVnews) from the Play Store.
2. Allow Downloader to install apps. Android TV and SHIELD: **Settings → Device Preferences → Security &
   restrictions → Unknown sources → Downloader**. Google TV: first enable developer options (**Settings → System →
   About**, select **Android TV OS build** seven times), then **Settings → Apps → Security & restrictions →
   Unknown sources → Downloader**.
3. Open Downloader, enter the APK's download URL from the release page, and choose **Install**.
4. Eclipse appears in the TV's apps row. You can turn **Unknown sources** off again afterwards.

### With ADB (from a computer)

1. On the TV, enable developer options: **Settings → Device Preferences → About**, select **Build** seven times.
2. **Settings → Device Preferences → Developer options**: turn on **Network debugging** (SHIELD) or
   **USB debugging** / **Wireless debugging**.
3. On the computer, install [Android platform tools](https://developer.android.com/tools/releases/platform-tools)
   and run:

   ```sh
   sha256sum -c eclipse-android.apk.sha256      # Windows: Get-FileHash eclipse-android.apk -Algorithm SHA256
   adb connect <TV IP address>:5555             # accept the prompt on the TV
   adb install eclipse-android.apk
   ```

### Updating

Install the newer APK the same way; paired hosts and settings are kept. Android only accepts an update signed with
the **same key** as the installed app: a locally built APK (signed with your own debug key) cannot update a release
install, or the other way round. Uninstalling removes the paired hosts and settings.

## 3. Using Eclipse on a TV

Everything works as on the other platforms: discovery, PIN pairing with Umbra, launching, every stream setting,
co-op with two hosts, controllers (with rumble, motion and touchpads where Android exposes them), keyboard and
mouse, the on-screen keyboard and the statistics overlay.

### Remote

The keys mean what they mean on the webOS remote wherever the remotes share them.

| Key | In menus | During a stream |
|---|---|---|
| D-pad, OK | Navigate, select | Arrow keys, Enter. Holding OK is a right click. |
| Back | Back | Tap: Escape on the host. Hold: open the stream menu. Closes the on-screen keyboard first. |
| Red, Yellow, Blue | As on the webOS remote | Red: open the stream menu. Blue: open the on-screen keyboard. Yellow: nothing. |
| Menu (SHIELD ≡) | As the webOS Yellow key | Open the stream menu |
| Info, Guide | | Open the stream menu |
| Channel + / − | | Page Up / Page Down |
| Green | | Nothing (as on webOS) |

A full keyboard's Menu key is the host's Applications key, as on a PC.

### Display

When a stream starts, Eclipse asks the TV to switch to the stream's refresh rate (or the rate chosen in
**Settings → Basic**) at the TV's current resolution, and returns it to the system's choice afterwards: the webOS
panel does the same itself. Some devices only switch when their own frame-rate matching option is on (SHIELD:
**Settings → Device Preferences → Display & Sound → Advanced settings → Match content frame rate**).

The resolution and frame rate lists are limited to what the TV's current mode and the decoder support, as on
webOS.

### Differences from the other platforms

These follow from what Android allows an app to do, not from missing work:

- **Audio output device**: not offered. Android routes a TV app's sound itself (HDMI, Bluetooth), and the device
  list it gives SDL has no names.
- **Use the wired network**: the host-side wired pin needs the network card's hardware address, which Android hides
  from apps. Eclipse behaves as on a device without a wired adapter; Android itself prefers Ethernet when it is
  connected.
- **Diagnostic logs** are shared through the same local web page as on webOS
  (**Settings → About → Keep diagnostic logs**, then **Share logs…**), because a TV app's files are out of reach.
  With ADB, `adb logcat -s Eclipse` shows the live log.

## 4. Building

Requirements (Linux or macOS; on Windows use WSL):

- Android SDK with **platform 35** and **build-tools 35**, and **NDK r27** (`27.2.12479018`, as CI uses) —
  e.g. from Android Studio's SDK Manager or `sdkmanager "ndk;27.2.12479018" "platforms;android-35" "build-tools;35.0.0"`
- JDK 17 or newer
- `cmake` 3.22+, `ninja`, `pkg-config`, `gettext` (`msgfmt`), `python3`, `curl`, `git`

```sh
export ANDROID_HOME=~/Android/Sdk
export ANDROID_NDK_HOME=$ANDROID_HOME/ndk/27.2.12479018
./scripts/android/build_apk.sh                  # all three ABIs -> dist/eclipse-android*.apk (+ .sha256)
./scripts/android/build_apk.sh --abi arm64-v8a  # one ABI, faster (fine for a SHIELD)
```

The first build compiles the dependencies (`scripts/android/build_deps.sh`: SDL2, SDL2_image, mbedTLS, curl, Opus,
inih, expat, FreeType) from the same pinned, hash-checked source archives as the Linux and Windows packages, into
`build/android-deps`. Later builds reuse them.

Options: `--debug` (a debuggable build), `--clean`, `--require-clean-tree` (release builds), `--selftest`
([section 6](#6-testing)).

Outputs:

| File | What |
|---|---|
| `dist/eclipse-android.apk` | Release build signed with the project key ([section 5](#5-signing)) |
| `dist/eclipse-android-debug-signed.apk` | Release build signed with your local debug key (no key configured) |
| `build/android-symbols/<abi>/` | Unstripped libraries, for symbolising crash records |

The script checks what it packages: every library a packaged binary needs is packaged or is an Android system
library, every file in `lib/` is installable, and the co-op peer finds its libraries beside it.

## 5. Signing

Every APK must be signed, and every update must be signed with the same key. Create the project's key once and
keep it (and a backup) private:

```sh
keytool -genkeypair -v -keystore eclipse-release.jks -alias eclipse -keyalg RSA -keysize 4096 -validity 36500
```

Build with it:

```sh
export ECLIPSE_ANDROID_KEYSTORE=/path/to/eclipse-release.jks
export ECLIPSE_ANDROID_KEYSTORE_PASSWORD=... ECLIPSE_ANDROID_KEY_ALIAS=eclipse ECLIPSE_ANDROID_KEY_PASSWORD=...
./scripts/android/build_apk.sh --clean --require-clean-tree
```

For the release workflow (`.github/workflows/release.yml`, job `release-android`), store the key as repository
secrets: `ECLIPSE_ANDROID_KEYSTORE_BASE64` (`base64 -w0 eclipse-release.jks`), `ECLIPSE_ANDROID_KEYSTORE_PASSWORD`,
`ECLIPSE_ANDROID_KEY_ALIAS` and `ECLIPSE_ANDROID_KEY_PASSWORD`.

If the key is lost, users must uninstall Eclipse (losing their pairings) before they can install a build signed with
a new one.

## 6. Testing

**Host tests** run with the desktop build (`ctest -R test_android`): the translation catalogue reader against
gettext, the UUID generator, the logcat backend, and the MediaCodec module's feeding against real H.264 and HEVC
streams, including a co-op composite made by the shipping splice code, decoded by FFmpeg in strict mode.

**Device smoke tests** run the real APK on an emulator (CI) or a device. They use a self-test build, which installs
**beside** a real Eclipse (its own application ID, `io.github.preceptorofmagic.eclipse.selftest`) without touching
its hosts or settings:

```sh
./scripts/android/build_apk.sh --selftest --abi arm64-v8a       # dist/eclipse-android-selftest.apk
cc -O2 -o hevc_tilecols tests/coop/hevc_tilecols_cli.c src/app/stream/video/coop_splice.c \
   src/app/stream/video/coop_poc.c -Isrc/app/stream/video
adb connect <SHIELD IP>:5555
tests/android/device_smoke.sh dist/eclipse-android-selftest.apk ./hevc_tilecols smoke --uhd
adb uninstall io.github.preceptorofmagic.eclipse.selftest
```

They check that the interface starts and survives d-pad input; that the co-op peer executable runs from the
installed package; and that a co-op composite, H.264, a narrow letterboxed HEVC picture and (with `--uhd`, for real
hardware) 4K HEVC Main 10 play through the MediaCodec module onto the video surface with no codec errors. Logs and
screenshots are left in `smoke/`. FFmpeg with libx264 and libx265 is needed to make the test streams.

Stream tests against an Umbra host (picture, sound, input, latency, co-op with two hosts) need a person at the TV;
follow the same checklist as for a webOS release.

## 7. How it works

- **Process and UI.** SDL2's `SDLActivity` (subclassed by `EclipseActivity`) starts the client's normal `main`
  from `libmain.so`. LVGL draws with SDL's renderer, exactly as on the desktop.
- **Video** is decoded by MediaCodec straight onto a `SurfaceView` **under** the interface, which is drawn with an
  alpha channel: the same plane model as webOS, so the interface composites over the picture without copies. The
  decoder is put in low-latency mode where the device has one, and newer pictures replace older ones that missed
  their display slot.
- **Audio** uses SS4S's SDL module (AAudio or OpenSL ES underneath), with the same clock and pull path as the
  desktop.
- **Co-op** runs its second session in a separate process, as on every platform. The peer is an executable packaged
  as `lib/<abi>/libeclipse_peer.so`, which Android installs executable beside the app's libraries.
- **Files**: settings and pairings live in the app's private storage; translations, the controller database and
  the CA bundle are unpacked from the APK's assets at the first start of each version.
- **Logging**: to logcat (tag `Eclipse`) and, when diagnostic logs are kept, to their files.

## 8. Troubleshooting

| Symptom | Cause and fix |
|---|---|
| "App not installed" when updating | The installed Eclipse is signed with a different key. Uninstall it first (pairings are lost), or install an APK signed with the same key. |
| "There was a problem parsing the package" | The download is incomplete or the device is older than Android 9. Check the SHA-256. |
| Eclipse is not in the apps row | Some launchers list sideloaded apps only under **Settings → Apps**. Eclipse has a TV banner and appears on Android TV launchers. |
| Picture stays black, sound plays | The decoder refused the stream. Try H.264 or a lower resolution in **Settings → Basic**, and attach the log (`adb logcat -s Eclipse`). |
| Refresh rate does not change | Turn on the device's frame-rate matching option ([Display](#display)). |

### Reporting

Use **Settings → About → Keep diagnostic logs**, restart, reproduce, then **Share logs…**, and attach the bundle to
an [Eclipse issue](https://github.com/PreceptorOfMagic/Eclipse/issues/new/choose) with the device model, Android
version and Eclipse version. Results from `tests/android/device_smoke.sh` on a device are welcome too.
