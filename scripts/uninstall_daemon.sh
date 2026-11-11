#!/usr/bin/env bash
set -euo pipefail

if [[ "$(id -u)" -ne 0 ]]; then
    echo "error: must run as root (e.g. sudo $0)" >&2
    exit 1
fi

LABEL="com.rush.aic8800_fwload"
PLIST_DST="/Library/LaunchDaemons/com.rush.aic8800_fwload.plist"
BIN_DST="/Library/PrivilegedHelperTools/aic8800_fwload"
FW_ROOT="/Library/Application Support/AIC8800"
LOCK_DST="/var/run/aic8800_fwload.lock"

launchctl bootout "system/${LABEL}" 2>/dev/null || true
rm -f "$PLIST_DST" "$BIN_DST" "$LOCK_DST"
rm -rf "$FW_ROOT"

echo "Uninstalled system/${LABEL} cleanly."

