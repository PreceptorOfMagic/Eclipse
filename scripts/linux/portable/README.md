# Portable Linux build (x86_64)

Builds `dist/eclipse-linux-x86_64.tar.gz`: one folder that runs on any x86_64 desktop with glibc 2.35 or
newer (Ubuntu 22.04+, Debian 12+, Fedora 36+, Arch, SteamOS 3.5+), unpacked anywhere, no installation. The
Linux twin of `scripts/windows/build_portable.ps1`.

## Steps

```sh
scripts/linux/portable/make_sysroot.sh    # once: Ubuntu 22.04 sysroot from sysroot-jammy.lock (225 .debs, SHA-256)
scripts/linux/portable/build_deps.sh      # once: pinned dependencies from verified source into the sysroot
scripts/linux/build_portable.sh           # the client + package + audits (--clean, --no-archive, --require-clean-tree)
```

Work tree: `$ECLIPSE_LINUX_WORK` (default `~/eclipse-linux`). Builder: any x86_64 Linux with `gcc`, `cmake`,
`make`, `python3`, `git`, `curl`, `dpkg-deb`, and `pkg-config`, `nasm`, `patchelf`, `msgfmt`, `wayland-scanner`
(installed, or unpacked rootless under `$ECLIPSE_LINUX_WORK/hosttools/root` -- see `env.sh`).

## Why it is built this way

- **Linked against Ubuntu 22.04, not the builder.** A native gcc with `--sysroot` still searches its own
  library directory first, so `env.sh` puts the sysroot first with `-B`/`-L`/`-rpath-link`; otherwise the host
  glibc leaks in (SDL2 once needed GLIBC_2.43). `build_portable.sh` fails any binary above GLIBC_2.35.
- **Dependencies from pinned source** (`build_deps.sh`): the same versions as the Windows package. FFmpeg is
  decode-only (H.264/HEVC) with VA-API and NVDEC; SDL2 loads X11/Wayland/PulseAudio/PipeWire/ALSA at run time.
- **One shared, thread-safe mbedTLS.** Statically linked into three libraries, ELF symbol interposition mixed
  the copies inside one TLS handshake and crashed (ASan: SEGV in `mbedtls_sha512_finish`); the default config
  also lacks `MBEDTLS_THREADING_C` while hosts are polled concurrently. Windows never saw it: DLL imports do
  not interpose.
- **curl patch** `patches/curl-8.22.0-mbedtls-pem-key-blob.patch`: curl's mbedTLS backend rejected every
  in-memory PEM client key.
- **Relocatable package.** RUNPATH `$ORIGIN/../lib` (executable) and `$ORIGIN` (libraries). The SS4S modules
  are `dlopen()`ed by name through the executable's RUNPATH. The `eclipse` launcher only points the client
  at the folder: translations (`TEXTDOMAINDIR`), window class `eclipse` for the menu entry, libva/libdrm
  fallbacks used one by one only when the system lacks them, a log file when started from the menu.
- **Not bundled:** glibc, fontconfig/freetype/expat/libuuid, X11/Wayland/GL, PulseAudio/ALSA -- they must
  match the system's configuration and drivers. The launcher names any missing one.

## Audits in build_portable.sh

NEEDED closure (packaged, declared system library, or declared fallback -- nothing else), `$ORIGIN`
RUNPATHs, glibc floor, no builder paths in any binary this build produced, every module declared in
`ss4s_modules.ini` packaged, and every ELF loaded by the Ubuntu 22.04 `ld.so` against the sysroot's glibc.

## Diagnosis switches (also in the package README)

`SDL_VIDEODRIVER=x11|wayland`, `ECLIPSE_DIAGNOSTIC_VIDEO_DECODER=software|nvdec|vaapi`. For memory errors,
configure a separate build with `-fsanitize=address` and `-Wl,--allow-shlib-undefined` (the ASan runtime is
the builder's, newer than the sysroot's glibc -- diagnosis only, never packaged).
