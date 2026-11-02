# FluidDial-JC 1.5.1 - which file, when

| Situation | File(s) | How |
|---|---|---|
| **Every normal update** (the pendant already runs an OTA-capable 1.5.1 image) | `FluidDial-Updater-v1.5.1-win-x64.zip` (contains the program and both firmware images) | Wi-Fi: extract, run `FluidDialUpdater.exe`, see below. No USB, no Python, no PlatformIO. |
| **First install of an OTA-capable image** (the pendant still runs a pre-OTA firmware, or the OTA service cannot be used) | `fluiddial-jc-1.5.1-fullcontrol.bin` (or `...-diagnostic-motion-disabled.bin`) | USB, **app only** (below). Needed only once. |
| **USB recovery** (OTA failed and rollback did not help, the pendant boots the wrong image, or it looks blank) | the `*-usb-*` files plus the `.bin` | USB, "full set" (below) |

The `.bin` files are **application images**. They are not complete flash images: do not write them to address 0x0, and do not build or write a "merged" image at 0x0 - that would erase the saved Wi-Fi, calibration and settings (NVS at 0x9000).

## Wi-Fi update with FluidDial Updater

1. Extract `FluidDial-Updater-v1.5.1-win-x64.zip` (keep the `firmware` folder next to the `.exe`) and open `FluidDialUpdater.exe`.
2. Enter the pendant's IP (default `192.168.0.135`; shown on SETTINGS > FIRMWARE UPDATE). **Test connection** shows whether it answers and whether the update is being offered.
3. On the pendant, SETTINGS > FIRMWARE UPDATE must say **UPDATE: READY**: a password is set, Wi-Fi is up, the controller is Idle with fresh status. There is no arming step. If it says UNAVAILABLE it tells you why.
4. Choose `fluiddial-jc-1.5.1-fullcontrol.bin` (the normal choice; the red FULL CONTROL image). The diagnostic image is motion-disabled and the program asks you to confirm before sending it.
5. Type the OTA password and click **Update**. Do not power off the pendant. The pendant checks the image, restarts and the program confirms it is back on the network.
6. Confirm the version on the pendant. Do **not** reset or power-cycle the pendant during the first 15 seconds after the restart (the new image is confirmed after 15 s of healthy running; a reset before that rolls back).

If the PC's firewall asks, allow FluidDial Updater on **Private** networks (the pendant connects back to the PC to fetch the image). The upload is protected by the password only; it is not encrypted and the firmware is not signed. Details and troubleshooting: [OTA_UPDATES.md](OTA_UPDATES.md).

## USB first install (app only, keeps all settings) - owner's PC, COM3

This is the only time a cable is needed. It needs Python with esptool (PlatformIO's copy is used below); alternatively use any ESP32-S3 flash tool with the same addresses.

1. Controller Idle and nothing running (a restart of the pendant sends no machine command, but be tidy).
2. Optional backup of the settings area: `esptool.py --port COM3 read_flash 0x9000 0x5000 nvs_backup.bin` (contains your Wi-Fi password - keep it private).
3. Write the application to slot A:

```powershell
$py  = "$env:USERPROFILE\.platformio\penv\Scripts\python.exe"
$est = "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py"
& $py $est --chip esp32s3 --port COM3 --baud 460800 write_flash 0xe000 fluiddial-jc-1.5.1-usb-boot_app0.bin 0x10000 fluiddial-jc-1.5.1-fullcontrol.bin
& $py $est --chip esp32s3 --port COM3 verify_flash 0x10000 fluiddial-jc-1.5.1-fullcontrol.bin
```

`usb-boot_app0.bin` (8 KB at 0xE000) resets the boot selector so the pendant boots slot A, which matters if an earlier OTA update left it booting slot B (or the wrong image).

4. Reset. Expect: the FULL CONTROL banner, saved calibration, Wi-Fi and LIVE as before. The partition table and bootloader on the pendant already match this release, so nothing else is written. (From a source checkout `pio run -e jc8048w550_fullcontrol_test -t upload` does the same plus the three small boot files; none of them touches NVS.)
5. SETTINGS > FIRMWARE UPDATE > SET PASSWORD (once). After that the update is offered whenever the controller is Idle.

Note: tools that read flash with `--after no_reset` leave the chip in the ROM loader (a blank-looking pendant); finish with a reset (`--after hard_reset`, or press RESET).

## USB recovery - full set (still keeps NVS)

```powershell
& $py $est --chip esp32s3 --port COM3 --baud 460800 write_flash `
  0x0     fluiddial-jc-1.5.1-usb-bootloader.bin `
  0x8000  fluiddial-jc-1.5.1-usb-partitions.bin `
  0xe000  fluiddial-jc-1.5.1-usb-boot_app0.bin `
  0x10000 fluiddial-jc-1.5.1-fullcontrol.bin
```

The regions are separate, so 0x9000-0xDFFF (NVS) is not written. If the Wi-Fi settings are lost anyway, re-enter them (SETTINGS > WI-FI SETUP), recalibrate the touch (SETTINGS > TOUCH CALIBRATION) and set the OTA password again. The serial upload normally resets into the loader by itself; if it does not, put the board into download mode with its BOOT and RESET buttons. Writing only `usb-boot_app0.bin` and the `.bin` (the first-install command above) is enough when the bootloader and partition table are fine.

## Which image?

- `fluiddial-jc-1.5.1-fullcontrol.bin` - the operational pendant (red FULL CONTROL banner). Sends operator-confirmed commands. Never starts or resumes programs.
- `fluiddial-jc-1.5.1-diagnostic-motion-disabled.bin` - motion-disabled diagnostic/preview image. Installing it over the full-control image turns the pendant into a read-only/simulated one until you install the full-control image again. Don't do it by accident; FluidDial Updater asks first.

Check downloads against `SHA256SUMS.txt`: `Get-FileHash <file> -Algorithm SHA256`.
