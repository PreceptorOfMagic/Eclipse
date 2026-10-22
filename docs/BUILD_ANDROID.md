# Eclipse for Android TV and NVIDIA SHIELD

Eclipse for Android TV is the same program as on webOS, Windows and Linux: the C client, its LVGL interface,
co-op and every setting are built from this repository with the Android NDK. The only Android-specific parts are
platform glue (`src/app/platform/android`), a small activity (`deploy/android`) and a video module that drives
Android's MediaCodec (`third_party/ss4s/modules/android`, carried in `patches/client-deps/ss4s.patch`).

It is distributed by **sideloading** only, like the rest of the project: there is no Play Store listing, no
developer account and no fee. Google's developer-verification requirement for sideloaded apps does not apply to
Android TV devices, and installs over ADB are exempt everywhere.

> **Status.** Tested on a 2019 SHIELD TV (SHIELD Experience 9, Android 11): streaming, two-PC co-op with its audio
> filter, the SHIELD remote, and Xbox and Nintendo Switch Pro controllers. Its host tests pass, the APK builds for
> all three ABIs, and the emulator smoke tests pass on Android 11 (API 30, as CI runs them:
> `.github/workflows/build-android.yml`) and on the Android TV 16 (API 36) image. The other SHIELD models and other
> Android TV devices run the same software but have not been tested: please report results
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
- **NVIDIA SHIELD TV** (2015, 2017, 2019 and Pro) on SHIELD Experience 9 (Android 11) is the primary target;
  SHIELD Experience 8 (Android 9) is the oldest it installs on.
- One APK holds `arm64-v8a`, `armeabi-v7a` (many TV devices run 32-bit Android on a 64-bit CPU: the 2019 SHIELD TV
  does) and `x86_64`.
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
| D-pad, OK | Navigate, select | Arrow keys, Enter. Holding OK is a right click. With the on-screen keyboard open, OK presses the selected key. |
| Back | Back | Tap: Escape on the host. Hold: open the stream menu. Closes the on-screen keyboard first. |
| Red, Yellow, Blue | As on the webOS remote | Red: open the stream menu. Blue: open the on-screen keyboard. Yellow: nothing. |
| Menu (SHIELD ≡) | As the webOS Yellow key | Open the stream menu |
| Info, Guide | | Open the stream menu |
| Channel + / − | | Page Up / Page Down |
| Green | | Nothing (as on webOS) |

A full keyboard's Menu key is the host's Applications key, as on a PC.

### Controllers

Controllers work as on the other platforms, with the same combinations (Start + View + LB + RB for the stream menu,
LB + right stick click for the virtual mouse, RB + right stick click for the on-screen keyboard), plus one for
Android: **View + Start shows the TV's home screen**, acted on when both are let go; the PC sees neither button.

**Hold Start for the shortcut wheel**, on every platform and any controller (the combinations above need buttons
some controllers lack). After 0.75 s a wheel shows; point either stick or the d-pad at **Menu** (up), **Mouse** (right),
**Keyboard** (down) or **Performance** (left), and let go of Start to choose what is highlighted. A stick can go back to
the middle and the highlight stays. Each d-pad press, and each new direction a stick points, moves the highlight one
step: from the middle to that option, and from an option back to the middle (**Continue**, which does nothing) when
pointed the opposite way; **B** goes back to the middle too. The choice happens once the controller is let go. A short press of Start is Start, and the controller keeps
playing while the wheel only shows: the PC stops seeing it from the first push towards a choice until it is let go.
Letting go of Start without pushing anything closes the wheel. This is the Start gesture of Moonlight V+.
**Settings > Input > Controller shortcuts** turns the combinations, the wheel, or both off.

A controller's Guide (Xbox) button goes to the PC, as on a PC, wherever Android delivers it. The SHIELD TV does
not: its system package `com.nvidia.shieldtech.hooks` turns the Guide button into Home before any app sees it, and
lets no app opt out (Steam Link and Moonlight live with the same). So on a SHIELD, as Sunshine's
`back_button_timeout` setting does on a host, **holding View for 2 seconds is the Guide button**: the PC sees View
let go, then Guide pressed and let go. The stream menu also has a **Guide button** that presses the first
controller's Guide. The Xbox button itself stays the SHIELD's Home.

**Which button is which** is decided for each controller when it connects, from the buttons Android says that
controller has on the Android version the device runs. SDL's own mapping, the controller database's lines for it and
a mapping read straight off Android's key codes are each checked against those buttons, and the best fit wins: a
mapping written for another Android version binds buttons the controller does not have there, so it loses. (The
database's Nintendo Switch Pro Controller line is Android 9's; on a SHIELD TV running Android 11 it made ZR Start and
+ the right stick.) The log names each choice: `Controller <name> (<GUID>): mapping from ...`.

**A Nintendo Switch Pro Controller follows its letters in Eclipse and its positions everywhere else on the SHIELD.**
Eclipse sends the button labelled A as A (**Settings > Input > Nintendo controllers**, "Buttons as printed", the
default); the SHIELD's own menus and other apps treat the controller like an Xbox controller, by position, so there
the bottom button (labelled B) confirms. Choosing "Xbox positions" makes Eclipse go by position too.

In co-op, each controller goes to the PC chosen for it on the co-op screen. The virtual mouse points like the LG
Magic Remote: it starts in the middle of the PC it is over, and pushed past the line between the two pictures it
becomes the other PC's pointer, arriving at the matching edge. The triggers click, and the wheel and the on-screen
keyboard type, on whichever PC it is over; while a trigger is held it stays on that PC. Any controller's right stick
moves it. (The other platforms do the same since 1.3.5, except when the Windows PC running Eclipse is itself one of
the two co-op PCs: there the virtual mouse keeps its earlier behaviour.)

A TV's own remote reaches a box plugged into the TV only as buttons, over HDMI-CEC: an LG Magic Remote's pointer
and wheel stay with the TV (measured with the SHIELD TV on an LG G5: no input event reached the SHIELD while the
pointer was moved, and Home was handled by the TV). For a pointer in a stream, pair a pointer remote or a mouse with
the Android device itself: Android reports it as a mouse, and Eclipse sends it to the PC as one, through the same
path as the desktop builds' mouse.

### Display

Before a stream starts, Eclipse switches the TV to the mode that shows it best at the TV's current resolution:
the stream's rate (or the rate chosen in **Settings → Basic**), else the smallest whole multiple of it (a 30 FPS
stream on 60 Hz), else the TV's highest rate. It returns the TV to the system's choice when the stream ends: the
webOS panel does the same by itself. If the TV has no mode for the frame rate but has its NTSC equivalent
(59.94 Hz for 60 FPS), Eclipse asks the PC for that rate, as the webOS build asks for its panel's, so the game is
paced to the screen. On the 2019 SHIELD TV (Android 11) this needs no setting: its Advanced display settings have
no frame-rate matching switch, and the self-test measured the requested mode taking effect (59.94 Hz to 60 Hz).
Some other devices only switch when their own frame-rate matching option is on.

The resolution and frame rate lists are limited to what the TV's current mode and the decoder support, as on
webOS.

### Audio output

**Settings → Audio → Audio output device** lists the TV's outputs by kind and product, for example
**HDMI: LG TV**, **HDMI ARC: Sonos Arc** or **Bluetooth: WH-1000XM5**, as the desktop builds list the PC's. As on
the desktop, a choice applies from the next start of Eclipse; **System default** follows Android's own routing.
Android numbers its outputs differently after every restart or reconnection, so Eclipse stores the name and looks
the output up when a stream starts. If the chosen output is not connected then, Eclipse waits up to five seconds for
it (Bluetooth reconnecting) and otherwise streams the picture without sound rather than sound somewhere you did
not choose, as on the desktop. A chosen output plays through Android's AAudio; **System default** uses OpenSL ES.

### Settings that differ by platform

- **Use the wired network (USB Ethernet adapter)** is a webOS setting; the Windows and Linux builds do not have it
  either. It works around LG TVs that, with a USB Ethernet adapter and Wi-Fi on the same network, answer for their
  wired address over Wi-Fi: Umbra then pins the TV's addresses to the adapter's hardware (MAC) address. Android
  normally keeps one connection, Ethernet when it is plugged in, so the case does not arise; and Android 11 and
  later do not tell apps a network card's hardware address, which the pin needs.
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
./scripts/android/build_apk.sh --abi armeabi-v7a  # one ABI, faster: the device's first ABI
                                                 # (adb shell getprop ro.product.cpu.abilist)
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
library, every file in `lib/` is installable, and the co-op peer finds its libraries beside it: every packaged library
that needs another packaged one carries `RUNPATH $ORIGIN` ([section 7](#7-how-it-works)).

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

**Host tests** run with the desktop build (`ctest -R test_android`, also in the desktop CI). One compiles every C
source of the Android app as the Android build configures it (`test_android_typecheck`: NDK headers stubbed, the
host's C library), which catches Android-only compile errors without an NDK; the others check the translation
catalogue reader against gettext, the UUID generator, the logcat backend, the audio output names and how a stream
opens a chosen output (the SS4S SDL module with the activity faked), the display mode and refresh rate a stream asks
for, and the MediaCodec module's feeding against real H.264 and HEVC streams, including a co-op composite made by
the shipping splice code, decoded by FFmpeg in strict mode, and co-op composites with padding before their second
slice fed without it (see [How it works](#7-how-it-works)), decoding to exactly the same pictures. The type-check compiles with the NDK's own
`-Werror=format-security`. Configure the desktop build with a JDK on `PATH` (JDK 17, as for the APK): the
type-check, the MediaCodec and SDL-audio tests and the Java tests need JNI or Java, and CMake leaves them out
without one (`ctest -N | grep android` lists 17 when they are all there).

**Device smoke tests** run the real APK on an emulator (CI) or a device. They use a self-test build, which installs
**beside** a real Eclipse (its own application ID, `io.github.preceptorofmagic.eclipse.selftest`) without touching
its hosts or settings:

```sh
./scripts/android/build_apk.sh --selftest --abi armeabi-v7a     # dist/eclipse-android-selftest.apk (the device's ABI)
cc -O2 -o hevc_tilecols tests/coop/hevc_tilecols_cli.c src/app/stream/video/coop_splice.c \
   src/app/stream/video/coop_poc.c -Isrc/app/stream/video
adb connect <SHIELD IP>:5555
tests/android/device_smoke.sh dist/eclipse-android-selftest.apk ./hevc_tilecols smoke --uhd
adb uninstall io.github.preceptorofmagic.eclipse.selftest
```

They check that the interface starts and survives d-pad input; that no remote-like device is a joystick to SDL
and, on an emulator, that Up and Back from a d-pad device arrive as the keys the remote's Back (tap/hold) and OK
(hold) depend on; that the TV's refresh rates are read and the mode
for a 60 FPS stream takes effect; that the audio outputs are listed by name and one opens by its current number on
AAudio; that an output closes at once while the app is in the background (the script sends it Home), on SDL's own
driver and on AAudio; that the co-op peer executable runs from the
installed package (it must exit saying it cannot reach the coordinator: the dynamic linker's failure exits 1 too)
and, spawned exactly as a co-op stream spawns it, starts up without a Java VM and answers the
coordinator over the co-op control channel; and that a co-op composite, H.264, a narrow letterboxed HEVC picture and (with `--uhd`, for real
hardware) 4K HEVC Main 10 play through the MediaCodec module onto the video surface with no codec errors. Logs and
screenshots are left in `smoke/`. FFmpeg with libx264 and libx265 is needed to make the test streams, and `python3`
to unpack the test remote.

- **The test remote.** On an emulator the keys case plugs in a TV remote of its own: `libeclipse_test_remote.so`
  (`tests/android/test_remote_uinput.c`, in self-test APKs only), a uinput device with a remote's keys, which Android
  classifies as a keyboard with a d-pad (`SOURCE_KEYBOARD | SOURCE_DPAD`) as it does a SHIELD remote. It runs as
  adb's shell user; the emulator's own key injection (`adb emu event send`) reaches no input device on current
  emulators. On a device, press Back and OK during a stream instead.
- **Emulator images.** CI's is `system-images;android-30;google_apis;x86_64` with the emulator runner's default
  device (a 320x640 screen); an AVD made with `avdmanager create avd` and no `-d` matches it. The app asks for
  landscape, so on that portrait screen the first seconds of a case can be a rotation. Android TV images are 32-bit
  `x86` up to API 34, which the APK does not build; the TV image to use is
  `system-images;android-36;android-tv;x86_64`.
- **Screenshots.** The video cases play under the launcher, whose background is opaque, so their screenshots show
  the launcher, not the picture: these cases check the decoder's counts. The picture itself is checked in a stream.
- Each case reads only the log written since it started (`logcat -T`): `logcat -c` alone can leave the previous
  case's lines, and its "SELFTEST done".

Stream tests against an Umbra host (picture, sound, input, latency, co-op with two hosts) need a person at the TV;
follow the same checklist as for a webOS release.

## 7. How it works

- **Process and UI.** SDL2's `SDLActivity` (subclassed by `EclipseActivity`) starts the client's normal `main`
  from `libmain.so`. LVGL draws with SDL's renderer, exactly as on the desktop.
- **Video** is decoded by MediaCodec straight onto a `SurfaceView` **under** the interface, which is drawn with an
  alpha channel: the same plane model as webOS, so the interface composites over the picture without copies. The
  decoder is put in low-latency mode where the device has one, and newer pictures replace older ones that missed
  their display slot.
- **Audio** uses SS4S's SDL module, with the same clock and pull path as the desktop: OpenSL ES for the system
  default output, AAudio for a chosen one (SDL carries `scripts/android/patches/sdl2-2.32.10-android-audio-hotplug.patch`
  so outputs connected after start-up are offered too, and `sdl2-2.32.10-android-aaudio-close-race.patch`: unpatched,
  closing an AAudio output could crash the event thread, which checks the stream on every event pump). Switching the
  app away ends a stream, which closes its audio while Android has the app paused; unpatched SDL 2 then waited for
  the app to come back -- the app froze and the PC went on streaming to it for minutes -- on both drivers:
  `sdl2-2.32.10-android-opensles-close-while-paused.patch` lets OpenSL ES's audio thread notice the close, and
  `sdl2-2.32.10-android-aaudio-pause-without-lock.patch` pauses AAudio without holding the lock a close needs.
- **Remote and controllers**: a TV remote reaches Eclipse as keys (SDL carries
  `scripts/android/patches/sdl2-2.32.10-android-tv-remote.patch`; unpatched SDL 2 takes any device with a d-pad
  for a joystick, which turned Back into Escape and OK into Enter), and, as on webOS, nothing remote-like is ever
  offered to a game as a player's controller.
- **Controller mappings** (`src/app/platform/android/controller_mapping_android.h`): SDL numbers an Android
  controller's buttons by the key code Android delivers, from a key layout that changes between Android versions,
  and a mapping's GUID records too little to say which layout it was written for (SDL even matches Android mappings
  with the button mask ignored). So the database is not loaded into SDL. Each controller's mapping is chosen as it
  connects, by how each candidate fits the keys `InputDevice.hasKeys` reports (`EclipseActivity.controllerKeys`):
  fewest bindings to keys it cannot produce, then fewest of its keys left unbound, then SDL's line for this Android
  version, a database line, SDL's other lines, the mapping SDL builds from key codes.
- **Co-op** runs its second session in a separate process, as on every platform. The peer is an executable packaged
  as `lib/<abi>/libeclipse_peer.so`, which Android installs executable beside the app's libraries. The app loads its
  libraries in its own linker namespace, which searches that directory; the peer runs in the default one, which
  searches only the system and each library's own `RUNPATH`, so the peer and every packaged library that needs
  another (curl, mbedTLS, `libmain.so`, the SS4S modules) carry `RUNPATH $ORIGIN`. CMake's Android platform ignores
  `BUILD_RPATH`, so it is passed to the linker directly (`CMakeLists.txt`, `scripts/android/build_deps.sh`).
- **Co-op pictures** are fed to the decoder without the padding that comes before their second slice. Hosts may end
  each picture with padding (`cabac_zero_words`), and a co-op picture puts one PC's last slice first, so that padding
  then comes before the other PC's slice. The SHIELD TV's decoder does not decode a slice after such padding (the
  second PC's pane is flat green, with no error), while other decoders skip it. The padding carries no picture data,
  so on Android it is removed (`coop_au_strip_zero_words`, `src/app/stream/video/coop_splice.c`) and the picture is
  the same (`test_android_hevc_zerowords`). Left/right co-op also opens the decoder at the composite's own size,
  twice as wide as one PC's pane: MediaCodec sizes its buffers from it, where other platforms' decoders follow the
  stream.
- **Entropy**: mbedTLS reads `/dev/urandom` (`MBEDTLS_PLATFORM_DEV_RANDOM`, set by `build_deps.sh`). It uses
  `getrandom()` only with glibc, and its own default, `/dev/random`, blocks on kernels before 5.6: the SHIELD TV
  (Linux 4.9) hung at start-up in curl's first TLS set-up. `build_apk.sh` checks the packaged library.
- **Environment**: `libmain.so`, the peer and the MediaCodec module link with `--wrap=setenv,--wrap=unsetenv`
  (`src/app/platform/android/env_android.c`): Bionic's `setenv` frees the old `environ` array for every new name
  while Android's RenderThread may be reading it in `getenv` (a crash at start-up on the SHIELD TV).
- **Quitting** ends the process when the program ends, so a relaunch never sees stale state: with `_exit`, as
  `exit()` would also run Android's own libraries' destructors under their running render threads (Android 16
  aborts on that).
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
| Co-op ends with "the second co-op PC did not begin streaming in time" on Android 12 or newer | Android 12+ limits how many helper processes all apps together may run, and may stop Eclipse's co-op helper when many others are running. Restart the device, or turn on **Developer options → Disable child process restrictions** where offered. The SHIELD (Android 11) has no such limit. |

### Reporting

Use **Settings → About → Keep diagnostic logs**, restart, reproduce, then **Share logs…**, and attach the bundle to
an [Eclipse issue](https://github.com/PreceptorOfMagic/Eclipse/issues/new/choose) with the device model, Android
version and Eclipse version. Results from `tests/android/device_smoke.sh` on a device are welcome too.
