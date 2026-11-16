#!/usr/bin/env bash
set -euo pipefail

if [[ "$(id -u)" -ne 0 ]]; then
    echo "error: must run as root (e.g. sudo $0)" >&2
    exit 1
fi

LABEL="com.rush.aic8800_fwload"
WIFI_LABEL="com.rush.aic8800_wifi"
PLIST_DST="/Library/LaunchDaemons/com.rush.aic8800_fwload.plist"
WIFI_PLIST_DST="/Library/LaunchDaemons/com.rush.aic8800_wifi.plist"
BIN_DST="/Library/PrivilegedHelperTools/aic8800_fwload"
WIFI_DST="/Library/PrivilegedHelperTools/aic8800_wifi"
MENU_DST="/Applications/AIC8800Menu.app"
FW_ROOT="/Library/Application Support/AIC8800"
LOCK_DST="/var/run/aic8800_fwload.lock"
SOCK_DST="/var/run/aic8800_wifi.sock"

launchctl bootout "system/${LABEL}" 2>/dev/null || true
launchctl bootout "system/${WIFI_LABEL}" 2>/dev/null || true
rm -f "$PLIST_DST" "$WIFI_PLIST_DST" "$BIN_DST" "$WIFI_DST" "$LOCK_DST" "$SOCK_DST" "/usr/local/bin/aic8800_wifi"
rm -rf "$FW_ROOT" "$MENU_DST"

echo "Uninstalled system/${LABEL} & system/${WIFI_LABEL} cleanly."

