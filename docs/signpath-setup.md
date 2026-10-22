# SignPath code signing: setup and release steps

Eclipse and Umbra Windows releases are to be signed through SignPath Foundation's free open-source programme ([terms](https://signpath.org/terms)). This file lists what is already in the repositories, what only the maintainer can do, the draft application, and how a signed release is made. Prepared 2026-10-10; nothing here has run in GitHub Actions yet, because Actions was blocked on the private repositories (failed payment / spending limit). Public repositories run Actions at no cost.

## Already in the repositories

| Item | Eclipse (`main` from 1.3.4) | Umbra (branch `ci/signpath`) |
|---|---|---|
| Policy page | `CODE_SIGNING_POLICY.md` | `CODE_SIGNING_POLICY.md` |
| Signed-release workflow, started by hand | `.github/workflows/release-windows-signed.yml` | `.github/workflows/release-windows-signed.yml` |
| Copies of the SignPath artifact configurations | `.signpath/artifact-configurations/eclipse-windows-{app,installer}.xml` | `.signpath/artifact-configurations/umbra-windows-{app,installer}.xml` |
| Packaging after signing | `scripts/windows/finalize_signed_package.ps1`, `build_installer.ps1 -OutputName` | in the workflow: strip, sign, then CPack with `UMBRA_PACKAGE_PRESTRIPPED=ON` (`cmake/packaging/common.cmake`, default off) |

**Tested on SKYNET** (2026-10-10, against a copy of the real 1.3.1 Windows package), the Eclipse post-signing script:

- refuses an unsigned program, a different signer, and a package that was already finalised;
- accepts a real timestamped signature;
- re-hashes the package so that `build_installer.ps1` accepts it, and the installer step then fails if any file is changed afterwards.

**Tested on SKYNET for Umbra** (2026-10-10):

- **Why packing had to change:** CPack strips binaries while packing, and stripping removes a signature. With a signed stand-in for `sunshine.exe`, normal packing produced an unsigned, smaller file. With `UMBRA_PACKAGE_PRESTRIPPED=ON` the file was packed byte-identical and still validly signed.
- **The encoder build:** the workflow's download, x265 and GPU HEVC steps, run straight from the workflow file, built both encoder DLLs from the pinned sources. They have the same exports, kernels and licences as the 0.5.0 DLLs, but are about 7 KB smaller (0.4%), probably from a different compiler version. **The first Umbra built by this workflow needs the usual co-op test on the PCs before release.**

**Not tested:** the workflows end to end. They need Actions and SignPath accounts.

**One more dependency:** the Umbra workflow checks out the public Eclipse repository at the 1.3.4 release commit (`6d13e672`) for the encoder source, so the public Eclipse repository must exist first.

## Steps only the maintainer can take

1. **Make the repositories public.** SignPath signs only open-source projects with a public repository, and public repositories also get GitHub Actions at no cost, which ends the failed-payment emails for these builds.
   - **Eclipse goes public as a new repository**, because the current one still holds pre-redaction history through closed pull request #1 (old passwords), which only GitHub Support can remove. Rename the current repository (for example to `Eclipse-dev`; it stays private), create a new public `PreceptorOfMagic/Eclipse`, and push the prepared public history (upstream aurora-tv plus one Eclipse 1.3.4 commit) and the `eclipse-v1.3.4` tag.
   - **Umbra can be made public as it is.** Its whole history was scanned clean. First set its default branch to the release line.
2. **Check GitHub two-factor sign-in is on** (Settings → Password and authentication). SignPath requires it for everyone on the team. The token this machine uses can't read that setting, so it hasn't been checked.
3. **Apply** at [signpath.org](https://signpath.org/). The draft answers are below; apply once per project or ask for both in one application.
4. **After approval**, in SignPath:
   - Project slugs: `eclipse` and `umbra`. If SignPath picks others, set the repository variable `SIGNPATH_PROJECT_SLUG` to match.
   - Signing policy: SignPath Foundation's release policy, usually `release-signing`. If its slug is different, set `SIGNPATH_SIGNING_POLICY_SLUG`.
   - Artifact configurations: create the ones named in `.signpath/artifact-configurations/` and paste each file's XML.
   - Trusted build system: link GitHub.com to both projects, with the repository URLs.
   - Create a CI user with the *submitter* role and generate its API token.
5. **In each GitHub repository** (Settings → Secrets and variables → Actions):
   - secret `SIGNPATH_API_TOKEN`: the CI user's API token;
   - variable `SIGNPATH_ORGANIZATION_ID`: the SignPath organisation ID.
6. **Protect `main` and the release tags** (Settings → Rules → Rulesets):
   - Require a pull request for `main`, so outside contributions need your approval. Allow yourself to bypass the rule, because GitHub won't let you approve your own pull request.
   - Restrict who can create `eclipse-v*` and `umbra-v*` tags.
7. **Merge `ci/signpath` into Umbra's release branch.** Eclipse's public `main` already includes it.
8. **Website:** SignPath asks for the words "Code signing policy" on the home page and the download page, linking to the policy. Add a link from the Project Hub's downloads page.

## Making a signed release

1. Tag and publish the GitHub release with the Linux, TV and source files, exactly as now.
2. Open Actions → **Release (Windows x64, signed)** → *Run workflow*, and enter the tag (for example `eclipse-v1.3.4` or `umbra-v0.5.1`).
3. The workflow builds from that tag and sends **two** signing requests: the program first, then the installer built from the signed program. Approve each one in SignPath. Each request waits up to two hours.
4. The workflow checks each signature (valid, SignPath Foundation, timestamped) and attaches the signed installer and ZIP, with their `.sha256` files, to the release.
5. Edit the release notes to point at the signed files. Remove the `-unsigned` files, or keep them only for older links.

## Draft application answers

- **Project names:** Eclipse (game-streaming client) and Umbra (game-streaming host).
- **Repositories:** https://github.com/PreceptorOfMagic/Eclipse and https://github.com/PreceptorOfMagic/Umbra
- **Homepage:** https://github.com/PreceptorOfMagic/Eclipse-Umbra-Project-Hub
- **Licence:** GPL-3.0 for both. Eclipse is a fork of moonlight-tv; Umbra is a fork of Apollo, itself a fork of Sunshine.
- **Description:** Eclipse streams games and desktops from your own PCs to LG webOS TVs, the NVIDIA SHIELD TV, Windows and Linux, including two-PC split-screen co-op on one TV. Umbra is the host that runs on those PCs, on Windows or Linux.
- **Released already:** yes. Eclipse 1.3.5, Umbra 0.5.0 for Windows and Umbra 0.5.2 for Linux are published as GitHub releases. The Windows packages are unsigned; the Android APK is signed with the project's own key, as Android requires.
- **What would be signed:**
  - Eclipse: `moonlight-tv.exe` and the NSIS installer.
  - Umbra: the executables and DLLs built from the Umbra repository (`sunshine.exe`, the tools, the GPU HEVC co-op encoder DLLs) and the NSIS installer.
- **Build system:** GitHub Actions on `windows-2022`, from release tags, one manual run per release. Every build input is pinned by version or hash.
- **Third-party binaries shipped unsigned or under their own signatures:**
  - Eclipse: MSYS2 DLLs (SDL, FFmpeg, and others), listed in `THIRD-PARTY-NOTICES.txt`.
  - Umbra: the ViGEmBus installer (signed by Nefarius), the SudoVDA virtual display driver (signed by its author), and `nefconc.exe`, built from nefcon's MIT source.
- **Team:** one maintainer (PreceptorOfMagic), who is author, reviewer and approver. Development uses AI coding assistants under the maintainer's account, and the maintainer reviews and tests their output.

## Points SignPath may question (Umbra)

- **The SudoVDA certificate.** The Umbra installer (inherited from Apollo) adds SudoVDA's self-signed code-signing certificate to the machine's *Trusted Root* and *Trusted Publisher* stores, so that the virtual display driver installs (`src_assets/windows/drivers/sudovda/install.bat`). This is not mentioned in the installer's wording today, and SignPath's terms ask that system changes come with clear warnings. Options, in order of effort:
  1. Say so in the installer's SudoVDA component description and in the policy.
  2. Remove the certificate when the driver is removed. Today `uninstall.bat` removes only the device, and the certificate stays trusted after Umbra is uninstalled.
  3. Use a virtual display driver signed through Microsoft's own driver signing (WHQL or attestation), so no certificate needs installing.
- **The settings page checks GitHub for new versions.** When you open the settings page, it asks GitHub for the latest Apollo and Umbra releases. This is disclosed in Umbra's policy page.
