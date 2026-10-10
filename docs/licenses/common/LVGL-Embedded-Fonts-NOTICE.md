# Embedded LVGL font notice

The client compiles generated glyph arrays from the official LVGL v8.3.0
source tree. The LVGL MIT licence covers LVGL's code; the underlying font and
icon-font material has the separate terms recorded here.

## Enabled arrays

| Compiled source | Upstream material | SHA-256 |
|---|---|---|
| `third_party/lvgl/src/font/lv_font_montserrat_22.c` | Montserrat 7.200 text glyphs plus Font Awesome Free 5.9.0 symbols | `1389038eacafd11e2675442c9222164a585ead72f24caa87b8eaeba464bbd5fc` |
| `third_party/lvgl/src/font/lv_font_montserrat_48.c` | Montserrat 7.200 text glyphs plus Font Awesome Free 5.9.0 symbols | `d0611a239c482f48d024dfa3436606a7c1d0a3da2f5bd1b864a1f70d76c59e1a` |
| `third_party/lvgl/src/font/lv_font_unscii_8.c` | Unscii-8 1.0 glyphs | `0f5a9b6131ffed26f42099ced657cc3f619ea66e2f958ca54673f97e0e71cf0e` |

These hashes match the files in the official
[LVGL v8.3.0 tag](https://github.com/lvgl/lvgl/tree/v8.3.0/src/font).

## Source fonts retained with corresponding source

| Source asset | SHA-256 | Terms shipped beside this notice |
|---|---|---|
| `Montserrat-Medium.ttf` | `421f26b23e2be6b98373d32acd3cb2897b154d4bf0a77d26534ce476e4cbed53` | Copyright 2011 The Montserrat Project Authors; SIL OFL 1.1 in `Montserrat-7.200-OFL-1.1.txt` |
| `FontAwesome5-Solid+Brands+Regular.woff` | `f4e42f6cd69e5dbdcccc0f2f5be136cebde0e427641e45403bf9173a92da95f4` | Copyright (c) Font Awesome; font glyphs under the complete `SIL-OFL-1.1.txt` terms and the upstream summary/brand warning in `Font-Awesome-Free-5.9.0-LICENSE.txt` |
| `unscii-8.ttf` | `1be7ca5552beb67f6bf1bed4a0db92a53835f1c07cb0f643063d82ffb0dccba0` | Unscii-8 by Ville-Matias “Viznut” Heikkilä; pinned public-domain/CC0 statement and exception analysis in `Unscii-8-1.0-PROVENANCE.txt` |

The source-font hashes also match the corresponding files in the official
[LVGL v8.3.0 built-in-font directory](https://github.com/lvgl/lvgl/tree/v8.3.0/scripts/built_in_font).

Font Awesome's brand glyphs remain trade marks of their respective owners.
Their presence does not imply endorsement. Use a brand glyph only to represent
the company, product or service to which it refers; the complete upstream
warning is retained in the packaged Font Awesome licence file.
