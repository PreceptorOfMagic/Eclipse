Eclipse 1.3.4 is the first public release of Eclipse, the screen-side app of the Eclipse + Umbra project: everyday game streaming plus two-PC couch co-op on LG webOS, Windows and Linux. Pair it with **Umbra 0.5.0** on the host PCs.

Built from the commit tagged `eclipse-v1.3.4`. Every package reports version 1.3.4 and that commit.

## Downloads

| Device | Asset | Notes |
|---|---|---|
| LG webOS TV | `com.aurora.gamestream_1.3.4_arm.ipk` | Install with webOS Dev Manager (Developer Mode). The app id stays `com.aurora.gamestream`, so an earlier install keeps its settings and pairings. |
| Windows x64 (installer) | `eclipse-windows-x64-setup-unsigned.exe` | Per-user install, no administrator rights, no background service. Adds Start menu entries and an Apps & features uninstaller. |
| Windows x64 (portable) | `eclipse-windows-x64-unsigned.zip` | Extract everything and run `moonlight-tv.exe` (inherited executable name). |
| Linux x86_64 | `eclipse-linux-x86_64.tar.gz` | Extract, then `./install.sh` (per-user) or run `./eclipse` in place. Needs glibc 2.35+ (Ubuntu 22.04 baseline). |

Each asset has a matching `.sha256` file, and `SHA256SUMS` covers everything. Source archives are attached for the exact build (see **Source** below).

## What's in this release

- **Two-PC co-op on one screen:** two PCs running Umbra each stream half of the picture, joined into one image. Horizontal and vertical splits, with controllers assigned per PC. The co-op pane is built into Umbra 0.5.0 and set up automatically. Mixed NVIDIA + AMD pairs use Umbra's GPU HEVC encoder.
- **Co-op sound:** both PCs' sound is mixed into one output. **Settings → Co-op → "Play shared sounds once"** removes the second copy of a sound both PCs play (narration, cutscenes, music), with **Accurate** (default, look-ahead slider 0–100 ms, default 40), **Fast** and **Off**. In this release the filter no longer removes a sound only one PC played: it keeps gunfire and reload sounds it used to cut short (see **Tested**).
- **Disconnect pauses, Quit ends:** in co-op, Disconnect pauses both PCs so you can resume, and Quit ends both.
- **LG TVs on webOS 26:** settings, pairings and co-op keep working after the TV restarts. A restart opens up the Developer Mode app folders; Eclipse locks its own folders again when it starts.
- **Installers and clean removal:** a Windows per-user installer with a keep-settings option, a Linux per-user installer and uninstaller, and webOS settings in an app-owned, protected folder. Uninstalling removes only what Eclipse owns.
- **Support logs:** Settings → About → *Keep diagnostic logs*, then *Share logs* (on the TV it serves a download page on your network; PINs and passwords are left out).
- **No network queries for development hardware:** release builds send nothing for the project's own Raspberry Pi controller hub.

## Tested

TESTED-PLACEHOLDER

## Known issues

- **Windows packages are unsigned.** Windows SmartScreen may warn. Check the SHA-256 before running. Code signing through SignPath Foundation is being set up.
- **Some games pick music or other sound clips at random on each PC.** The two PCs then play different clips at the same moment, which the filter cannot merge, so you hear both. Halo: Combat Evolved does this with its music. Turn the game's music off on one PC if it sounds doubled.
- **Umbra 0.5.0 does not report its graphics card,** so Eclipse asks once for each PC after it is paired, although **Settings → Co-op → Graphics cards** says Umbra reports it.
- **After a TV restart, until Eclipse next starts,** other Developer Mode apps could read or change its files. Eclipse locks them again when it starts.
- **If the TV restarts before Eclipse is first opened,** box art is stored in a temporary folder and downloaded again after each restart.
- The extra audio delay that **Accurate** adds has not been measured on the TV. Choose **Fast** or lower the look-ahead if you notice lag.
- Only the LG G5 on webOS 26 has been physically tested. Other LG models and firmware versions need reports. Linux is tested under WSL; native Linux with AMD/Intel VA-API is not yet validated.

## Source

- `eclipse-project-source-<commit>.tar.gz`: this repository at the tagged commit, with its pinned dependency modules and their repository-owned patches already applied.
- `eclipse-linux-webos-external-source.tar.gz`, `eclipse-windows-external-source.tar.gz`, `eclipse-assets-source.tar.gz`: third-party library and original asset sources for these builds. Read each archive's manifest for build recipes and provenance limits.

Eclipse is a fork of moonlight-tv (GPL-3.0). Upstream names that remain in identifiers and file names (Moonlight, `moonlight-tv.exe`, `com.aurora.gamestream`) are kept for compatibility. This project is developed with AI assistance: Claude and Codex write and debug code under the maintainer's direction and hands-on testing. See the Project Hub for details.
