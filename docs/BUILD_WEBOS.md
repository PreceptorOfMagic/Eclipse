# Client build and installation for LG webOS

This guide explains how to enable developer mode, build the client, and install it manually on an LG webOS TV. The current project rig is an LG G5 running webOS 26; this is not a claim of broad model or firmware compatibility.

---

## Table of contents

1. [Developer mode on the TV](#1-developer-mode-on-the-tv)
2. [Manual installation](#2-manual-installation)
3. [Build](#3-build)
4. [Troubleshooting](#4-troubleshooting)
5. [webOS Homebrew (catalog)](#5-webos-homebrew-publication)

---

## 1. Developer mode on the TV

To install apps manually (.ipk), the TV must be in developer mode.

### Step-by-step (LG official)

1. **LG developer account**
   - Go to [developer.lge.com](https://developer.lge.com) and create an account

2. **Developer Mode app on the TV**
   - Press **Home** on the remote
   - Open **LG Content Store**
   - Search for **"Developer Mode"**
   - Install the app

3. **Enable developer mode**
   - Open the **Developer Mode** app
   - Sign in with your LG account
   - Turn on **"Dev Mode Status"** (the TV may restart)
   - Turn on **Key Server**

4. **Connection passphrase**
   - In the Developer Mode app, note the **passphrase** shown
   - You will use it to connect the TV to your PC

> Developer Mode is time-limited. Extend it while the TV is online before the timer expires; it also disables after ten offline reboots. When Developer Mode is disabled, apps installed through it are uninstalled. See LG's [Developer Mode app guide](https://webostv.developer.lge.com/develop/getting-started/developer-mode-app).

### GUI alternative: webOS Dev Manager

If you prefer a GUI, [webOS Dev Manager](https://github.com/webosbrew/dev-manager-desktop) supports TVs in Developer Mode as well as rooted TVs:

1. Install webOS Dev Manager on the computer.
2. Add the TV using the connection details from the Developer Mode app.
3. Use its application installer for the locally built `.ipk`.

---

## 2. Manual installation

### Prerequisites

- TV in developer mode (see section 1)
- TV and PC on the **same network**
- A locally built `.ipk` package from section 3; no public package is available yet

### Method A: ares-cli (command line)

1. **Install webOS CLI**
   - Follow [LG's webOS CLI installation guide](https://webostv.developer.lge.com/develop/tools/cli-installation)
   - Windows: `npm install -g @webos-tools/cli`
   - Linux: `sudo npm install -g @webos-tools/cli`

2. **Configure the device**
   ```bash
   ares-setup-device
   ```
   - Add the TV with port `9922` and user `prisoner`.
   - Retrieve its key, replacing the quoted example with the device name you configured:
     ```bash
     ares-novacom --device "TV_DEVICE_NAME" --getkey
     ```
   - `ares-novacom` prompts for the **passphrase** from the Developer Mode app.

3. **Install the exact `.ipk` produced by the build**
   ```bash
   find dist -maxdepth 1 -type f -name '*.ipk' -print
   ares-install "dist/ACTUAL_GENERATED_FILENAME.ipk" -d "TV_DEVICE_NAME"
   ```
   Do not copy a versioned filename from older documentation; use the file the build actually produced. The public application name is **Eclipse**. The installed app ID remains `com.aurora.gamestream` as a compatibility identifier so existing settings and pairings keep their namespace.

4. **Close any running copy, then launch the installed build**
   ```bash
   ares-launch --close com.aurora.gamestream -d "TV_DEVICE_NAME"
   ares-launch com.aurora.gamestream -d "TV_DEVICE_NAME"
   ```

### Method B: webOS Dev Manager (GUI)

1. Install [webOS Dev Manager](https://github.com/webosbrew/dev-manager-desktop) on the computer.
2. Connect it to a Developer Mode or rooted TV on the same network.
3. Select the exact `.ipk` created by the current build and install it.
4. Close and relaunch the app after an update; installation alone does not reload a running process.

---

## 3. Build

The webOS client is built via **cross-compilation** on **Linux** (or WSL2 on Windows).

### Tested target

- **Project rig:** LG G5 running webOS 26; broader model support is unverified
- **Build target:** ARM with the `arm-webos-linux-gnueabi` toolchain

### Prerequisites

**Linux (Ubuntu/Debian):**
```bash
sudo apt-get update
sudo apt-get install -y cmake gawk curl git build-essential
```

**Windows:** Use WSL2 with Ubuntu (see section 3.3).

### 3.1. Quick build (automated script)

> [!WARNING]
> If the SDK is absent, this convenience wrapper downloads a third-party toolchain archive without verifying a pinned digest. That path is suitable only for the controlled development setup while the provenance check is open; it is not an approved public release build. A release builder must provide a previously verified SDK through `WEBOS_SDK_DIR` until the wrapper enforces the digest itself.

```bash
cd /path/to/client-repository
chmod +x scripts/webos/build_for_lg.sh
./scripts/webos/build_for_lg.sh
```

The `.ipk` will be created in `dist/`.

The wrapper owns dependency preparation, cross-compilation and packaging. A low-level SDK-download recipe is intentionally omitted until the third-party archive and its digest are recorded together; do not replace that check with an unverified download pasted from old documentation.

### 3.2. Output

The resulting package is written under `dist/`. Read the actual generated filename;
the version embedded in the name is not bumped for every local build.

### 3.3. Windows through WSL2

The existing Docker helper downloads an unverified third-party packaging binary, so it is not a supported release path. Use WSL2 until that helper either builds the tool from audited source or verifies an approved artifact.

**WSL2 (Ubuntu):**
```bash
wsl --install -d Ubuntu
# In Ubuntu:
sudo apt-get install cmake gawk curl git build-essential
./scripts/webos/build_for_lg.sh
```

---

## 4. Troubleshooting

### "TOOLCHAIN_FILE not found"

Set the SDK path:
```bash
export WEBOS_SDK_DIR=/path/to/arm-webos-linux-gnueabi_sdk-buildroot
./scripts/webos/build_for_lg.sh
```

### Custom SDK location

```bash
sudo mv /tmp/arm-webos-linux-gnueabi_sdk-buildroot /opt/
export TOOLCHAIN_FILE=/opt/arm-webos-linux-gnueabi_sdk-buildroot/share/buildroot/toolchainfile.cmake
./scripts/webos/easy_build.sh -DCMAKE_BUILD_TYPE=Release
```

### CMake dependency errors

The buildroot-nc4 SDK includes pbnjson_c, PmLogLib, webosi18n, etc. If something is missing, verify your SDK installation.

### TV does not appear in ares-setup-device

- TV and PC on the same network
- Developer mode enabled on the TV
- Firewall not blocking the ares-cli port

### HEVC visual degradation

Do not treat periodic decoder refresh as a documented cure for blockiness or colour smearing; the project has not measured that outcome, and refresh requests can have their own visible or bitrate effects. Reproduce the issue with a live host encode, capture a redacted photograph or screenshot of the actual display, and attach a narrow log slice and exact host/client build information to the structured bug report described in [CONTRIBUTING.md](../CONTRIBUTING.md#evidence-expected).

---

## 5. webOS Homebrew publication

> [!WARNING]
> **Do not submit the inherited catalog metadata yet.** Its name, repository/release links, artwork and package notices are not the final public set. Publishing is gated until the identity, source, artifact-compliance and fresh-install checks are complete.

See **[WEBOS_HOMEBREW.md](WEBOS_HOMEBREW.md)** for the current blockers and the submission procedure to follow after they clear. The existing file under `deploy/webosbrew/` is a historical regeneration input, not a ready-to-submit catalog entry.
