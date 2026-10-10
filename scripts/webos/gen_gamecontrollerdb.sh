#!/bin/sh

set -eu

# Keep this aligned with the audited Windows package input. A mutable `master`
# URL made two packages built on different days silently contain different
# controller mappings and provided no way to prove which licence/source applied.
GCDB_REVISION='5a12daa568d19344f9b6e9286ef5929833b25c7c'
GCDB_SHA256='07ec5b753e685c4829987919b27905cc3c45c8e18c11b3046e275fcf38b0f3cb'
gcdb_tmp=$(mktemp /tmp/aurora-gcdb.XXXXXX)

cleanup() {
  rm -f "$gcdb_tmp"
}
trap cleanup EXIT HUP INT TERM

curl --fail --location --silent --show-error \
  "https://raw.githubusercontent.com/gabomdq/SDL_GameControllerDB/${GCDB_REVISION}/gamecontrollerdb.txt" \
  --output "$gcdb_tmp"
printf '%s  %s\n' "$GCDB_SHA256" "$gcdb_tmp" | sha256sum --check --status

printf '# Generated from https://github.com/gabomdq/SDL_GameControllerDB at commit %s\n' "$GCDB_REVISION"
printf '# Linux mappings below are altered only by changing platform:Linux to platform:webOS.\n\n'
sed -n 's/^\(.*\),platform:Linux,$/\1,platform:webOS,/p' "$gcdb_tmp"
