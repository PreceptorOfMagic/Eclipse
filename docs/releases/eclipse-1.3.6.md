Eclipse 1.3.6 makes the Flatpak the Linux package, fixes 4K streaming and co-op sound on Linux, stops the app menu acting on the wrong app, and brings every platform onto the same code as the NVIDIA SHIELD. Pair it with **Umbra 0.5.0** on Windows PCs or **Umbra 0.5.2 for Linux**.

Every package is built from `9f520ae4` (tag `eclipse-v1.3.6`) and reports version 1.3.6 and that commit. The webOS, Windows, Linux and Android packages now come from one commit again.

## Downloads

| Device | Asset | Notes |
|---|---|---|
| LG webOS TV | `com.aurora.gamestream_1.3.6_arm.ipk` | Install with webOS Dev Manager (Developer Mode). The app id stays `com.aurora.gamestream`, so an earlier install keeps its settings and pairings. |
| Windows x64 (installer) | `eclipse-windows-x64-setup-unsigned.exe` | Per-user install, no administrator rights, no background service. Upgrades an existing install in place. |
| Windows x64 (portable) | `eclipse-windows-x64-unsigned.zip` | Extract everything and run `moonlight-tv.exe` (inherited executable name). |
| Linux x86_64 | `Eclipse-1.3.6-linux-x86_64.flatpak` | **New.** `flatpak install --user Eclipse-1.3.6-linux-x86_64.flatpak`. Any x86_64 distribution with Flatpak; uses the Freedesktop 25.08 runtime from Flathub, as Umbra for Linux does. See **Linux** below. |
| Linux x86_64 (older package) | `eclipse-linux-x86_64.tar.gz` | The Ubuntu-built tarball, kept for anyone who wants it. Extract, then `./install.sh` (per-user) or run `./eclipse` in place. Needs glibc 2.35+ (Ubuntu 22.04 baseline). |
| NVIDIA SHIELD TV | `eclipse-android.apk` | Sideload with the Downloader app or `adb install`. Signed with the project's release key, so it updates 1.3.5 in place. |

Each asset has a matching `.sha256` file, and `SHA256SUMS` covers everything. Source archives are attached for the exact build (see **Source** below).

## What's in this release

- **Eclipse for Linux is a Flatpak.** One bundle for any x86_64 distribution, on Wayland or X11, in place of a tarball built against Ubuntu 22.04. It uses the tarball's settings folder, so a PC that already runs Eclipse keeps its paired hosts and settings: there is nothing to copy and nothing to pair again. The tarball stays on the release.
- **4K streaming on Linux with VA-API (AMD, and Intel, which is untested).** Each decoded picture used to be copied back from the graphics card on the thread that receives the stream. At 4K that thread fell behind and about a quarter of the video packets were lost, so the stream stuttered and reported a poor connection. Pictures now go straight from the decoder to the screen: on an RX Vega at 4K60 and 80 Mbps, packet loss went from 26% to none, on Wayland and on X11.
- **Co-op on Linux stays on the graphics card.** A co-op stream starts with damaged pictures until both PCs have sent a full one. Eclipse counted those as decoder failures and switched to software decoding, which cannot keep up with co-op; the panes then flashed and the stream reported network errors. Damaged pictures from a decoder that works now have an allowance of about 5 seconds of their own; real decoder errors still switch after three.
- **No more crackle in co-op on Linux.** Eclipse now plays through PulseAudio (or PipeWire's PulseAudio service) when one answers, and falls back to ALSA. Through ALSA a late block of sound became a short gap, about five a second in co-op.
- **Scaled Linux desktops stream at the screen's real size.** On a 4K TV with the desktop scaled to 200%, the stream defaulted to 1080p. It now defaults to the panel's own size.
- **The app menu acts on the app it shows.** With an app's menu open, the app list refreshes every few seconds; afterwards **Start streaming** could start a different app, and **Info** described it. The menu now keeps the app itself. The highlighted tile also no longer flicks to another tile and back on each refresh.
- **Shared fixes from the SHIELD, now on every platform:**
  - When the second co-op PC refuses to start, Eclipse shows the reason it gives instead of "did not begin streaming in time".
  - If Eclipse is closed while it makes its identity key on first start (this can take a few seconds on a TV), the next start makes it again instead of failing.
  - A fatal error is also written to the log, so it reaches a support report.
  - The packages' `ACKNOWLEDGEMENTS.md` now credits [Moonlight V+](https://github.com/qiin2333/moonlight-vplus) for the hold-Start shortcut wheel.
- **NVIDIA SHIELD TV:** no changes since the 1.3.5 APK apart from the version number.

## Linux

**Install:**
1. Install Flatpak and add Flathub if you have not already (Ubuntu does not include Flatpak by default): `sudo apt install flatpak`, then `flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo`.
2. Download `Eclipse-1.3.6-linux-x86_64.flatpak` and its `.sha256` into one folder: `sha256sum -c Eclipse-1.3.6-linux-x86_64.flatpak.sha256`.
3. `flatpak install --user Eclipse-1.3.6-linux-x86_64.flatpak`. Flatpak fetches the runtime and, on NVIDIA, the driver extension matching your driver.
4. Start **Eclipse** from the applications menu, or with `flatpak run io.github.PreceptorOfMagic.Eclipse`.

To update later, install the newer bundle with `--reinstall` added. To remove it: `flatpak uninstall --user io.github.PreceptorOfMagic.Eclipse`; settings and pairings stay in `~/.local/share/com.limelight/moonlight-tv`. If you no longer want the tarball install, remove it with `~/.local/share/eclipse/app/uninstall.sh --keep-settings`, which keeps the shared settings.

**How likely it is to work on your PC:**
- **Low risk:** Ubuntu, Fedora, Arch, openSUSE, Debian and Linux Mint with GNOME, KDE Plasma, Cinnamon or Xfce, on AMD or NVIDIA graphics.
- **Untested, probably fine:** Intel graphics (Flatpak fetches its VA-API driver automatically); NVIDIA on an X11 desktop; distributions whose firewall (firewalld) hides PCs from discovery (add the PC by its address); controller gyro and rumble where the distribution has no udev rules for controllers; a brand-new NVIDIA driver before Flathub publishes its matching extension.
- **More likely to have problems:** wlroots desktops such as Hyprland and Sway, and COSMIC (fullscreen and mouse capture); the older Flatpak 1.12 to 1.14 of Ubuntu 22.04, Linux Mint 21 and Debian 12; Steam Deck Game Mode.
- **Slow:** the open-source nouveau driver on older NVIDIA cards decodes in software.
- **Not supported:** ARM computers.

## Tested

- **Linux, on hardware:** development builds with the same Linux changes, on one PC at the TV running Ubuntu 26.04. With an AMD Radeon RX Vega: ordinary 4K60 streams on GNOME (Wayland) and KDE Plasma (Wayland and X11) with no packet loss, and two-PC co-op, accepted by the maintainer. With an NVIDIA RTX 2060 SUPER on GNOME: a 4K60 stream with no dropped frames, and two-PC co-op, which the maintainer called flawless. Scaled desktops at 150% and 200% defaulted to 4K.
- **Linux, Flatpak install:** the 1.3.5 Flatpak installed and streamed on fresh Ubuntu 26.04, Fedora 44, openSUSE Tumbleweed, Arch Linux and Debian 13 under WSL, and kept the tarball's settings and pairings.
- **App menu:** the code builds for desktop Linux, and a test of the app grid showed every tile keeping its place when an unchanged list is applied again. Both menu fixes have been in the SHIELD APK since 1.3.5.
- **Not tested on hardware:** the 1.3.6 packages themselves, the app-menu fix on webOS, Windows and Linux, the shared SHIELD fixes outside the SHIELD, and the Windows installer over 1.3.5. The changes outside Linux are small, and this release was judged low risk without those tests.

## Known issues

- **Windows packages are unsigned.** Windows SmartScreen may warn. Check the SHA-256 before running. Code signing through SignPath Foundation is being set up.
- **Some games pick music or other sound clips at random on each PC.** The two PCs then play different clips at the same moment, which the filter cannot merge, so you hear both. Halo: Combat Evolved does this with its music. Turn the game's music off on one PC if it sounds doubled.
- **If the second co-op PC takes longer than about 9 seconds to start,** the first PC's stream can drop. Start co-op again. Seen once.
- **Umbra 0.5.0 does not report its graphics card,** so Eclipse asks once for each PC after it is paired.
- **After a TV restart, until Eclipse next starts,** other Developer Mode apps could read or change its files. Eclipse locks them again when it starts.
- **NVIDIA on Linux** still copies each picture once on its way to the screen. It kept up at 4K60 on an RTX 2060 SUPER.
- Only the LG G5 on webOS 26 has been physically tested. Other LG models and firmware versions need reports.

## Source

- `eclipse-project-source-9f520ae4.tar.gz`: this repository at `9f520ae4`, with its pinned dependency modules and their repository-owned patches already applied. It is the source of every package in this release, the APK's build scripts included.
- `Eclipse-1.3.6-linux-flatpak-sources.tar.gz`: every third-party source the Flatpak build downloaded, as flatpak-builder fetched them. The Flatpak manifest is `packaging/linux/flatpak/io.github.PreceptorOfMagic.Eclipse.yml` on `main`; it pins `9f520ae4`.
- `eclipse-assets-source.tar.gz`, `eclipse-android-assets-source.tar.gz`: the original asset sources, unchanged from 1.3.5 because no asset changed.
- `eclipse-linux-webos-external-source.tar.gz`, `eclipse-windows-external-source.tar.gz`, `eclipse-android-external-source.tar.gz`: third-party library sources, unchanged from 1.3.5 because no third-party source input changed. Read each archive's manifest for build recipes and provenance limits.

Eclipse is a fork of moonlight-tv (GPL-3.0). Upstream names that remain in identifiers and file names (Moonlight, `moonlight-tv.exe`, `com.aurora.gamestream`) are kept for compatibility. This project is developed with AI assistance: Claude and Codex write and debug code under the maintainer's direction and hands-on testing. See the Project Hub for details.
