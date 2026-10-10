# webOS 26: where the TV app keeps its files, and what a TV restart does to them

Written 2026-10-10 after two releases in a row shipped a folder-permission bug: 1.3.0/1.3.1 lost settings after a restart, and 1.3.0–1.3.2 could not start co-op after one. This is the audit of every place the release TV app reads or writes files. Read it before changing anything in `src/app/platform/webos/path_webos.c` or `src/app/platform/posix/runtime_files.c`.

## What the TV does at every boot (measured)

- The whole Developer Mode app tree, `/media/developer/apps/usr/palm/applications/<id>/`, is set to mode `775`, recursively.
  - Owners are kept: the install files belong to `1000:1000`, and what the app created belongs to the app's uid with group `jailer`.
  - The change time of every entry is the boot instant.
- `userdata/` goes from `1777` (packaged, sticky) to `1000:1000 0775`. **The app can no longer create anything in it**: it isn't owner 1000 or in group 1000.
- The app's own `conf/`, `cache/` and `cache/runtime/`, and all files in them, become `0775`, writable by group `jailer`, which other Developer Mode apps and the `prisoner` login share.
- `/media/developer/temp/<id>/…` is reset the same way.
- `/tmp` is a tmpfs and starts empty.
- `ares-install` over an existing install does not restore `userdata/` to `1777`.

The app runs as uid 6584 (live app id), gid `jailer` (5000), plus system groups. It is not in group 1000.

## Every place the release TV app touches files

| Feature | Location | Created by | After a restart | Status |
|---|---|---|---|---|
| Settings, PC list, controller DB (`moonlight.ini`, `hosts.ini`, `gamecontrollerdb.txt`) | `path_pref()`: `$HOME/conf` if it exists, else `userdata/conf`, else `/media/developer/temp/<id>/conf` | app | `userdata/conf` was refused (group-writable) | Fixed 1.3.2 (`fa2bf8b7`): re-closed to `0700` |
| Pairing keys (`conf/key/*`) | under settings | libgamestream `file_compat.c` | files `0775` | Fine: every open checks the owner and re-closes to `0600` |
| Kept logs, Share logs (`conf/logs`) | under settings | `diag_logs.c` | `0775` | Fine: needs only owner write access |
| Runtime files: `/tmp/coop-*`, `/tmp/coop_*`, `/tmp/aurora-*` (diagnostics log, co-op coordinator/audio/input sockets, co-op sentinels) | release builds redirect them to `cache/runtime` (`runtime_files.c`) | app | `path_dir_ensure(userdata)` needed a writable parent, so the cache was refused; the `0775` runtime leaf was refused too. **Co-op could not start** (9003, "the second co-op PC did not begin streaming in time") and the diagnostics file was missing | Fixed 1.3.3 (`4e040cb1`) |
| Box art | `path_cache()`: `userdata/cache`, else `/tmp/moonlight-tv/cache` | app | Fell back to `/tmp`, so it was re-downloaded after every restart | Fixed 1.3.3 (same change: an existing owned cache is reused) |
| Fonts, translations, controller mappings, NDL module | install tree (`path_assets()`, `lib/`) | installer | `775`, read-only use | Fine |
| Co-op second process | `/proc/self/exe` | — | — | Fine. It skips kept logs and uses the same settings and runtime lookups |
| System info | `/proc`, `/sys`, `/var/run/nyx/*`, `/etc/os-release` | system | read-only | Fine |
| Opt-in probes (`/tmp/avsync-*`, `/tmp/ndl_pipeline_probe`) | shared `/tmp` | only if a developer creates the trigger file | emptied | Fine |

Beta builds (`AURORA_TEST_HOOKS`) do not redirect runtime files, so the beta app never showed the co-op failure. Test folder behaviour on the **release** app id.

## The "restarted before first launch" case

If the TV restarts after the IPK is installed but before Eclipse is first opened, `userdata/` is already unwritable and empty:

- **Settings:** `/media/developer/temp/<id>/conf`. It works, and it survives restarts (measured 2026-10-10).
- **Box art:** `/tmp/moonlight-tv/cache`. It works, but isn't kept across restarts.
- **Runtime:** had no fallback, so co-op could never start on such an install. Fixed in 1.3.3 (`ec08cce3`): `/media/developer/temp/<id>/cache`, owned by the app and closed to `0700`.

Uninstalling leaves `/media/developer/temp/<id>/` behind, as it already did for settings.

## Rules for future changes

1. **Assume every directory the app made becomes `0775` at boot, and `userdata/` becomes unwritable.** Any check that refuses a group-writable directory of the app's own must re-close it (`fchmod 0700`) instead.
2. **Never require a writable parent to use a directory that already exists.** `path_dir_ensure()` checks `W_OK` on the target, which is fine for the app's own folders but fails for `userdata/`.
3. **A group-writable container is a risk only if the group is one of ours** (`path_container_shared()`). World-writable without the sticky bit is always a risk.
4. **Test on the release app id and through a real restart:** `lgtv.py off` makes webOS 26 reboot itself within about 30 s, and `lgtv.py on` wakes it. **Start co-op, not just open its screen.** Opening the screen does not touch the runtime folder.
5. The settings-path tests (`scripts/webos/test_path_preference.py`) and runtime tests (`scripts/linux/test_runtime_files.py`) cover each state above. Add a case whenever a new folder state is found.

## Not controllable

Between a boot and the app's next start, the app's files are writable by group `jailer`. The app closes them again when it starts; it cannot prevent the boot reset.
