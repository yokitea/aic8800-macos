#!/usr/bin/env bash
set -euo pipefail

LABEL="system/com.rush.aic8800_fwload"
LOG_FILE="/var/log/aic8800_fwload.log"

echo "=== AIC8800 Phase 1 Empirical Health Snapshot ($(date '+%Y-%m-%d %H:%M:%S')) ==="

echo "[1] Loaded Kexts:"
kextstat | grep -i AIC8800 || echo "  (none loaded)"

echo "[2] USB Topology (Stage 0 / 1 / 2):"
ioreg -p IOUSB -w0 | grep -E "AIC|0x8d80|0x8d81|0x572" || true
stage0=$(ioreg -p IOUSB -l -w0 | grep -c '"idProduct" = 22305' || true)
stage1=$(ioreg -p IOUSB -l -w0 | grep -c '"idProduct" = 36224' || true)
stage2=$(ioreg -p IOUSB -l -w0 | grep -c '"idProduct" = 36225' || true)
echo "  counters: stage0=${stage0} stage1=${stage1} stage2=${stage2}"

echo "[3] DiskArbitration Stage 0 Mount Check:"
mounted_msc=$(diskutil list | grep -c "AIC" || true)
mounted_noname=$(ls -1 /Volumes 2>/dev/null | grep -c "^NO NAME" || true)
echo "  diskarbitration_mount_no_name=${mounted_noname} diskutil_aic_hits=${mounted_msc}"

echo "[4] Network Interface Attachment:"
wifi_ioreg=$(ioreg -r -c AIC8800WiFi -d 3 -w0 || true)
echo "$wifi_ioreg" | grep -E "AIC8800WiFi|IOEthernetInterface|en[0-9]+" || true
wifi_active=$(printf "%s\n" "$wifi_ioreg" | grep -cE "<class .*AIC8800WiFi.* active, busy 0 " || true)
wifi_hung=$(printf "%s\n" "$wifi_ioreg" | grep -cE "AIC8800WiFi.*(!registered|!matched|inactive|busy [1-9])" || true)
usb_owners=$(ioreg -p IOService -l -w0 | grep -c '"UsbExclusiveOwner" = "AIC8800WiFi"' || true)
wifi_objs=$(ioreg -l -w0 | grep -o '"AIC8800WiFi"=[0-9]*' | head -n 1 || echo "AIC8800WiFi=?")
echo "  counters: wifi_active=${wifi_active} wifi_hung=${wifi_hung} usb_owners=${usb_owners} ${wifi_objs}"
for iface in $(ifconfig -l); do
    if ifconfig "$iface" 2>/dev/null | grep -q "90:de:80:10:4b:09"; then
        echo "  attached_iface=${iface}"
        ifconfig "$iface" | sed 's/^/    /'
    fi
done

echo "[5] LaunchDaemon Status (${LABEL}):"
if launchctl print "$LABEL" >/dev/null 2>&1; then
    ld_info=$(launchctl print "$LABEL" || true)
    printf "%s\n" "$ld_info" | grep -E "state =|pid =|last exit code =|runs =" || true
    ld_pid=$(printf "%s\n" "$ld_info" | awk '/^[[:space:]]*pid = /{print $3; exit}')
    if [[ -n "$ld_pid" ]] && ps -p "$ld_pid" >/dev/null 2>&1; then
        ps -o pid,rss,vsz,etime -p "$ld_pid" | sed 's/^/  /'
    fi
else
    echo "  LaunchDaemon not installed yet (run: make install-daemon)"
fi

echo "[6] Kernel Panic Logs Check:"
panics=$( (shopt -s nullglob nocaseglob; set -- /Library/Logs/DiagnosticReports/*panic*; echo $#) )
echo "  diagnostic_panic_files=${panics}"

if [[ -r "$LOG_FILE" ]]; then
    echo "[7] Recent ${LOG_FILE} (last 15 lines):"
    tail -n 15 "$LOG_FILE" | sed 's/^/  /'
fi
