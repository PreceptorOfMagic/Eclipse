# Update notice

Eclipse tells its user that a newer version is out with one small dot, and shows what the new version
brings. The list of releases lives in one file on the Project Hub, `site/updates.json`
(published at `https://preceptorofmagic.github.io/Eclipse-Umbra-Project-Hub/updates.json`), which the
release process edits with `scripts/updates/updates.mjs` in the hub repository.

## What the user sees

- **A dot** on the launcher's Settings button and on the About entry in Settings, while there is an
  announced release newer than the installed one that the user has not looked at yet.
- **Settings > About**: one line, "Eclipse 1.3.7 is available", and a **What's new** button. It lists
  every release since the installed one (newest first, with its highlights), then how to update on this
  platform. On Windows and Linux a **Download page** button opens the release page; on the TV and the
  SHIELD the address is shown instead. Opening it removes the dot for that version; the About line stays
  until the update is installed.
- **Nothing else**: no pop-up, nothing on the stream screen, no message when the check fails.
- **Settings > About > Check for updates** (on by default, `check_updates` in `moonlight.ini`) turns the
  download off and hides a notice already found.

## When it checks

At most once a day, when the launcher opens. The launcher is never on screen during a stream, so the check
never runs beside one. The download runs on the app's background worker, times out after 15 seconds, and
is aborted when the app closes. A failed check is logged (`[Update] Check failed ...`) and retried at a
later launcher open, not sooner than an hour later in the same run. The last good copy is kept in the
settings folder (`updates-cache.json`, beside `update-state.ini`), so a known update shows at once and
offline. Only the file is downloaded: nothing about the user, the device or their PCs is sent, beyond what
any web request carries (the address, and a `User-Agent` of `Eclipse/<version>`).

## The file

```json
{ "schema": 1,
  "eclipse": {
    "download_url": "https://github.com/PreceptorOfMagic/Eclipse/releases/latest",
    "how_to_update": { "webos": "...", "windows": "...", "linux": "...", "android": "..." },
    "releases": [
      { "version": "1.3.7", "date": "2026-11-20",
        "notify": true,
        "platforms": ["windows", "linux"],
        "highlights": ["...", "..."],
        "notes_url": "https://github.com/PreceptorOfMagic/Eclipse/releases/tag/eclipse-v1.3.7" } ] },
  "umbra": { ... } }
```

- `notify` is optional and defaults to `true`. With `false`, the release is **silent**: it never shows a
  dot on its own. When a later release is announced, the silent ones between the installed version and it
  are listed with it, because installing the later one brings their changes too.
- `platforms` is optional and defaults to every platform. It limits a release, silent or not.
- Highlights are plain text, at most 10 per release and 160 characters each. Keep to plain words and
  ASCII punctuation: the dialog draws its own "-" bullets, and which other symbols the app's fonts cover
  has not been checked.
- Umbra (Vibepollo) is NOT on this list: it keeps its own designed update check, which reads GitHub for its
  releases and Vibepollo's and tells the user how they match up (user, 2026-11-14). `--silent` therefore does
  not apply to it.
- Clients read `schema` 1 only, and ignore keys they do not know. A change older clients would misread
  needs `schema: 2`; clients built for 1 then show nothing, which is the safe failure.

## Releasing

In the hub repository, after the GitHub release is published:

```bash
# Announced (the default):
node scripts/updates/updates.mjs add --product eclipse --version 1.3.7 --date 2026-11-20 \
  --notes-url https://github.com/PreceptorOfMagic/Eclipse/releases/tag/eclipse-v1.3.7 \
  --highlight "Windows installers are signed" --highlight "A dot in Settings when an update is out"
# Silent (published, not announced):
node scripts/updates/updates.mjs add --product eclipse --version 1.3.8 --silent ...
# Announce later, or withdraw an announcement:
node scripts/updates/updates.mjs notify --product eclipse --version 1.3.8 on
node scripts/updates/updates.mjs notify --product eclipse --version 1.3.8 off
```

Then commit and push the hub's `main`; its Pages workflow validates the file before it deploys. A release
is silent when the user asks for it, or when the release is not meant for everyone yet (a fix for one
platform, a candidate under test). Add the entry only once the release's files are on GitHub: a notice
for a release that cannot be downloaded yet is worse than none.

## Settings are kept on every update

| Platform | Update path | Why settings survive |
|---|---|---|
| webOS | the new `.ipk` through webOS Dev Manager | same app id; the settings folder is outside the files an install replaces |
| Windows installer | run the new installer over the old one | settings are in the user's app-data folder, not the install folder |
| Windows portable | extract the new ZIP and run it | the same app-data folder as the installer |
| Linux | `flatpak install --user --reinstall <file>.flatpak` | the Flatpak keeps `~/.var/app/...` and the tarball's settings folder |
| SHIELD | install the new APK | same package and the same release signing key |

Code: `src/app/backend/update_check.c` (download, cache, state), `src/app/backend/update_manifest.c`
(which release to show; `tests/app/test_update_manifest.c`), `src/app/util/json_mini.c`,
`src/app/ui/common/update_dot.c`, and the About pane.
