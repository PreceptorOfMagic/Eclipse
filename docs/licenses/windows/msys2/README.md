# Pinned supplemental MSYS2 notices

These files supplement eleven Windows runtime packages that do not install their
own full licence/copyright files. They are copied verbatim from each exact
upstream source archive, including upstream authors, patent grants and bundled
third-party notices where supplied. `manifest.json` binds the files to exact
MSYS2 package versions and SHA-256 digests.

For every package, the pinned `PKGBUILD` hash matches `pkgbuild_sha256sum` in the
cached binary package's `.BUILDINFO`. The upstream archive hash matches that
recipe. x264 uses a pinned Git commit: `git -c core.abbrev=no archive --format tar
b35605ace3ddf7c1a5d67a2eb553f034aef41d55` reproduces the recipe's checksum; its
separately downloaded immutable GitLab archive has its own recorded SHA-256.
All recipe-owned patches and source files are retained under `provenance/`,
with hashes checked against the exact recipe. Binary hashes allow an auditor
to retrieve and check the same `.BUILDINFO` independently.

The CA bundle's original Debian copyright file contains the complete MPL 2.0
and the Mozilla/Debian copyright notices. It references a separate full GPL 2.0
text. `GPL-2.0.txt` supplies the exact GPL 2.0 text from the verified FFmpeg source
archive; the manifest explicitly records that additional origin.

Packaging must match the installed package version exactly, verify every notice
hash before copying, and fail if a runtime package has neither installed full
licence files nor a verified supplemental entry. Metadata alone is insufficient.
Unknown/new versions require a fresh source audit rather than silently reusing
these files. Copy the manifest and provenance alongside the notices.

This directory is a notice supplement and verified source-input map, not the
complete external corresponding-source component. The referenced full archives,
all other shipped dependency sources and build toolchain/source instructions are
separate release inputs. Eclipse's project-source archive alone does not cover
those external dependencies.
