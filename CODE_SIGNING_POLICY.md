# Code signing policy

**Status:** signing through SignPath Foundation is being set up. Every package of every release up to and including Eclipse 1.3.7 is **unsigned**, except the Android APK (below). Until signing is approved, check each download against its published SHA-256.

**Android:** Android installs only signed APKs, and an update only over an install signed with the same key. From its first release (1.3.5), Eclipse for Android is signed with the project's own release key, kept by the maintainer, and every update will be signed with it. Its certificate SHA-256 is `8C:31:18:05:69:36:39:EF:2A:F8:28:63:9A:31:DE:9B:F4:DB:CC:28:AA:2A:5B:98:89:F3:E6:04:75:26:EF:AC`; check a download with `apksigner verify --print-certs eclipse-android.apk`.

Once approved: free code signing provided by [SignPath.io](https://about.signpath.io/), certificate by [SignPath Foundation](https://signpath.org/).

## What is signed

- The Windows program (`moonlight-tv.exe`) and the Windows installer (`eclipse-windows-x64-setup.exe`) of each release.
- They are built by the [`release-windows-signed.yml`](.github/workflows/release-windows-signed.yml) GitHub Actions workflow, from the release tag in this repository. Nothing built on a personal machine is sent for signing.
- Third-party libraries in the package (SDL, FFmpeg and the other DLLs listed in `THIRD-PARTY-NOTICES.txt`) are shipped as their projects built them and are not signed by this project.
- The LG webOS and Linux packages don't use Authenticode. Verify them with the published SHA-256 files.

## Team roles

| Role | Members |
|---|---|
| Committers and reviewers | [PreceptorOfMagic](https://github.com/PreceptorOfMagic) |
| Approvers | [PreceptorOfMagic](https://github.com/PreceptorOfMagic) |

- **Committers** can change the source directly. Contributions from anyone else arrive as pull requests, and a reviewer must approve them before they are merged.
- **Approvers** approve each signing request in SignPath, one release at a time.
- **AI assistance:** parts of this project are written and debugged with AI coding assistants (Claude, Codex). They work under the maintainer's account and direction. The maintainer reviews and tests what they produce before it is merged or released. They have no separate access and cannot approve signing.
- **Sign-in security:** every team member uses multi-factor authentication for GitHub and SignPath.

## Privacy

Eclipse collects no personal data and has no telemetry or analytics. It makes these network connections, and no
others:

- to the game-streaming PCs you add or pair, and to PCs it discovers on your local network (mDNS);
- a Wake-on-LAN packet when you ask it to wake a PC, or when you start co-op with a PC that is asleep;
- a support-log download page on your local network, only while you use **Share logs**;
- at most once a day, when its main screen opens, a download of the Project Hub's list of releases
  (`https://preceptorofmagic.github.io/Eclipse-Umbra-Project-Hub/updates.json`), to show a dot when a newer version
  is out. **Settings > About > Check for updates** turns it off;
- at start, a download of the community controller database (SDL_GameControllerDB) from GitHub, so that newly
  released controllers are recognised.

The two downloads send nothing about you, your device or your PCs beyond what any web request carries: your IP
address and the program's name and version. Both are served by GitHub, whose
[privacy statement](https://docs.github.com/site-policy/privacy-policies/github-general-privacy-statement) applies to
them.

## Reporting

Report a signed binary that you think is malicious, or a breach of this policy, through this repository's issues. If it concerns the certificate itself, also report it to [SignPath Foundation](https://signpath.org/).
