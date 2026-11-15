Eclipse 1.3.7 lets the PC you play on be one of the two co-op PCs, on Windows and on Linux (GNOME and KDE Plasma): your half runs on this PC, and only the other PC is streamed. It also tells you when a new version is out, shows streaming tips, and takes up to 16 controllers per stream. Pair it with **Umbra 0.5.1** on Windows PCs (it installs HidHide, which keeps controllers given to the other PC away from this PC's games) or **Umbra 0.5.3 for Linux**.

The Windows, Linux (Flatpak) and source packages are built from `57d4355c` (tag `eclipse-v1.3.7`). The LG TV and Android packages were built from an earlier candidate of this release, `cfeea95b` (not published): it differs from `57d4355c` only in Windows and desktop Linux code, none of which is compiled into those two packages, so they report `cfeea95b` and behave the same. Every package reports version 1.3.7.

## Downloads

| Device | Asset | Notes |
|---|---|---|
| LG webOS TV | `com.aurora.gamestream_1.3.7_arm.ipk` | Install with webOS Dev Manager (Developer Mode). Same app id, so settings and pairings are kept. |
| Windows x64 (installer) | `eclipse-windows-x64-setup-unsigned.exe` | Per-user install, no administrator rights. Upgrades an existing install in place. |
| Windows x64 (portable) | `eclipse-windows-x64-unsigned.zip` | Extract everything and run `moonlight-tv.exe` (inherited executable name). |
| Linux x86_64 | `Eclipse-1.3.7-linux-x86_64.flatpak` | `flatpak install --user --reinstall Eclipse-1.3.7-linux-x86_64.flatpak`. Settings and pairings are kept. |
| NVIDIA SHIELD TV | `eclipse-android.apk` | Sideload with the Downloader app or `adb install`. Signed with the project's release key, so it updates 1.3.6 in place. |

Each asset has a matching `.sha256` file, and `SHA256SUMS` covers everything. Source archives for the exact build are attached. **The Linux tarball is no longer published**; the Flatpak has been the Linux package since 1.3.6, and the 1.3.6 tarball stays on its release.

## What's in this release

- **Co-op with this PC as one of the two PCs (Windows, and Linux with GNOME or KDE Plasma).** When one of the two co-op PCs is the PC Eclipse runs on, its half is no longer streamed to itself: this PC's games run on a display of their own, shown directly in its half of the Eclipse window, and only the other PC is streamed. That removes the capture, encode and decode of this PC's half, which slowed games on this PC and added delay.
  - The mouse moves between the halves at the divider: in this PC's half it is this PC's own pointer, and across the divider it controls the other PC, while this PC's game keeps its focus and controllers.
  - Controllers you give to the other PC are kept away from this PC's own games and Steam. On Windows this uses HidHide, which Umbra 0.5.1 installs, and Eclipse offers to restart apps that already had them open. On Linux it needs nothing extra: Eclipse takes the controller for itself, which also cuts off programs that already had it open.
  - On Windows, Eclipse asks once per session for administrator rights, and says why. The taskbar icon's right-click menu has Disconnect, Quit stream, Give back the mouse and Resume co-op.
  - On Linux, the desktop asks once per session to allow input capture, and the first time to register Eclipse's two shortcuts. It needs GNOME or KDE Plasma on Wayland. On other desktops and on X11, co-op with this PC works as before: this PC is streamed to itself.
  - Ctrl+Alt+Shift+Q ends the session and Ctrl+Alt+Shift+Z brings the mouse back, from either half.
  - This PC no longer has to be paired for this: Eclipse runs its side itself.
- **Update notice.** Once a day, when the launcher opens, Eclipse checks the Project Hub's list of releases. When a newer version is out, a dot appears on Settings and in Settings > About, with **What's new** and how to update on your device. No pop-ups and nothing during a stream. It can be turned off in Settings > About.
- **Streaming tips.** A tip on the connecting screen, and one in the settings bar with **Next tip** and **All tips**. 82 tips, chosen for each platform, shown in order of usefulness, without repeats.
- **Up to 16 controllers per stream** (was 4).
- **The right stick scrolls** long lists and dialogs in Eclipse's menus.
- **Nintendo controllers:** X and Y follow the Xbox layout, as A and B already did.
- **Steam Controller (2026)** on Windows and Linux.
- **Co-op start:** Eclipse waits for the second PC beside the first one's stream instead of failing after a few seconds. If the second PC takes more than 20 seconds, it asks whether to keep waiting (up to a minute) or end.
- **Co-op never turns a host's monitors off** at the start of a session.
- **Ending a stream** says "Quitting game..." when you quit the game and "Disconnecting..." when you disconnect.
- **Fixes:** the launcher no longer stays half-drawn after the screen mode changes (Windows); a crash when the slow-connection question was shown; a freeze at "Checking..." when starting co-op on Linux; tiles no longer zoom when focused, so the focus outline is drawn cleanly; cover images are attached to their tiles more reliably; for co-op, this PC is found straight away instead of only when network discovery answers in time; on GNOME, the pointer is drawn on this PC's half.

## Tested

- **Windows:** co-op with this PC as one of the two PCs, over many sessions on two PCs. Streaming with five controllers (the host saw all five). Update notice, tips and the launcher fix on the Windows build.
- **Linux (Ubuntu 26.04, GNOME 50 and KDE Plasma 6.6, Flatpak and a native build):** co-op with this PC and a Windows PC: this PC's half, the pointer crossing in both directions, input capture, a controller given to the other PC (this PC's programs received none of its input, the other PC received it), both shortcuts with a game focused on this PC's half (KDE), and a clean end, including after Eclipse was killed (KDE puts the screen layout back at the next start). With virtual controllers and a test animation instead of a real game. On KDE this PC's half is shown at about 48 pictures per second (60 on GNOME); the game itself runs at its own rate. Update notice, tips, and an ordinary two-PC co-op start, including a second PC slowed to 28 seconds (the 20-second question, Wait and End).
- **LG TV:** a beta build of this code: launcher, tips, a 4K HDR stream, two-PC co-op and quitting.
- **Not tested:** the NVIDIA SHIELD build (shared code only; the release was judged low risk without it), the 1.3.7 packages themselves on the TV and the SHIELD, PlayStation and Switch controllers in Linux co-op with this PC (programs reading them directly can still see them; Eclipse names those programs), the Steam Controller on Linux, other Linux distributions and GNOME 51 / Plasma 6.7 for co-op with this PC (the same code paths; Plasma 6.7's output protocol is handled but was not run).

## Known issues

- **Windows packages are unsigned.** Windows SmartScreen may warn. Check the SHA-256 before running.
- **GNOME asks "Capture Input" at the start of each co-op session with this PC.** Choose Share; without it the pointer cannot move over to the other PC's half.
- **GNOME: don't use the orange screen-sharing icon in the top bar during co-op with this PC.** It stops this PC's half; end the session with Ctrl+Alt+Shift+Q instead.
- **GNOME: absolute pointing devices point to the wrong place during co-op with this PC.** A touchscreen, a graphics tablet or a KVM in absolute mouse mode is mapped across this PC's half and the screen together. An ordinary mouse is not affected.
- **Some games pick music or other sound clips at random on each PC.** In co-op you may hear both PCs' clips. Turn the game's music off on one PC if it sounds doubled.
