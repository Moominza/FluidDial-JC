# FluidDial-JC 1.5.1 - which file, when

| Situation | File(s) | How |
|---|---|---|
| **First install of an OTA-capable image** (the pendant still runs a pre-OTA firmware) | `fluiddial-jc-1.5.1-fullcontrol.bin` (or `...-diagnostic-motion-disabled.bin`) | USB, **app only** (below). This is the only time a cable is needed. |
| **Every later update** | the same `.bin` files | Wi-Fi OTA ([OTA_UPDATES.md](OTA_UPDATES.md)); no USB |
| **USB recovery** (OTA failed and rollback did not help, or a blank/bricked-looking pendant) | the four `*-usb-*` files plus the `.bin` | USB, "full set" (below) |

These `.bin` files are **application images**. They are not complete flash images: do not write them to address 0x0, and do not build or write a "merged" image at 0x0 - that would erase the saved Wi-Fi, calibration and settings (NVS at 0x9000).

## USB first install (app only, keeps all settings) - owner's PC, COM3

1. Controller Idle and nothing running (a restart of the pendant sends no machine command, but be tidy).
2. Optional backup of the settings area: `esptool.py --port COM3 read_flash 0x9000 0x5000 nvs_backup.bin` (contains your Wi-Fi password - keep it private).
3. Write the application to slot A:

```powershell
$py  = "$env:USERPROFILE\.platformio\penv\Scripts\python.exe"
$est = "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py"
& $py $est --chip esp32s3 --port COM3 --baud 460800 write_flash 0xe000 fluiddial-jc-1.5.1-usb-boot_app0.bin 0x10000 fluiddial-jc-1.5.1-fullcontrol.bin
& $py $est --chip esp32s3 --port COM3 verify_flash 0x10000 fluiddial-jc-1.5.1-fullcontrol.bin
```

`usb-boot_app0.bin` (8 KB at 0xE000) resets the boot selector so the pendant boots slot A, which matters if an earlier OTA update had left it booting slot B.

4. Reset. Expect: the FULL CONTROL banner, saved calibration, Wi-Fi and LIVE as before. The partition table and bootloader on the pendant already match this release, so nothing else is written. (From a source checkout `pio run -e jc8048w550_fullcontrol_test -t upload` does the same plus the three small boot files; none of them touches NVS.)
5. SETTINGS > FIRMWARE UPDATE > SET PASSWORD (once), then the OTA path is ready.

## USB recovery - full set (still keeps NVS)

```powershell
& $py $est --chip esp32s3 --port COM3 --baud 460800 write_flash `
  0x0     fluiddial-jc-1.5.1-usb-bootloader.bin `
  0x8000  fluiddial-jc-1.5.1-usb-partitions.bin `
  0xe000  fluiddial-jc-1.5.1-usb-boot_app0.bin `
  0x10000 fluiddial-jc-1.5.1-fullcontrol.bin
```

The regions are separate, so 0x9000-0xDFFF (NVS) is not written. If the Wi-Fi settings are lost anyway, re-enter them (SETTINGS > WI-FI SETUP), recalibrate the touch (SETTINGS > TOUCH CALIBRATION) and set the OTA password again. The serial upload normally resets into the loader by itself; if it does not, put the board into download mode with its BOOT and RESET buttons.

## Which image?

- `fluiddial-jc-1.5.1-fullcontrol.bin` - the operational pendant (red FULL CONTROL banner). Sends operator-confirmed commands. Never starts or resumes programs.
- `fluiddial-jc-1.5.1-diagnostic-motion-disabled.bin` - motion-disabled diagnostic/preview image. Flashing it over the full-control image turns the pendant into a read-only/simulated one until you install the full-control image again. Don't do it by accident.

Check the download against `SHA256SUMS.txt` before installing: `Get-FileHash <file> -Algorithm SHA256`.
