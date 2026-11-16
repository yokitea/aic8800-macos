#!/usr/bin/env bash
set -euo pipefail

if [[ "$(id -u)" -ne 0 ]]; then
    echo "error: must run as root (e.g. sudo $0)" >&2
    exit 1
fi

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FW_SRC_DIR="${FW_DIR:-/Users/rush/.gemini/antigravity/brain/3af3af06-ee65-4085-960c-f78f553e3493/scratch/aic8800d80/fw/aic8800D80}"
BIN_SRC="${REPO_DIR}/build/aic8800_fwload"
WIFI_SRC="${REPO_DIR}/build/aic8800_wifi"
MENU_SRC="${REPO_DIR}/build/AIC8800Menu.app"
PLIST_SRC="${REPO_DIR}/daemon/com.rush.aic8800_fwload.plist"
WIFI_PLIST_SRC="${REPO_DIR}/daemon/com.rush.aic8800_wifi.plist"

BIN_DST="/Library/PrivilegedHelperTools/aic8800_fwload"
WIFI_DST="/Library/PrivilegedHelperTools/aic8800_wifi"
MENU_DST="/Applications/AIC8800Menu.app"
FW_DST="/Library/Application Support/AIC8800/fw/aic8800D80"
PLIST_DST="/Library/LaunchDaemons/com.rush.aic8800_fwload.plist"
WIFI_PLIST_DST="/Library/LaunchDaemons/com.rush.aic8800_wifi.plist"
LOG_DST="/var/log/aic8800_fwload.log"
WIFI_LOG_DST="/var/log/aic8800_wifi.log"
LABEL="com.rush.aic8800_fwload"
WIFI_LABEL="com.rush.aic8800_wifi"

if [[ ! -x "$BIN_SRC" || ! -x "$WIFI_SRC" ]]; then
    echo "error: missing built binaries (run make first)" >&2
    exit 1
fi
if [[ ! -r "${FW_SRC_DIR}/fw_patch_table_8800d80_u02.bin" ]]; then
    echo "error: missing firmware files in '$FW_SRC_DIR'" >&2
    exit 1
fi
plutil -lint "$PLIST_SRC"
plutil -lint "$WIFI_PLIST_SRC"

launchctl bootout "system/${LABEL}" 2>/dev/null || true
launchctl bootout "system/${WIFI_LABEL}" 2>/dev/null || true

install -d -m 0755 -o root -g wheel "/Library/PrivilegedHelperTools"
install -d -m 0755 -o root -g wheel "/Library/Application Support/AIC8800"
install -d -m 0755 -o root -g wheel "/Library/Application Support/AIC8800/fw"
install -d -m 0755 -o root -g wheel "$FW_DST"

install -m 0755 -o root -g wheel "$BIN_SRC" "$BIN_DST"
install -m 0755 -o root -g wheel "$WIFI_SRC" "$WIFI_DST"
ln -sf "$WIFI_DST" "/usr/local/bin/aic8800_wifi" 2>/dev/null || true
if [[ -d "$MENU_SRC" ]]; then
    rm -rf "$MENU_DST"
    ditto "$MENU_SRC" "$MENU_DST"
    chmod -R a+rX "$MENU_DST"
fi
cp -f "${FW_SRC_DIR}/"*.bin "$FW_DST/"
chown root:wheel "${FW_DST}/"*.bin
chmod 0644 "${FW_DST}/"*.bin

install -m 0644 -o root -g wheel "$PLIST_SRC" "$PLIST_DST"
install -m 0644 -o root -g wheel "$WIFI_PLIST_SRC" "$WIFI_PLIST_DST"
plutil -lint "$PLIST_DST"
plutil -lint "$WIFI_PLIST_DST"

touch "$LOG_DST" "$WIFI_LOG_DST"
chown root:wheel "$LOG_DST" "$WIFI_LOG_DST"
chmod 0644 "$LOG_DST" "$WIFI_LOG_DST"

launchctl bootstrap system "$PLIST_DST"
launchctl kickstart -k "system/${LABEL}"
launchctl bootstrap system "$WIFI_PLIST_DST"
launchctl kickstart -k "system/${WIFI_LABEL}"

echo "Installed and started system/${LABEL} & system/${WIFI_LABEL}:"
echo "  binaries: $BIN_DST, $WIFI_DST"
if [[ -d "$MENU_DST" ]]; then
    echo "  menu app: $MENU_DST"
fi
echo "  firmware: $FW_DST ($(ls -1 "${FW_DST}"/*.bin | wc -l | tr -d ' ') files)"
echo "  plists:   $PLIST_DST, $WIFI_PLIST_DST"
echo "  logs:     $LOG_DST, $WIFI_LOG_DST"

