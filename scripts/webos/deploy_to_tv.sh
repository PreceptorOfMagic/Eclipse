#!/usr/bin/env bash
# Build and deploy Eclipse to the LG TV using the resolved TV address.
# Usage: ./scripts/webos/deploy_to_tv.sh [--no-build]
#
# Without --no-build: builds the IPK then deploys it.
# With --no-build:    deploys the most recent IPK in dist/ without rebuilding.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

export PATH="$HOME/.local/bin:$PATH"
TV_DEVICE=LivingRoomMoonlight
. "${SCRIPT_DIR}/../lib/net_env.sh"
TV_HOST="$(resolve_tv)" || exit 1
TV_APP_ID=com.aurora.gamestream

SCP_OPTS="-P 9922 -i $HOME/.ssh/aurora_tv -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa -o StrictHostKeyChecking=no"
# NOTE: scp uses -P for the port, ssh uses lowercase -p. Keep these separate —
# building $SSH from $SCP_OPTS (with -P) silently sends ssh to port 22.
SSH_OPTS="-p 9922 -i $HOME/.ssh/aurora_tv -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa -o StrictHostKeyChecking=no"
SSH="ssh $SSH_OPTS prisoner@${TV_HOST}"

# ── Build ──────────────────────────────────────────────────────────────────
if [[ "${1}" != "--no-build" ]]; then
    echo "=== Building ==="
    cd "${PROJECT_ROOT}"
    ./scripts/webos/build_for_lg.sh
fi

# ── Find IPK ──────────────────────────────────────────────────────────────
IPK=$(ls -t "${PROJECT_ROOT}/dist/"*.ipk 2>/dev/null | head -1)
if [[ -z "${IPK}" ]]; then
    echo "ERROR: No IPK found in dist/. Run without --no-build first."
    exit 1
fi
IPK_APP_ID="$(python3 "${SCRIPT_DIR}/ipk_app_id.py" "$IPK")" || exit 1
if [ "$IPK_APP_ID" != "$TV_APP_ID" ]; then
    echo "ERROR: IPK contains $IPK_APP_ID; this deployment targets $TV_APP_ID. Refusing wrong-variant install." >&2
    exit 1
fi
echo ""
echo "=== Deploying $(basename ${IPK}) to ${TV_HOST} ==="

# ── Back up live conf to the LOCAL machine BEFORE install ─────────────────
# Ordinary ares upgrades preserve profile files (measured). This is best-effort
# backup only: native 0600 pairing keys may be unreadable to prisoner. Never
# restore a stale local snapshot or weaken key permissions after an install.
if CONF_OLD="$(resolve_tv_conf "$TV_APP_ID" $SSH)"; then
    :
else
    profile_status=$?
    [ "$profile_status" -eq 1 ] || exit "$profile_status"
    CONF_OLD=""
fi
umask 077
BACKUP_DIR="$HOME/.aurora-tv-conf-backup"
echo "--- Backing up TV conf -> ${BACKUP_DIR} ---"
if [ -n "$CONF_OLD" ] && $SSH "[ -f ${CONF_OLD}/hosts.ini ] || [ -d ${CONF_OLD}/key ]"; then
    rm -rf "${BACKUP_DIR}.new"; mkdir -p "${BACKUP_DIR}.new"
    if scp $SCP_OPTS -r "prisoner@${TV_HOST}:${CONF_OLD}/." "${BACKUP_DIR}.new/" >/dev/null 2>&1; then
        rm -rf "${BACKUP_DIR}"; mv "${BACKUP_DIR}.new" "${BACKUP_DIR}"
        echo "  backed up: $(ls "${BACKUP_DIR}" 2>/dev/null | tr '\n' ' ')"
    else
        echo "  WARN: backup INCOMPLETE (native 0600 keys may be unreadable); prior snapshot retained, never auto-restored"; rm -rf "${BACKUP_DIR}.new"
    fi
else
    echo "  no live conf on TV to back up (keeping existing backup)"
fi

# ── Install via ares-install ───────────────────────────────────────────────
# ares-install IGNORES TV_HOST and reads the DEVICE PROFILE, so resolving the TV above is
# not enough: a profile left on an old address makes scp/ssh here succeed while the install
# goes nowhere. Resync the profile to the address we just resolved, every time.
"${SCRIPT_DIR}/../tv/sync_ares_device.sh" "${TV_HOST}" 2>&1 | sed 's/^/    /'

echo "--- Installing (ares-install) ---"
cd "${PROJECT_ROOT}"
ares-install "${IPK}" -d "${TV_DEVICE}"

# Existing TV profiles remain authoritative. A prior local backup is never
# pushed automatically: it may be stale or incomplete, especially with 0600 keys.
echo "--- Existing TV profile preserved; no automatic restore or permission changes ---"

# ── Pull previous diagnostic log ──────────────────────────────────────────
echo "--- Pulling diagnostic log ---"
scp $SCP_OPTS "prisoner@${TV_HOST}:/tmp/aurora-diag.log" "${PROJECT_ROOT}/aurora-diag.log" 2>/dev/null \
    && echo "Log saved to aurora-diag.log" \
    || echo "(No log — TV may have been restarted)"

echo ""
echo "=== Done. Launch Eclipse on the TV to start streaming. ==="
