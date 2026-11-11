#!/usr/bin/env bash
set -euo pipefail

if [[ "$(id -u)" -ne 0 ]]; then
    echo "error: must run as root (e.g. sudo $0)" >&2
    exit 1
fi

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FW_SRC_DIR="${FW_DIR:-/Users/rush/.gemini/antigravity/brain/3af3af06-ee65-4085-960c-f78f553e3493/scratch/aic8800d80/fw/aic8800D80}"
BIN_SRC="${REPO_DIR}/build/aic8800_fwload"
PLIST_SRC="${REPO_DIR}/daemon/com.rush.aic8800_fwload.plist"

BIN_DST="/Library/PrivilegedHelperTools/aic8800_fwload"
FW_DST="/Library/Application Support/AIC8800/fw/aic8800D80"
PLIST_DST="/Library/LaunchDaemons/com.rush.aic8800_fwload.plist"
LOG_DST="/var/log/aic8800_fwload.log"
LABEL="com.rush.aic8800_fwload"

if [[ ! -x "$BIN_SRC" ]]; then
    echo "error: missing built binary '$BIN_SRC' (run make first)" >&2
    exit 1
fi
if [[ ! -r "${FW_SRC_DIR}/fw_patch_table_8800d80_u02.bin" ]]; then
    echo "error: missing firmware files in '$FW_SRC_DIR'" >&2
    exit 1
fi
plutil -lint "$PLIST_SRC"

launchctl bootout "system/${LABEL}" 2>/dev/null || true

install -d -m 0755 -o root -g wheel "/Library/PrivilegedHelperTools"
install -d -m 0755 -o root -g wheel "/Library/Application Support/AIC8800"
install -d -m 0755 -o root -g wheel "/Library/Application Support/AIC8800/fw"
install -d -m 0755 -o root -g wheel "$FW_DST"

install -m 0755 -o root -g wheel "$BIN_SRC" "$BIN_DST"
cp -f "${FW_SRC_DIR}/"*.bin "$FW_DST/"
chown root:wheel "${FW_DST}/"*.bin
chmod 0644 "${FW_DST}/"*.bin

install -m 0644 -o root -g wheel "$PLIST_SRC" "$PLIST_DST"
plutil -lint "$PLIST_DST"

touch "$LOG_DST"
chown root:wheel "$LOG_DST"
chmod 0644 "$LOG_DST"

launchctl bootstrap system "$PLIST_DST"
launchctl kickstart -k "system/${LABEL}"

echo "Installed and started system/${LABEL}:"
echo "  binary:   $BIN_DST"
echo "  firmware: $FW_DST ($(ls -1 "${FW_DST}"/*.bin | wc -l | tr -d ' ') files)"
echo "  plist:    $PLIST_DST"
echo "  log:      $LOG_DST"

