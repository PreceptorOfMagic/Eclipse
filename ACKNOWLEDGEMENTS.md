# Acknowledgements

Eclipse/Umbra is only possible because generations of game-streaming developers published their work. Eclipse is the client application and Umbra is the host application; this page gives prominent credit to both lineages and records the major projects used by the wider system.

It is also important to say what this page is **not**: acknowledgements do not replace licence texts, copyright notices, modification notices, corresponding source, relinkable material, NOTICE files or other obligations that must travel with a particular source or binary release.

The client fork's derivative and GPL-3.0-or-later declaration is recorded separately in [COPYRIGHT](COPYRIGHT); file-level and component notices remain in force.

## The lineage

```text
Moonlight
  ├─ moonlight-common-c
  └─ Moonlight Embedded → Moonlight TV → Aurora → Eclipse

Sunshine → Apollo → Umbra

SudoVDA provides companion virtual-display functionality.
```

This is an independent community project. Eclipse/Umbra is not affiliated with or endorsed by the upstream projects, LG Electronics, Valve, NVIDIA, AMD, Amazon, DXC, the Eclipse Foundation or other named vendors.

## People and projects at the foundation

### Moonlight

[Moonlight](https://moonlight-stream.org/) was created at MHacks in 2013 by **Cameron Gutman, Diego Waxemberg, Aidan Campbell, Aaron Neyer, Michelle Bergeron and Andrew Hennessy**, and has since been built by the wider Moonlight community. Moonlight’s own [Project and Community page](https://github.com/moonlight-stream/moonlight-docs/wiki/Project-and-Community) is the source for this attribution.

This client uses [moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c), the Moonlight community’s core protocol implementation, and descends through [Moonlight Embedded](https://github.com/moonlight-stream/moonlight-embedded), created by **Iwan Timmer** and contributors.

The desktop video module also incorporates and adapts decoding and presentation work from [Moonlight Qt](https://github.com/moonlight-stream/moonlight-qt) at commit [`032529d`](https://github.com/moonlight-stream/moonlight-qt/commit/032529d782242e3833e0b3b147dbbf96e878e3ca). Moonlight Qt is GPL-3.0-or-later work maintained by the Moonlight Game Streaming Project and contributors; its copyright, licence, exact source reference and this fork's modification record must accompany desktop distributions.

moonlight-common-c's Reed–Solomon forward-error-correction implementation credits **Luigi Rizzo, Alain Knaff and Iwan Timmer**, with portions derived from work by **Phil Karn, Robert Morelos-Zaragoza and Hari Thirumoorthy**. Its BSD-style notice is already present in the webOS notice bundle; source distributions must retain it, and binary distributions must reproduce it in their documentation or other accompanying material.

### Sunshine

[Sunshine](https://github.com/LizardByte/Sunshine) was originally created by **[@loki-47-6F-64](https://github.com/loki-47-6F-64/sunshine)** and is now maintained and developed by **[LizardByte](https://github.com/LizardByte)** and contributors. Sunshine provides the open host foundation from which Apollo and the matching host fork descend.

### Moonlight TV

[Moonlight TV](https://github.com/mariotaku/moonlight-tv) was created by **Mariotaku / Ningyuan Li** and contributors. Its LVGL large-screen client, webOS work and associated support libraries form the direct client foundation used by Aurora and this fork.

### Aurora

[Aurora](https://github.com/GuiDev1994/aurora-tv) was created by **[@GuiDev1994](https://github.com/GuiDev1994)** and contributors as a Moonlight TV derivative. This repository retains Aurora-era compatibility names in parts of the source and packaging.

### Apollo

[Apollo](https://github.com/ClassicOldSong/Apollo) is developed by **[@ClassicOldSong](https://github.com/ClassicOldSong)** and contributors as a Sunshine-derived host with a virtual-display-oriented client workflow. It is the direct host foundation for Umbra.

### Companion projects

- [SudoVDA](https://github.com/SudoMaker/SudoVDA), by **SudoMaker** and contributors, supplies virtual-display functionality while carrying inherited Microsoft/VirtualDrivers lineage that must remain credited in its distributions.
- Current fork integration and testing are maintained by **[@PreceptorOfMagic](https://github.com/PreceptorOfMagic)** with the people who report, test, review and contribute changes.

## Client source and runtime dependencies

The exact set varies by platform and build. Licence links below describe the upstream project; every release must use the licence and notices for the exact revision and configuration it distributes.

| Project | Role | Licence family / source |
|---|---|---|
| [moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c) | Core streaming protocol | GPL-3.0; [local licence](core/moonlight-common-c/LICENSE.txt) |
| [ENet](https://github.com/lsalzman/enet) | Network transport dependency | MIT; [local licence](core/moonlight-common-c/enet/LICENSE) |
| [SS4S](https://github.com/mariotaku/ss4s) | Streaming/media support | LGPL-3.0; [local licence](third_party/ss4s/LICENSE) |
| [commons-c](https://github.com/mariotaku/commons-c) | Shared C utilities | Upstream project is MIT; two attributed copied snippets require separate source/licence resolution before release |
| [LVGL](https://github.com/lvgl/lvgl) / [Mariotaku fork](https://github.com/mariotaku/lvgl) | User interface | MIT |
| [libmicrodns](https://github.com/videolabs/libmicrodns) | Local service discovery | LGPL-2.1; [local copying file](third_party/libmicrodns/COPYING) |
| [SDL](https://github.com/libsdl-org/SDL) and [SDL_image](https://github.com/libsdl-org/SDL_image) | Window, input and image support | zlib |
| [SDL-webOS](https://github.com/webosbrew/SDL-webOS) | webOS SDL port | Upstream project notices apply |
| [SDL_GameControllerDB](https://github.com/gabomdq/SDL_GameControllerDB) | Controller mappings | zlib |
| [Google Material Icons](https://github.com/google/material-design-icons) | Interface icon font | Apache-2.0 |
| [Montserrat 7.200](https://github.com/JulietaUla/Montserrat/releases/tag/v7.200) | Embedded LVGL text glyph data | Copyright 2011 The Montserrat Project Authors; SIL Open Font License 1.1; begun by Julieta Ulanovsky and developed by its contributors |
| [Font Awesome Free 5.9.0](https://github.com/FortAwesome/Font-Awesome/releases/tag/5.9.0) | Embedded Solid, Brands and Regular symbols in the LVGL font data | Font glyphs are SIL Open Font License 1.1; Copyright © Font Awesome |
| [Unscii 1.0](https://github.com/viznut/unscii) | Embedded LVGL 8-pixel font data | Unscii-8 by Ville-Matias “Viznut” Heikkilä; upstream offers this variant as public domain or CC0 |
| [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) | TLS/cryptographic support | Apache-2.0 for relevant current versions |
| [Opus](https://github.com/xiph/opus) | Audio codec | BSD-style redistribution terms; its `COPYING` also records royalty-free patent-licence references |
| [curl/libcurl](https://github.com/curl/curl) | HTTP transport | curl licence |
| [Expat](https://github.com/libexpat/libexpat) | XML parsing | MIT |
| [FreeType](https://gitlab.freedesktop.org/freetype/freetype) | Font rendering | FreeType licence or GPL-2.0 |
| [Fontconfig](https://gitlab.freedesktop.org/fontconfig/fontconfig) | Font discovery | MIT-style |
| [inih](https://github.com/benhoyt/inih) | INI parsing | BSD-3-Clause |
| [FFmpeg](https://github.com/FFmpeg/FFmpeg) | Media pipeline components | LGPL/GPL depending exact configuration |
| [OpenSSL](https://github.com/openssl/openssl) | TLS in applicable desktop builds | Apache-2.0 for current versions |
| [h264bitstream](https://github.com/aizvorski/h264bitstream) | Tracked experimental/dormant codec tooling | LGPL-2.1; [local licence](third_party/h264bitstream/LICENSE) |
| [CMake](https://github.com/Kitware/CMake) | Copied build modules | BSD-3-Clause notices |
| [Unity](https://github.com/ThrowTheSwitch/Unity), by Mike Karlesky, Mark VanderVoord, Greg Williams and contributors | Client unit-test framework; development only | MIT; [local licence](third_party/Unity/LICENSE.txt), pinned submodule revision `8ba01386008196a92ef4fdbdb0b00f2434c79563` |
| [Sharp](https://github.com/lovell/sharp), by Lovell Fuller and contributors, with [libvips](https://github.com/libvips/libvips) and the colour-package contributors | Pinned launch-tool pipeline that renders the social card and webOS artwork; development only, not part of the static site or application runtime | Sharp Apache-2.0; the locked Linux libvips package declares LGPL-3.0-or-later; colour dependencies are MIT; exact versions are in `scripts/launch/package-lock.json` |

Platform and build projects also include [libpbnjson](https://github.com/webosose/libpbnjson), [webOS OSE](https://github.com/webosose), [webos-userland](https://github.com/webosbrew/webos-userland) and [buildroot-nc4](https://github.com/openlgtv/buildroot-nc4). This inventory covers source, linked or vendored components, generated-asset inputs and project-specific build/test dependencies; it is not intended to enumerate every general-purpose utility on a maintainer's workstation. Whether a notice belongs in a binary artifact depends on what that artifact actually redistributes.

## Umbra and Apollo dependencies

The host’s release-specific notice/SBOM must be generated from its exact dependency graph. Major projects include:

- The ClassicOldSong fork of [moonlight-common-c](https://github.com/ClassicOldSong/moonlight-common-c) and its [ENet](https://github.com/lsalzman/enet) transport dependency. Umbra compiles/links these itself; they are not only Eclipse dependencies.
- [libdisplaydevice](https://github.com/LizardByte/libdisplaydevice) — its checked-out `LICENSE` says AGPL-3.0 while its current CMake metadata says GPL-3.0. That upstream inconsistency must be resolved against the exact revision before release; if AGPL governs the linked/network service, its source-availability terms need explicit treatment.
- [shared-web](https://github.com/LizardByte/shared-web) — AGPL-3.0-only in the package lock. No source import was found in the current audit, so treat it as a package/build input unless the final Vite bundle proves its code ships.
- [inputtino](https://github.com/games-on-whales/inputtino) — MIT.
- [Simple-Web-Server fork](https://github.com/ClassicOldSong/Simple-Web-Server) — MIT, by Ole Christian Eidheim and contributors.
- [ViGEmClient](https://github.com/LizardByte/Virtual-Gamepad-Emulation-Client) — MIT, Copyright © 2017–2023 Nefarius Software Solutions e.U. and contributors; the checked-out client revision is `8d71f674`.
- [ViGEmBus 1.21.442.0](https://github.com/nefarius/ViGEmBus/releases/tag/v1.21.442.0) — BSD-3-Clause, Nefarius Software Solutions. This is a separate signed driver installer and needs its own notice and artifact verification.
- [qrcodejs](https://github.com/davidshimjs/qrcodejs) by David Shim — MIT. The bundled minified file matches upstream commit `04f46c6`, has no licence header and therefore needs an outer notice.
- [nanors](https://github.com/sleepybishop/nanors) and [tray](https://github.com/LizardByte/tray) — permissive projects whose exact notices, including nanors' nested `sse2neon` material where shipped, must be preserved.
- [TPCircularBuffer](https://github.com/michaeltyson/TPCircularBuffer) — its podspec says MIT, while the redistributed C/H files carry zlib-style terms including an altered-source marking condition. Preserve those file notices and resolve the metadata inconsistency rather than labelling the component generically “MIT-family.”
- [Wayland protocols](https://gitlab.freedesktop.org/wayland/wayland-protocols) and [wlr protocols](https://gitlab.freedesktop.org/wlroots/wlr-protocols) — MIT-style, including per-file notices.
- [nv-codec-headers](https://github.com/FFmpeg/nv-codec-headers) and [NVAPI SDK fork](https://github.com/LizardByte/nvapi-open-source-sdk) — NVIDIA notices.
- Vue, Vue I18n, Bootstrap, Popper, Boost, nlohmann JSON, miniupnpc, MinHook, zlib, Opus, curl and OpenSSL — exact licences and notices must follow the shipped Windows link/runtime closure. The current generated static-curl link record also names libssh2, Brotli, zstd, libpsl, libunistring, libiconv and libidn2; their LGPL/GPL/permissive source and relinking implications must be assessed from the release artifact.
- Font Awesome Free — mixed CC-BY-4.0, OFL-1.1 and MIT terms depending the redistributed asset. Bootstrap 5.3.7 and Font Awesome 6.7.2 are imported into the current web application.
- FFmpeg, x264, x265, SVT-AV1, Intel VPL and AMD AMF — exact source and build configuration must match the released static media stack. The checked-out binary-distribution submodule is commit `cf5dffaf`; its commit points to [LizardByte build-deps recipe `6ad5cf8`](https://github.com/LizardByte/build-deps/tree/6ad5cf841f592f95be47fb401cde02ae621acd0f). That recipe pins FFmpeg `9373b442` (`n7.1.1-20`), x264 `ff620d0`/build 165, x265 `1d117be`/4.1, SVT-AV1 `08c18ba`/1.6.0, AMF `16f7d73` and nv-codec-headers `22441b5`; both the binary-dist and recipe commits belong in the reproducibility record.
- Umbra statically links x265 4.1's `dynamicHDR10`/HDR10+ library, Copyright © 2013–2020 MulticoreWare, Inc., authored by **Bhavna Hariharan and Kavitha Sampath**, offered under GPL-2.0-or-later or separate commercial terms. No commercial licence has been evidenced for this build, so release compliance must proceed under the applicable GPL option. That archive includes [json11](https://github.com/dropbox/json11), Copyright © 2013 Dropbox, Inc., under the MIT License. Preserve both the dynamicHDR10 terms and json11's separate copyright and permission notice.
- Platform-specific inherited source includes GLAD/Khronos material and NVIDIA NvFBC/`helper_math` notices. Build-only tools such as GoogleTest, Doxygen themes, Flatpak tooling, Vite/esbuild and Codecov must be separated from components actually redistributed in a binary.

Apollo’s existing Valve/Steam trademark notice must remain intact. The
SudoVDA/Microsoft indirect-display driver chain needs exact-artifact licence
verification before redistribution; SudoVDA-derived interface headers are also
compiled into the host, so omitting only the incomplete driver directory does
not remove the source-provenance question.

## Experimental GPU HEVC work

The experimental encoder code records provenance from [x265 commit `b81f650`](https://bitbucket.org/multicoreware/x265_git/src/b81f650e21e8aacbe6a9ad04ce14aefc05b932c0/) and [Khronos OpenCL-Headers](https://github.com/KhronosGroup/OpenCL-Headers). A distributed combined work needs the applicable GPL-3.0 terms, x265 notices, exact corresponding source and build scripts. Codec copyright licences do not themselves grant every patent right that may apply in every jurisdiction.

## Release-compliance work still open

As of 1 October 2026, the following are launch blockers rather than completed credits work:

- The webOS packaging rule now stages the project `LICENSE.txt`, `COPYRIGHT` and this acknowledgement file. Archive inspection of the explicitly `UNTESTED` 30 September 2026 build confirmed all three paths, and each copy matched the input used for that build; the canonical deploy IPK was then restored to its pre-build known state. A public release still needs the exact artifact-specific third-party notice bundle and an immutable corresponding-source reference/archive.
- Umbra's source packaging rules now install its root GPL licence, the existing Valve/Steam `NOTICE`, a targeted `THIRD-PARTY-NOTICES.md`, exact x265/dynamicHDR10/json11, ViGEmClient, ViGEmBus and qrcodejs texts, and a blocked source-manifest template. No generated NSIS/ZIP artifact has yet verified those working-tree rules, and the targeted bundle is not a complete SBOM, modification record or immutable corresponding-source delivery. Development source uses branch-independent `/umbra/blob/HEAD/...` legal links, but the final built web UI and installer still need inspection before that fix can be credited to a release.
- Umbra's targeted notice bundle now distinguishes ViGEmClient/MIT from the separately installed ViGEmBus/BSD-3-Clause artifact and preserves qrcodejs's outer MIT notice. The final package must byte-match those source notices and still cover the rest of the exact Windows static-link/runtime closure described above.
- Umbra packaging expects `SudoVDA.dll`, but the tracked driver directory does not contain it while the INF requires it and identifies version 1.10.9.289. The current SudoVDA source HEAD predates that INF, has no standalone licence and describes mixed SudoMaker/Microsoft/VirtualDrivers provenance. No public release may claim a source/licence match until the exact binary, source and inherited terms are established.
- Umbra's development source now identifies PreceptorOfMagic as the fork developer, derives package vendor/contact from that setting and labels the executable for Umbra contributors rather than SudoMaker/Apollo. Verify those values in the built installer and signed executable, and replace the maintainer handle with the exact legal publisher if signing/distribution requires one; factual SudoVDA and Apollo credit remains separate.
- Umbra's GPL-3.0-only declaration must be reconciled with the checked-out `libdisplaydevice` AGPL/GPL inconsistency and any LGPL/GPL static-link or relinking obligations. Generate notices, an SBOM, exact source/build references and an installer-content scan from the final artifact.
- The webOS and Windows packaging rules now stage Moonlight Qt, moonlight-common-c Reed–Solomon, Montserrat 7.200, Font Awesome Free 5.9.0 and Unscii-8 notices/provenance. No generated release artifact has yet verified those working-tree rules; clean artifact inspection and byte comparison remain mandatory. The generated Font Awesome sources include Brands glyphs, so the upstream trade-mark warning must remain unless a future regeneration proves those glyphs were removed.
- Applying `patches/client-deps/ss4s.patch` to the pinned SS4S base materialises the Moonlight Qt-derived desktop module as an untracked submodule-worktree file; reverse-apply verification confirms that the tracked patch reproduces it. A release must publish that exact base plus patch as immutable Corresponding Source, and ship the upstream GPL material and dated modification record with the binary.
- `third_party/commons/util/linked-list/linked_list.h` attributes token-pasting macros to a Stack Overflow answer and `sortedinsert` to a GeeksforGeeks example. The upstream commons-c MIT label does not establish permission for those separately sourced snippets; reimplement or document compatible source terms and attribution before release.
- Every download needs release-specific corresponding-source URLs/archives and build material, not merely a link to a moving default branch.

The Windows portable notice generator already pins downloaded inputs, verifies hashes, walks its runtime dependency closure and emits notices; it remains the model to extend, with a prominent exact-source reference added per release.

## Supporting upstream work

Funding this fork must never be presented as funding its upstreams. To support an upstream project, follow the verified funding links on that project’s own repository or maintainer profile. This project's support policy and current status are documented separately in [SUPPORT.md](SUPPORT.md).

## Corrections

Attribution and dependency graphs change. If a name, role, licence or link here is incomplete, open an issue with the primary source and the affected release/artifact. Compliance questions should be checked against exact source and package contents; this inventory is engineering documentation, not legal advice.
