# aic8800-macos

macOS kernel extension and user-space firmware loader workspace for **AICSemi AIC8800D80** USB Wi-Fi 6 adapters.

> **Project status:** **Phase 1 complete & empirically verified** (cold boot, S3 sleep/wake, hotplug replug, BSD Ethernet interface attachment, 0 kernel panics). **Phase 2 (Bluetooth 5.4 bring-up via AIC8800BTQuirk Lilu plugin)** implemented and verified in test suite. Full 802.11 / data-path TX/RX and `IO80211Family` integration belong to Phase 3.

## Hardware Stages

The AIC8800D80 enumerates across three USB PIDs during initialization:

| Stage | VID:PID | Role | Handled by |
| --- | --- | --- | --- |
| Stage 0 | `0xa69c:0x5721` | USB Mass Storage (driver ISO) | `AIC8800MSCBlocker.kext` + `aic8800_fwload` SCSI eject CBW |
| Stage 1 | `0xa69c:0x8d80` | ROM Bootloader | `aic8800_fwload` USB bulk/control firmware upload |
| Stage 2 | `0x368b:0x8d81` | Runtime Wi-Fi device | `AIC8800WiFi.kext` |

## Repository Layout

- `common/` — Shared C/C++ ABI structs, framing constants, LMAC definitions, and fwload control definitions.
- `kexts/AIC8800MSCBlocker/` — Codeless `IOService` blocker (`com.rush.driver.AIC8800MSCBlocker`) preventing `IOUSBMassStorageDriver` from claiming Stage 0 (`0xa69c:0x5721`) and preventing macOS `NO NAME` disk mount dialogs.
- `kexts/AIC8800WiFi/` — Pure public-KPI `IOEthernetController` kext (`com.rush.driver.AIC8800WiFi`) for Stage 2 (`0x368b:0x8d81`), including `willTerminate` / `didTerminate` hot-unplug lifecycle and pipe drain handling.
- `kexts/AIC8800BTQuirk/` — Phase 0 Lilu plugin skeleton (`com.rush.driver.AIC8800BTQuirk`).
- `daemon/aic8800_fwload/` — User-space IOKit daemon (`com.rush.aic8800_fwload`) and `launchd` property list (`daemon/com.rush.aic8800_fwload.plist`) driving automatic Stage 0 -> 1 -> 2 transitions across boot, hotplug, and S3 sleep/wake.
- `scripts/` — Helper scripts (`install_daemon.sh`, `uninstall_daemon.sh`, `verify_phase1.sh`).
- `tests/` — Host unit/regression tests (`test_abi_parity`, `test_phase0`, `test_phase1`, `test_hotpath_noheap`, `test_fwload_daemon`).
- `external/` — Git submodules for `acidanthera/Lilu` and `acidanthera/MacKernelSDK`.

## Prerequisites

- macOS with Xcode / macOS SDK (`DEVELOPER_DIR` and `SDKROOT` can be overridden via `make` variables).
- Submodules initialized:
  ```sh
  git clone --recurse-submodules https://github.com/yokitea/aic8800-macos.git
  # or, inside an existing clone:
  git submodule update --init --recursive
  ```
- External AIC8800D80 firmware binaries (e.g., `fw_adid_8800d80_u02.bin`, `fw_patch_8800d80_u02.bin`, `fw_patch_8800d80_u02_ext0.bin`, `fwacfw_8800d80_u02.bin`), supplied at runtime/install via `FW_DIR` or `--fw-dir` (not included in this repository).
- Optional Linux `aic8800_fdrv` reference header tree passed via `LINUX_FDRV=...` for ABI parity verification (`test_abi_parity`).

## Build & Test

```sh
make clean all
make test FW_DIR=/path/to/fw/aic8800D80 LINUX_FDRV=/path/to/aic8800_fdrv
```

## Daemon Installation & Verification

1. Add `AIC8800MSCBlocker.kext` and `AIC8800WiFi.kext` from `build/` to your OpenCore `EFI/OC/Kexts` and `config.plist` (ensure `AIC8800MSCBlocker.kext` loads to block Stage 0 storage attachment).
2. Install the firmware loader LaunchDaemon and firmware directory:
   ```sh
   sudo make install-daemon FW_DIR=/path/to/fw/aic8800D80
   ```
   This installs `/Library/PrivilegedHelperTools/aic8800_fwload`, copies firmware files to `/Library/Application Support/AIC8800/fw/aic8800D80`, and loads `/Library/LaunchDaemons/com.rush.aic8800_fwload.plist`.
3. Inspect live system health:
   ```sh
   ./scripts/verify_phase1.sh
   ```
4. Uninstall the LaunchDaemon if needed:
   ```sh
   sudo make uninstall-daemon
   ```

