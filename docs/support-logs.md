# Support logs — getting them from a user, and reading them

Written 2026-09-30 for the goal "everything you could possibly need from a random user is logged … and the recovery
is working and is tested across all versions". Assume ONE file is all you will ever get: it is built to be enough.

## What to ask the user for

| Platform | Normal case | The app will not open (crashes at start) |
|---|---|---|
| Windows | Settings → About → tick **Keep diagnostic logs**, restart, reproduce, then **Share logs…**: send the one file it names (`…\logs\eclipse-logs.txt`; **Open folder** shows it). | Start it twice more, then send `%APPDATA%\com.limelight\moonlight-tv\logs\eclipse-logs.txt`. |
| Linux | Same; **Share logs…** writes the same file and also serves the network page. | Same file: `~/.local/share/com.limelight/moonlight-tv/logs/eclipse-logs.txt` (`$XDG_DATA_HOME` if set). |
| webOS | Same setting; **Share logs…** shows an address (`http://<TV>:47900/<token>/`): open it on a phone or PC on the same network and download **everything as one file** (`all.txt`). | Not retrievable while it never starts (the page needs the app running). The kept logs survive and come out with the next start that works (e.g. after an update). |
| Android TV | Same as webOS: **Share logs…** serves the same page. | With ADB, `adb logcat -s Eclipse` shows the live log ([Android guide](BUILD_ANDROID.md)). |

"Start it twice more" works because of the **run marker** (`logs/running.txt`): a run that does not end cleanly
(crash, frozen and closed by force, failed start, power loss) makes the NEXT start keep its log even with the setting
off, and on Windows/Linux that start rewrites `eclipse-logs.txt` at once — so the file is current without ever reaching
the Share screen. A normal close (window close, SIGTERM, the TV's app manager) removes the marker: measured on all three.

## What the one file contains (top to bottom)

1. **Report** of the moment it was made: build (`App:` version, commit, app id; `Binary:` path, size, **sha256** — the
   version is not bumped per build, the hash identifies the file), time + UTC offset, OS (Windows edition/build/UBR;
   Linux distro, session type, GPU drivers from `/sys/class/drm`; webOS release, firmware, device), CPU, memory, GPUs and
   driver versions, SDL drivers, settings/log folders, displays (webOS: the panel), video/audio modules and their
   capabilities, audio devices, controllers AND the input devices not used as controllers, every network interface
   (addresses, gateways, speed, MAC), the default route, the webOS wired-network state, every known computer with what it
   last reported (state, codecs, 4K/HDR, Umbra caps, running app, lock screen), and the stream/input/co-op settings.
2. **The last 5 kept logs**, newest first. Each starts with a **start report** (the same, minus what needs video/audio:
   so a run that died while starting still says what it ran on) and has the full report once the app is up. In them:
   - `[Host]` a line per computer whenever what it reports changes;
   - `[PCManager] <action> failed on <computer or address>: <reason>` when something the user asked of a host
     (pairing, quitting the running app, wake-on-LAN, adding a computer) failed -- the host's own answer or the
     network error, not just the dialog's text;
   - `Session start` (host, codecs offered), a `[Stats]` line every 10 s (fps, loss, decode time, kbps),
     `Session end` / `FAILED before connecting` / `FAILED TO START` (the error in words and its code), `Stream totals`;
   - `===== HOST LOG … =====`: the tail (256 KB) of the host's own log, fetched at the end of every session from an
     Umbra host that offers it (`UmbraCaps` bit 1, Umbra ≥ d4d79b59), secrets redacted by the host;
   - `The previous run (…) did not end cleanly …` when the run before crashed or was killed;
   - `=== CRASH …` / `=== UI THREAD STUCK …` records (below).
3. Co-op and streaming diagnostics, the co-op second PC's log, the Windows this-PC host hand-over log.
4. The settings with PIN / password / token / secret lines removed, and the computers list.

No PIN, keys or passwords are in it. It does contain computer names, IP and MAC addresses.

## Crash and frozen-UI records

- `=== CRASH: …`: Windows — exception code, `module+offset`, access-violation detail, the stack as `module+offset`
  frames. Linux — signal, fault address (or, for a signal something SENT: `sent by pid P (uid U)` / `raised by this
  process itself (abort/raise)`), pc, a backtrace, `/proc/self/maps`. webOS — the same plus `lr`, `sp` and 128 raw stack
  words: 32-bit ARM backtraces come out EMPTY, the words are the walk.
- `=== UI THREAD STUCK: …`: the UI thread did not run for 5 s (20 s while starting, 15 s while closing): its stack, again
  25 s later if still stuck, and `The UI thread is running again, after about N s stuck` when it moves.

**Name the frames** with the exact binary the log names (sha256 in `Binary:`), as shipped — every package keeps its
symbols, nothing extra is archived:

    scripts/support/crash_frames.py eclipse-logs.txt moonlight-tv.exe      # Windows: from the release zip
    scripts/support/crash_frames.py eclipse-logs.txt bin/moonlight-tv      # Linux: from the tarball
    scripts/support/crash_frames.py all.txt bin/aurora                     # webOS: ar x the .ipk; tar xzf data.tar.gz

Stack-word hits are "possible return addresses": read them as the chain (e.g. `app_handle_launch` then `main`), stale
words appear too.

## Test hooks (to re-test all of this)

| Hook | Where | Effect (only while a log is kept) |
|---|---|---|
| `ECLIPSE_TEST_CRASH=early` | env, desktop | crash right after the start report |
| `ECLIPSE_TEST_CRASH=1` | env, desktop | crash once the app is up |
| `ECLIPSE_TEST_HANG=N` | env, desktop | the UI thread sleeps N s on its first pass |
| `{"aurora_test_crash":true}` | webOS launch parameters, **beta builds only** | `raise(SIGSEGV)` at launch |
| `{"aurora_test_hang":N}` | webOS launch parameters, **beta builds only** | the UI thread sleeps N s |

Verified 2026-09-30 on Linux, Windows (VEGA) and the TV (webOS 26, in standby): unclean end → next start kept with the
setting off; crash while starting; frozen UI; clean close removes the marker; frames named by `crash_frames.py`.

## Known limits

- webOS: an app that crashes at EVERY start cannot hand anything over (no file a user can reach).
- Windows has no network page (Windows Firewall's prompt appeared over the fullscreen app); the file is the way out.
- A host without `UmbraCaps` bit 1 (stock Sunshine/Apollo, or Umbra before d4d79b59) gives no host log:
  `Host log: this host does not offer its log`.
