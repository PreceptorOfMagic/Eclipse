#!/usr/bin/env bash
# sync_ares_device.sh — point the ares device profiles at wherever the TV actually is.
#
# WHY: `ares-install` and `ares-launch` IGNORE TV_HOST and read the device profile instead,
# so a TV that has moved leaves deploys and launches going to a dead address while every
# script that resolves at run time works fine. That split is the confusing part: the same
# session can pull logs successfully over ssh and silently install nothing.
#
# This is backlog item 20 spot (b). The TV is DHCP and the user declined reservations, so
# it moves on every power cycle -- .21 -> .25 -> .2 observed so far. Run this after any
# reshuffle, or just run it before deploying; it is idempotent and costs one lookup.
set -uo pipefail

. "$(cd "$(dirname "${BASH_SOURCE[0]}")/../lib" && pwd)/net_env.sh"

IP="${1:-${TV_HOST:-$(resolve_tv)}}"
if [ -z "$IP" ]; then
  echo "sync_ares_device: cannot resolve the TV. Pass an IP, or set TV_HOST." >&2
  echo "  (a powered-off TV cannot be found at all -- turning it on needs the remote)" >&2
  exit 1
fi

# Both names matter: `tv` is what the scripts pass to -d, and `LivingRoomMoonlight` is the
# profile the user's own tooling uses. Leaving one stale is the same bug half-fixed.
rc=0
for name in ${ARES_DEVICES:-tv LivingRoomMoonlight}; do
  before="$(ares-setup-device -F 2>/dev/null \
            | python3 -c "
import json,sys
try: d=json.load(sys.stdin)
except Exception: sys.exit()
for x in d:
    if x.get('name')=='$name':
        print(x.get('deviceinfo',{}).get('ip',''))
" 2>/dev/null)"
  if [ "$before" = "$IP" ]; then
    printf '  %-20s already %s\n' "$name" "$IP"
    continue
  fi
  if ares-setup-device -m "$name" -i "host=$IP" >/dev/null 2>&1; then
    printf '  %-20s %s -> %s\n' "$name" "${before:-<unset>}" "$IP"
  else
    printf '  %-20s FAILED to update (profile missing?)\n' "$name"
    rc=1
  fi
done

# Prove it took, rather than trusting the exit code: ares has reported success while
# leaving the profile untouched.
echo "== profiles now:"
ares-setup-device -F 2>/dev/null | python3 -c "
import json,sys
try: d=json.load(sys.stdin)
except Exception: sys.exit()
for x in d:
    n=x.get('name'); ip=x.get('deviceinfo',{}).get('ip','')
    if n in ('tv','LivingRoomMoonlight'): print(f'  {n:<20} {ip}')
"
exit $rc
