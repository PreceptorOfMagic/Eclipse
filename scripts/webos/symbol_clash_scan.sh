#!/usr/bin/env bash
# Does a library Eclipse SHIPS define a symbol that a TV library in the same process also defines?
#
#   scripts/webos/symbol_clash_scan.sh <app-dir> [crash-log]
#
# That is the webOS 26 stream-start failure class (docs/webos26-media-load.md): the first
# definition in the process's global scope wins for every library that reaches the name through
# its PLT, so a bundled `foo` silently replaces the TV's `foo` -- harmless until some path calls it
# (for g_log: GStreamer's plugin rescan on an app's first media load after a firmware update or
# a fresh install). The executable is
# guarded at build time (cmake/CheckExecutableExports.cmake). This covers the bundled libraries the
# executable links directly (they share the global scope; ss4s modules are dlopen()ed RTLD_LOCAL
# and cannot interpose), and it has to run against the TV because the firmware is the variable.
# RUN IT AFTER EVERY FIRMWARE UPDATE.
#
#   <app-dir>    unpacked app tree: bin/aurora, lib/ (e.g. extract the ipk's data.tar.gz, or
#                dist/_CPack_Packages/.../usr/palm/applications/<id>).
#   [crash-log]  /tmp/aurora-crash.log from a -DAURORA_VENDOR_DIAG=ON build. Its `maps:` block lists
#                every object actually mapped, dlopen()ed media libraries included; the diag build
#                writes one only on a fatal signal or a hang. Without it the TV side is the NEEDED
#                closure of the app's objects, which misses dlopen()ed libraries (NDL impl, ...).
#                prisoner cannot read the app's /proc/PID/maps (hidepid).
# Both modes also add every /usr/lib/gstreamer-1.0 plugin: they load into the app's process, and
# the only maps snapshot we have (taken at the webOS 26 fault) predates them.
# Exit 0 = no clash, 1 = clash (listed), 2 = setup error, INCLUDING any TV library that could not be
# pulled -- an unpulled library has no definitions and would otherwise read as "no clash".
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
APPDIR="${1:?usage: $0 <app-dir> [crash-log]}"
MAPS="${2:-}"
[ -x "$APPDIR/bin/aurora" ] || { echo "no $APPDIR/bin/aurora" >&2; exit 2; }
. "$ROOT/scripts/lib/net_env.sh"
TV="$(resolve_tv)" || { echo "cannot resolve the TV" >&2; exit 2; }
TVSSH=(ssh -p 9922 -i "$HOME/.ssh/aurora_tv" -o HostKeyAlgorithms=+ssh-rsa
       -o PubkeyAcceptedAlgorithms=+ssh-rsa -o StrictHostKeyChecking=no -o BatchMode=yes
       -o ConnectTimeout=8 "prisoner@${TV}")
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/tv"

# Global-scope exports of an object: defined, not LOCAL, version suffix stripped.
defs() { readelf --dyn-syms -W "$1" 2>/dev/null |
         awk '$1 ~ /^[0-9]+:$/ && $7 != "UND" && $5 != "LOCAL" && $8 != "" {sub(/@.*/, "", $8); print $8}'; }
needed() { readelf -d "$1" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'; }
pull() {   # stdin: absolute TV paths -> $WORK/tv/<path>
  "${TVSSH[@]}" 'tar cf - $(cat) 2>/dev/null' | tar xf - -C "$WORK/tv" 2>/dev/null || true
}

# Eclipse's side: the executable's direct NEEDED that the app itself ships. lib/backports is on the
# RPATH, so it wins even where the TV has the soname. Plain lib/ is searched AFTER the system dirs
# (measured on 11.2.0: libopus.so.0 and libcurl.so.4 are bundled AND mapped from /usr/lib), so a
# lib/ copy only loads where the TV lacks that soname.
: > "$WORK/ours"; : > "$WORK/libcand"
for n in $(needed "$APPDIR/bin/aurora"); do
  if [ -f "$APPDIR/lib/backports/$n" ]; then echo "$APPDIR/lib/backports/$n" >> "$WORK/ours"
  elif [ -f "$APPDIR/lib/$n" ]; then echo "$n" >> "$WORK/libcand"; fi
done
"${TVSSH[@]}" 'while read n; do [ -e "/usr/lib/$n" ] || [ -e "/lib/$n" ] || echo "$n"; done' \
    < "$WORK/libcand" 2>/dev/null | sed "s#^#$APPDIR/lib/#" >> "$WORK/ours"

# TV side.
"${TVSSH[@]}" 'ls /usr/lib/gstreamer-1.0/*.so' > "$WORK/plugins" 2>/dev/null || true
[ -s "$WORK/plugins" ] || { echo "cannot list /usr/lib/gstreamer-1.0 on the TV $TV" >&2; exit 2; }
if [ -n "$MAPS" ]; then
  grep -oE ' /(usr/)?lib[^ ]*\.so[^ ]*' "$MAPS" | sed 's/^ //' | sort -u > "$WORK/tvlist"
  cat "$WORK/plugins" >> "$WORK/tvlist"
  src="maps block of $MAPS + GStreamer plugins"
else
  # NEEDED closure, resolved on the TV the way the loader does for these (/usr/lib, then /lib).
  needed "$APPDIR/bin/aurora" > "$WORK/queue"
  for f in $(cat "$WORK/ours") "$APPDIR"/lib/ss4s*.so; do needed "$f"; done >> "$WORK/queue"
  # A soname Eclipse provides is REPLACED, not co-resident: the TV's libSDL2-2.0.so.0 never loads
  # beside lib/backports' one. (TV libraries then bind to Eclipse's copy -- a different risk,
  # listed in docs/webos26-media-load.md.) Mark those seen so the closure does not pull them.
  : > "$WORK/tvlist"; sed 's#.*/##' "$WORK/ours" > "$WORK/seen"
  pull < "$WORK/plugins"; cat "$WORK/plugins" >> "$WORK/tvlist"
  while read -r p; do needed "$WORK/tv$p"; done < "$WORK/plugins" >> "$WORK/queue"
  while [ -s "$WORK/queue" ]; do
    sort -u "$WORK/queue" | grep -vxF -f "$WORK/seen" > "$WORK/new" || true
    cat "$WORK/new" >> "$WORK/seen"; : > "$WORK/queue"
    [ -s "$WORK/new" ] || break
    "${TVSSH[@]}" 'while read n; do for d in /usr/lib /lib; do
        [ -e "$d/$n" ] && { readlink -f "$d/$n"; break; }; done; done' < "$WORK/new" > "$WORK/found" 2>/dev/null || true
    pull < "$WORK/found"; cat "$WORK/found" >> "$WORK/tvlist"
    while read -r p; do needed "$WORK/tv$p"; done < "$WORK/found" >> "$WORK/queue"
  done
  src="NEEDED closure + GStreamer plugins (other dlopen()ed libraries NOT covered -- pass a crash log)"
fi
grep -v '^/media/developer' "$WORK/tvlist" | sort -u > "$WORK/tvsys"
[ -n "$MAPS" ] && pull < "$WORK/tvsys"
# An unpulled library contributes no definitions, so it would read as "no clash": refuse instead.
while read -r p; do [ -f "$WORK/tv$p" ] || echo "$p"; done < "$WORK/tvsys" > "$WORK/missing"
if [ -s "$WORK/missing" ]; then
  echo "could not pull $(wc -l < "$WORK/missing") of $(wc -l < "$WORK/tvsys") TV libraries, e.g. $(head -1 "$WORK/missing")" >&2
  exit 2
fi
while read -r p; do defs "$WORK/tv$p" | sed "s#\$# $p#"; done < "$WORK/tvsys" | sort -u > "$WORK/tvdefs"
[ -s "$WORK/tvdefs" ] || { echo "no TV definitions read -- nothing to compare against" >&2; exit 2; }

echo "TV $TV: $(wc -l < "$WORK/tvsys") TV libraries from the $src"
echo "Eclipse global-scope libraries: $(sed "s#^$APPDIR/##" "$WORK/ours" | tr '\n' ' ')"
rc=0
while read -r lib; do
  defs "$lib" | sort -u > "$WORK/d"
  hits="$(awk 'NR==FNR {a[$1]; next} ($1 in a)' "$WORK/d" "$WORK/tvdefs")"
  if [ -n "$hits" ]; then
    rc=1; echo "CLASH  ${lib#$APPDIR/}  ($(wc -l < "$WORK/d") exports):"
    echo "$hits" | sed 's/^/         /' | head -40
  else
    echo "ok     ${lib#$APPDIR/}  ($(wc -l < "$WORK/d") exports, none defined by a TV library)"
  fi
done < "$WORK/ours"
exit "$rc"
