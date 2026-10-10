#!/usr/bin/env bash
# Pull the diagnostic log from the TV.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
. "${SCRIPT_DIR}/../lib/net_env.sh"
TV_HOST="$(resolve_tv)" || exit 1   # env TV_HOST -> ares profile -> cache -> scan

scp -P 9922 -i ~/.ssh/aurora_tv \
    -o HostKeyAlgorithms=+ssh-rsa \
    -o PubkeyAcceptedAlgorithms=+ssh-rsa \
    -o StrictHostKeyChecking=no \
    "prisoner@${TV_HOST}:/tmp/aurora-diag.log" \
    "${PROJECT_ROOT}/aurora-diag.log" && echo "Saved to aurora-diag.log"
