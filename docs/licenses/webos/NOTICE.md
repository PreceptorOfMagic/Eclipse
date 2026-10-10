# webOS third-party notice index

This file is the human-readable index for licence texts staged inside the webOS
application package. It supplements the project `LICENSE.txt`, `COPYRIGHT` and
`ACKNOWLEDGEMENTS.md`; it does not replace them.

## Code and libraries redistributed in the package

| Component | How it is distributed | Licence | Packaged text |
|---|---|---|---|
| Moonlight Common C and this client derivative | Linked into `bin/aurora` | GPL-3.0-or-later for this distribution | `../LICENSE.txt` and `../COPYRIGHT` |
| Moonlight Common C Reed-Solomon implementation | Linked into `bin/aurora` | BSD-style redistribution terms | `Moonlight-Common-C-Reed-Solomon-BSD.txt` |
| ENet | Linked into `bin/aurora` | MIT | `ENet-MIT.txt` |
| commons-c, including the webOS curl ABI shim | Linked into `bin/aurora` and `lib/libcurl.so.4` | MIT | `commons-c-MIT.txt` |
| LVGL | Linked into `bin/aurora` | MIT | `LVGL-MIT.txt` |
| Montserrat 7.200 | Generated text glyph arrays linked into `bin/aurora` | SIL Open Font License 1.1 | `Montserrat-7.200-OFL-1.1.txt` and `LVGL-Embedded-Fonts-NOTICE.md` |
| Font Awesome Free 5.9.0 | Generated Solid, Brands and Regular symbol glyphs linked into `bin/aurora` | SIL Open Font License 1.1 for the font; upstream trade-mark warning retained | `Font-Awesome-Free-5.9.0-LICENSE.txt`, `SIL-OFL-1.1.txt` and `LVGL-Embedded-Fonts-NOTICE.md` |
| Unscii-8 1.0 | Generated 8-pixel glyph array linked into `bin/aurora` | Upstream public-domain/CC0 statement; named GPL exceptions are not the embedded asset | `Unscii-8-1.0-PROVENANCE.txt` and `LVGL-Embedded-Fonts-NOTICE.md` |
| SS4S | Loader and media modules under `lib/` | LGPL-3.0 | `SS4S-LGPL-3.0.txt` |
| libmicrodns | `lib/libmicrodns.so.1` | LGPL-2.1-or-later plus ISC-style compatibility-source notices; upstream also offers separate commercial terms | `libmicrodns-LGPL-2.1.txt`, `libmicrodns-AUTHORS.txt` and `libmicrodns-compat-ISC-style.txt` |
| SDL-webOS / SDL 2 | `lib/backports/libSDL2-2.0.so.0` | zlib plus embedded HIDAPI and yuv2rgb terms | `SDL2-zlib.txt` and `SDL2-THIRD-PARTY-NOTICES.txt` |
| inih r58 | `lib/libinih.so.1` | BSD-3-Clause | `inih-BSD-3-Clause.txt` |
| Mbed TLS 3.4.0 | `lib/libmbedcrypto.so.14`, `lib/libmbedx509.so.5` and `lib/libmbedtls.so.19` | Apache-2.0 | `Mbed-TLS-Apache-2.0.txt` |
| Opus 1.4 | `lib/libopus.so.0` | BSD-style terms plus patent notice | `Opus-COPYING.txt` and `Opus-AUTHORS.txt` |
| Google Material Icons | Embedded interface icon font | Apache-2.0 | `Material-Icons-Apache-2.0.txt` (licence revision/hash in `../SOURCE-MANIFEST.txt`) |
| SDL_GameControllerDB | Altered Linux mappings generated as `assets/gamecontrollerdb.txt` | zlib; altered lines are marked `platform:webOS` | `SDL_GameControllerDB-zlib.txt` |

System libraries resolved from webOS at runtime are not copied into the IPK.
They are identified in the project acknowledgements but are not presented here
as redistributed binaries.

## Release status

The packaging rules stage this index, the listed notices, `SOURCE-MANIFEST.txt`,
`BUILDINFO.txt` and final payload `SHA256SUMS`. The inih, Mbed TLS
and Opus rows are conditional: their texts are installed when those fallback
libraries are built and copied into the package. This notice set is not, by
itself, release clearance. Before publishing an IPK:

- verify this table against the exact archive and generate its SBOM;
- record immutable source revisions, local patches and complete corresponding
  source for every LGPL/GPL component;
- verify embedded font and generated icon/splash provenance, translations and
  the two extra controller mappings against the matching source archive;
- remove any build-only static archive from the package; and
- provide an immutable corresponding-source reference matching the binary.

The public release checklist remains authoritative when any item above is open.
