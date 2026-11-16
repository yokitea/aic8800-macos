# aic8800-macos

macOS kernel extensions, user-space firmware loader, WPA2-PSK supplicant daemon, and HeliPort-style menu bar app for **AICSemi AIC8800D80** (`0xa69c:0x5721` -> `0xa69c:0x8d80` -> `0x368b:0x8d81`) USB Wi-Fi 6 + Bluetooth 5.4 combo adapters on macOS Sequoia (`x86_64`).

> **Status (verified live booting directly from Internal SATA EFI on macOS Sequoia 15.8.2):**
> - **Stage 0 -> 1 -> 2 Bring-Up**: Cold boot, S3 sleep/wake recovery, and hotplug replug via `AIC8800MSCBlocker.kext` + `aic8800_fwload`.
> - **Wi-Fi 6 Data Path**: Dual-band (2.4 GHz & 5 GHz), TX/RX A-MPDU aggregation with zero-heap `8 TID x 64-slot` BlockAck reordering, WPA2-PSK 4-Way Handshake + GTK rekey + Open networks, auto-DHCP, and accurate BSD `netstat -I enX -b` counters (`Ipkts`/`Ibytes`/`Opkts`/`Obytes`).
> - **Menu Bar UI (`AIC8800Menu.app`)**: Lightweight `LSUIElement` Swift/AppKit menu bar app talking to `aic8800_wifi --daemon` over `/var/run/aic8800_wifi.sock` (`root:admin 0660`) with no daily `sudo` prompts.
> - **Bluetooth 5.4**: Active LE scan & ACL/GATT pairing/connection using `AIC8800BTInjector.kext` + `scripts/bluetoolfixup_aic8800.patch` (for Acidanthera `BlueToolFixup.kext`).

## Hardware Stages

| Stage | VID:PID | Role | Handled by |
| --- | --- | --- | --- |
| Stage 0 | `0xa69c:0x5721` | USB Mass Storage (virtual driver ISO) | `AIC8800MSCBlocker.kext` + `aic8800_fwload` SCSI eject CBW |
| Stage 1 | `0xa69c:0x8d80` | ROM Bootloader | `aic8800_fwload` USB bulk/control firmware upload + 5 GHz `user_cfg` |
| Stage 2 | `0x368b:0x8d81` | Runtime Wi-Fi (`If2`) + Bluetooth (`If0/If1`) | `AIC8800WiFi.kext` + `AIC8800BTInjector.kext` + patched `BlueToolFixup.kext` |

## Repository Layout

- `common/` — Shared POD wire ABI structs, TX/RX framing, LMAC message builders, zero-heap RX BlockAck reorder ring (`aic8800_reord.h`), WPA2-PSK crypto (`aic8800_wpa2.h`), and IOKit UserClient / fwload contracts.
- `kexts/AIC8800MSCBlocker/` — Codeless `IOService` blocker (`com.rush.driver.AIC8800MSCBlocker`) preventing `IOUSBMassStorageDriver` from mounting Stage 0.
- `kexts/AIC8800WiFi/` — Pure public-KPI `IOEthernetController` kext (`com.rush.driver.AIC8800WiFi`) with hot-unplug lifecycle, `AIC8800WiFiUserClient`, A-MPDU + RX BA reorder, and `ifnet_stat_increment_in`.
- `kexts/AIC8800BTInjector.kext/` — Codeless USB property injector (`com.rush.driver.AIC8800BTInjector`) naming Stage 2 Bluetooth HCI/Audio interfaces.
- `kexts/AIC8800BTQuirk/` — Experimental Lilu plugin skeleton (`com.rush.driver.AIC8800BTQuirk`).
- `daemon/aic8800_fwload/` & `daemon/com.rush.aic8800_fwload.plist` — `launchd` daemon (`com.rush.aic8800_fwload`) driving automatic Stage 0 -> 1 -> 2 transitions.
- `tools/aic8800_wifi.c` & `daemon/com.rush.aic8800_wifi.plist` — WPA2-PSK supplicant CLI, auto-connect daemon (`com.rush.aic8800_wifi`), and `/var/run/aic8800_wifi.sock` IPC control server.
- `apps/AIC8800Menu/` — Lightweight Swift/AppKit menu bar UI (`AIC8800Menu.app`).
- `scripts/` — Daemon install/uninstall (`install_daemon.sh`, `uninstall_daemon.sh`), health check (`verify_phase1.sh`), and `bluetoolfixup_aic8800.patch` (4-patch diff against `acidanthera/BrcmPatchRAM` for `bluetoothd` on Sequoia).
- `tests/` — Host ASan/UBSan + zero-heap verification suites (`test_abi_parity`, `test_phase0`, `test_phase1`, `test_phase2_bt`, `test_phase3_wifi`, `test_hotpath_noheap`, `test_fwload_daemon`).

## Prerequisites

- **macOS (`x86_64`)** + Xcode / macOS SDK if building from source.
- **External AIC8800D80 Firmware Binaries** (`fw_adid_8800d80_u02.bin`, `fw_patch_8800d80_u02.bin`, `fw_patch_8800d80_u02_ext0.bin`, `fmacfw_8800d80_u02.bin`, etc. — **not included** in this repository or releases), supplied via `FW_DIR=/path/to/fw/aic8800D80`.
- **For Bluetooth 5.4**: Acidanthera `Lilu.kext` + `BlueToolFixup.kext` built from `acidanthera/BrcmPatchRAM` with `scripts/bluetoolfixup_aic8800.patch` applied, plus OpenCore NVRAM `boot-args`:
  ```text
  -btlfxallowanyaddr -btlfxboardid -btlfxnvramcheck
  ```
  and NVRAM keys under `7C436110-AB2A-4BBB-A880-FE41995C9F82`: `bluetoothExternalDongleFailed=00`, `bluetoothHostControllerSwitchBehavior=always`, `bluetoothInternalControllerInfo=0000000000000000000000000000`.

## Build & Test

```sh
git clone --recurse-submodules https://github.com/yokitea/aic8800-macos.git
cd aic8800-macos
make clean all
make test FW_DIR=/path/to/fw/aic8800D80
```

## Installation (Source or Release Archive)

1. Copy `AIC8800MSCBlocker.kext`, `AIC8800WiFi.kext`, `AIC8800BTInjector.kext`, and patched `BlueToolFixup.kext` (after `Lilu.kext`) to your OpenCore `EFI/OC/Kexts/` and register them in `config.plist`.
2. Install the daemons, firmware files, CLI, and `/Applications/AIC8800Menu.app`:
   ```sh
   sudo FW_DIR=/path/to/fw/aic8800D80 ./scripts/install_daemon.sh
   ```
   *(Or `sudo make install-daemon FW_DIR=/path/to/fw/aic8800D80` when building from source.)*
3. Reboot so OpenCore loads the kexts, then launch the Menu Bar App:
   ```sh
   open /Applications/AIC8800Menu.app
   ```
   Click the Wi-Fi icon in the menu bar to scan, join networks, and optionally enable **Launch at Login**.

### CLI Usage (`aic8800_wifi`)

```sh
aic8800_wifi status                                   # reads via daemon socket (no sudo needed)
sudo aic8800_wifi scan                                # list visible/hidden BSSes
sudo aic8800_wifi connect "<SSID>" "<PASSWORD>"       # one-shot connect + DHCP
sudo aic8800_wifi save "<SSID>" "<PASSWORD>"          # save root-only 0600 wifi.conf profile
sudo aic8800_wifi disconnect
```

## Limitations

- Presented as a pure public-KPI `IOEthernetController` (`enX`), not Apple's private `IO80211Family`, so SSID selection uses `AIC8800Menu.app` or `aic8800_wifi` rather than macOS System Settings -> Wi-Fi.
- Supplicant supports **WPA2-PSK** and **Open** networks (WPA3-SAE and 802.1X Enterprise are not yet implemented).
- Built and verified on **macOS Sequoia 15.8.2 (`x86_64`)**; untested on Apple Silicon.

## Uninstall

```sh
sudo ./scripts/uninstall_daemon.sh
```
