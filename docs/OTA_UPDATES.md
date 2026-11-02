# Wi-Fi firmware updates (OTA) - FluidDial-JC 1.5.1

Update the pendant from a Windows PC over the local network, with the touchscreen left mounted and no USB cable. Applies to the two JC images (`jc8048w550_fullcontrol_test`, `jc8048w550`). It does not exist in the `m5dial` or `cyddial` builds.

Short procedures: [OTA_INSTALL_GUIDE.md](OTA_INSTALL_GUIDE.md). This page is the reference.

## The simple way: FluidDial Updater (no Python, PlatformIO or VS Code)

1. Download `FluidDial-Updater-v1.5.1-win-x64.zip` and extract it anywhere.
2. Open `FluidDialUpdater.exe`. No installation, no administrator rights.
3. Check the pendant's IP address (default `192.168.0.135`; it is shown on the pendant under SETTINGS > FIRMWARE UPDATE). **Test connection** checks it.
4. Choose the firmware (the full-control image is the normal one; the diagnostic image asks for confirmation).
5. Type the OTA password (set on the pendant under SETTINGS > FIRMWARE UPDATE > SET PASSWORD). It is masked, never saved and never logged.
6. On the pendant, SETTINGS > FIRMWARE UPDATE must say **UPDATE: READY** (see "When the update is offered").
7. Click **Update**. The progress bar runs; the pendant checks the image, restarts by itself, and the program confirms it came back on the network.
8. Check the version on the pendant page. Your Wi-Fi, calibration and saved settings are kept.

The program is a single small .NET Framework executable built with the compiler that ships with Windows; it uses the .NET Framework 4.x that Windows 10/11 already include. It sends only compatible FluidDial-JC application images (see "Image checks") and never sends a machine command.

## What it is

- **Mechanism:** the stock Arduino ESP32 OTA stack (`ArduinoOTA`, the "espota" protocol) writing through the stock `Update` / `esp_ota_*` APIs on the pendant, and a C# client of the same protocol on the PC. There is no custom flash writer, no custom cryptography and no new protocol. The client was checked against the stock `espota.py` (the same stand-in pendant accepts both) and against the real pendant.
- **No arming step.** The update service is available whenever an OTA password is set and the controller is fresh and Idle (below). It closes by itself the moment that stops being true. There is no automatic download, no automatic install and no cloud service. The pendant never reaches out to the internet for firmware. (An earlier build had a touchscreen ARM UPDATE button with a 5-minute window; the owner asked for it to be removed because the password is adequate. The Idle interlock was kept.)
- **Local network only.** It listens on UDP 3232 on the pendant's LAN address and advertises `fluiddial-jc.local` via mDNS only while it is available; the pendant then opens a TCP connection back to the PC to pull the image. Do not port-forward it.

## When the update is offered (interlock)

The page shows `UPDATE: READY` only while ALL of these hold, using live data (a missing value counts as "not safe", never as Idle):

Wi-Fi up; an OTA password set (and applied); FluidNC status fresh on the active connection; the controller state is exactly `Idle` (not Run, Jog, Hold, Home, Alarm, Door, Check or Sleep); no guided positioning/probing/tool routine running; no jog unresolved; no machine command awaiting its reply. Otherwise it says `UPDATE: UNAVAILABLE` and the reason ("Not available: the controller is not Idle", "controller status is missing or stale", ...). While it is unavailable the pendant does not answer the update port at all.

- **Command lock.** Machine commands are NOT locked while the service is merely READY (jogging works normally). From the moment an authenticated transfer starts, **every machine-changing command is locked out on every connection** (Wi-Fi, UART and BLE all pass through the one transmit point where the lock sits - jogs, homing, offsets, hold, reset, unlock), the machine pages in the menu are blocked, and no reset/unlock/offset/FluidNC-setting change is ever sent. The lock is released when the update fails or the pendant restarts.
- **A transfer that starts while the controller is not quiet is refused** (the write is aborted before any byte is accepted). Once bytes are flowing the update cannot be abandoned half way.
- This Windows program does not bypass the interlock and never sends a CNC command to make the controller Idle. The physical E-stop is independent of all of this. FluidNC stays the only motion controller.
- After a pendant restart the controller connection can take a few minutes to come back (FluidNC keeps the previous telnet session for a while); the service is offered again only once the status is fresh and Idle. Measured on the device: 3 to 6 minutes.

## Authentication - and what it does not give you

- You set an OTA password on the pendant (SETTINGS > FIRMWARE UPDATE > SET PASSWORD; 8-32 printable characters, no spaces). **There is no default password and none is built into any binary.** With none set the service is never offered. The text is masked while typed, wiped from the entry page, never displayed again and never written to the serial log. It is stored in the pendant's NVS (flash) in clear text, like the Wi-Fi password: anyone with physical access to the pendant can read it.
- The stock protocol is a challenge/response (an MD5 digest of the password hash and two nonces) before any firmware is accepted. A wrong password is refused and counted; **five wrong passwords pause the service for 5 minutes** (then the count restarts).
- **Limitations, stated plainly:** the password is not sent, but **the firmware transfer itself is not encrypted** and **the firmware is not cryptographically signed**. Someone on your LAN who can watch traffic could see the image; the password check keeps an unauthorised device from installing one, but nothing here proves who built an image. MD5-based challenge/response is weak by current standards. Use it on a network you trust. Changing the password takes effect after the pendant is restarted (the library keeps the password it started with); the page says so and the service is not offered until then.
- Because the service is now always available while Idle, the password is the only barrier on your LAN: choose a strong one and do not reuse a password that has appeared in a message or a log. A password used during development testing was exposed in conversation; change it (SET PASSWORD, then RESTART PENDANT) before relying on this.

## Image checks

Windows program (before anything is sent): the file must be an ESP32-S3 application image (first byte `E9`, chip id 9), no larger than the 3264 KB slot, not a bootloader/partition file, contain the JC marker, and - if it is one of the packaged images - match the SHA-256 in `firmware\MANIFEST.txt`. M5Dial and CYD images are never offered. Selecting the diagnostic image asks for confirmation.

Pendant (the authoritative checks, independent of the PC program):

1. The client sends the image's MD5; the library refuses a transfer whose bytes do not match it.
2. `Update.begin(size)` refuses an image larger than the target slot (3264 KB).
3. The library refuses a first byte that is not the ESP image magic.
4. **Target check, at the last byte, before the boot slot is switched:** the written slot is scanned for the marker `FDJC-IMAGE-TARGET:JC8048W550C_I:ESP32-S3:V1`, which exists only in JC images. An image without it (an M5Dial image is a valid ESP32-S3 image and would otherwise pass) is aborted; the client receives `Aborted`, nothing restarts, the boot selector is untouched, and the page says the old firmware was kept. A build-time check (`tools/check_firmware_symbols.py`) proves the marker lies in the part of the image this check covers.
5. `esp_ota_end` verifies the image header, segment checksums, the appended SHA-256 and the chip type; the boot slot is only switched after that succeeds. A corrupted image fails here ("Could Not Activate The Firmware").
6. Second layer: after the library finishes, the marker is checked again; if it fails the boot slot is put back **and read back to verify it**. If it could not be restored the pendant says `BOOT SLOT NOT RESTORED - do not restart; reinstall by USB` and refuses to restart.

This is a compatibility check, not a signature. A JC diagnostic image can be installed over a full-control image and vice versa, on purpose.

**Defect found and fixed during acceptance:** the first implementation relied only on restoring the boot slot after the fact (step 6 without the read-back). On the real pendant a single `esp_ota_set_boot_partition(running)` left the boot selector pointing at the rejected M5Dial image (a restart would have booted it). USB recovery (below) put it right. The check was moved before the switch (step 4), step 6 now verifies and retries, and the cross-flash test was repeated on the device (see the table).

## Flash layout (no migration needed)

16 MB flash, the stock dual-OTA table `default_8MB.csv`, pinned in `platformio.ini` (`board_build.partitions`). It is the table the pendant was already running, so installing the OTA-capable firmware needs **no partition or bootloader change** and does not touch NVS:

| Name | Offset | Size | Purpose |
|---|---|---|---|
| (bootloader) | 0x0000 | 15 KB | rollback-enabled stock bootloader (identical to what is flashed) |
| (partition table) | 0x8000 | 3 KB | |
| nvs | 0x9000 | 20 KB | Wi-Fi, controller address, calibration, settings, OTA password |
| otadata | 0xE000 | 8 KB | which app slot boots, and the pending/valid state |
| app0 (ota_0) | 0x10000 | 3264 KB | firmware slot A |
| app1 (ota_1) | 0x340000 | 3264 KB | firmware slot B |
| spiffs | 0x670000 | 1536 KB | unused by the pendant |
| coredump | 0x7F0000 | 64 KB | |

The firmware is ~1.7 MB, about 52 % of a slot. Verified from the built table and by reading the table, otadata and bootloader back from the pendant.

## Settings preservation

An OTA update writes only the inactive app slot and otadata. NVS is never written or erased, so Wi-Fi, the controller address, selected transport, touch calibration, jog presets and the other saved settings survive; a failed, refused or interrupted update changes neither slot's running image nor NVS. The USB first-install procedure writes app-only regions and also leaves NVS alone - **never flash a "merged" full-flash image over 0x0**: its gap fill would overwrite NVS. The saved-settings format is versioned and migrates old blobs (a first-install defect where old settings were ignored was found on the device and fixed).

## Rollback and recovery

- **Slots:** a new image always goes to the *other* slot; the running image stays intact until the new one has been fully written and verified. An interrupted transfer, a corrupt image or a wrong image leaves the old firmware booting (device-tested, see the table).
- **Rollback:** the bootloader is built with app rollback enabled (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`). A freshly installed image boots as "pending verify"; the firmware confirms it after 15 s of healthy running (observed on the device: the otadata entry of an OTA-installed image reads state VALID afterwards). A crash or reset **before** that makes the bootloader go back to the previous image. This is why the pendant must not be reset (and nothing should read it over USB with a reset) in the first 15 seconds after an update. The rollback itself (a crashing image being undone) has **not** been exercised on hardware.
- **USB recovery** always works because the ESP32-S3 ROM serial loader cannot be overwritten by OTA. If the boot selector is wrong or an update misbehaves: reconnect USB and write `usb-boot_app0.bin` at 0xE000 (resets the selector to slot A) and a known-good image at 0x10000, as in [OTA_INSTALL_GUIDE.md](OTA_INSTALL_GUIDE.md). NVS settings are kept. This was used twice during acceptance.

## Windows firewall

The pendant opens a TCP connection **back to the PC** to pull the image, so the PC must allow inbound connections for FluidDial Updater on your **Private** network. Windows asks the first time; if you missed the prompt: Windows Security > Firewall > Allow an app through firewall > FluidDial Updater > Private. The program reports "the pendant did not connect back" when this is the cause. `fluiddial-jc.local` needs mDNS (Windows 10/11 resolve it; the IP shown on the pendant page always works).

## Advanced: PlatformIO or the stock espota.py

```powershell
$env:FLUIDDIAL_OTA_PASSWORD = "<the password you set on the pendant>"
pio run -e jc8048w550_fullcontrol_ota -t upload --upload-port 192.168.0.135
```

`jc8048w550_fullcontrol_ota` builds the same sources and flags as `jc8048w550_fullcontrol_test` (not byte-identical: build paths are embedded; release files come from the base environments); `jc8048w550_ota` does the same for the diagnostic image. Without PlatformIO but with Python: `python espota.py -i 192.168.0.135 -p 3232 --auth=<password> -f firmware.bin`. These bypass the Windows program's file checks; the pendant's own checks still apply. The stock tool reports `OK` when the library accepts the image: it does not wait for the restart.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| Page says UNAVAILABLE, "the controller is not Idle" / "status is missing or stale" | the machine is running/holding/alarmed, or the controller link is down; after a pendant restart wait a few minutes for LIVE. The program never sends anything to change this |
| UNAVAILABLE, "set an OTA password first" | SET PASSWORD |
| UNAVAILABLE, "restart the pendant to apply the new password" | tap RESTART PENDANT (or power-cycle) |
| Program: "Wrong OTA password" | retype; five wrong passwords pause the service for 5 minutes |
| Program: "No answer from the pendant" | the service is not being offered (see the page), wrong IP, the PC is on another network, or Wi-Fi roaming |
| Program: "did not connect back" | PC firewall blocking the pendant's connection back to the PC (see above) |
| Program: "REJECTED" / "not a FluidDial-JC image" | the file is not a JC image (for example an M5Dial/CYD build); nothing was sent / the old firmware was kept |
| Pendant shows FAILED | it clears itself after 60 s, or tap CLEAR FAILURE; the old firmware is running |
| Pendant restarts into the old version | the new image crashed before the 15 s confirmation and rolled back (check the serial log) |

## What has and has not been tested

| Item | Status |
|---|---|
| Policy: interlocks, availability/withdraw, lifecycle, wrong-password pause and recovery, wrong-target rejection before the switch, unrestorable-boot handling, error handling, failure auto-clear, command lock across the shared transmit point, command policy unchanged, linear marker scanning | **Host-tested** (194 checks, mutation-checked earlier) |
| Windows program: protocol (invitation, challenge, response, connect-back, chunks), file checks (M5Dial/CYD-style, wrong chip, bootloader, truncated, oversized, arbitrary file), wrong password, five wrong passwords, unavailable service, unreachable pendant, invalid address, empty password, interrupted transfer, device-side rejection, device refusing an oversized image, diagnostic-image confirmation, password never echoed | **Host-tested against a stand-in pendant** that follows the ArduinoOTA source; the stand-in itself accepts the stock `espota.py` |
| Settings page layouts and hitboxes, all OTA states | **Host-rendered** (not photographed on the panel) |
| Static guards: no literal password, lockout present, OTA code only in the JC sources, marker only in JC binaries, marker inside the checked part of the image | **Tool-verified** on the built images |
| Partition table / bootloader / otadata on the pendant equal the build; NVS untouched by app-only USB writes | **Device-read** |
| Real wireless updates from this PC (stock espota.py once, FluidDial Updater twice, then the final image): authentication, transfer of 1.7 MB, image accepted, pendant restarted and returned to the network, boot selector switched to the other slot and the new image confirmed VALID, installed bytes identical to the release file | **Device-tested** (see RELEASE_NOTES for the record) |
| Wrong password on the real pendant; transfer cut off at 25 %; corrupted image; **M5Dial image (cross-flash)** refused, boot selector unchanged; service offered again afterwards | **Device-tested** (interrupted and corrupted on the build before the final refusal fix; the foreign-image test repeated on the final build) |
| Service offered with no arming step; controller Idle + fresh status gating (the probe is answered only after the controller link returned) | **Device-tested** |
| Update refused while the controller is not Idle | **Host-tested only.** Not induced on the device: that would need a machine command (no motion was commanded in these tests) |
| Power loss during a transfer; a crashing image being rolled back; oversized image on the device; Windows firewall prompt on a clean PC; running the updater on a PC without any development tools | **NOT tested** |
| Touch calibration, jog presets and Wi-Fi after an OTA: Wi-Fi and the controller link returned by themselves (observed over the network); calibration and presets are not written by an OTA, but a visual check of them after the update was not recorded | **Partly device-tested** |
| The flashing the owner saw on the panel during the first update | **Reported by the operator** while the first wireless update ran. The software cause (the progress bar being cleared and redrawn about 100 times) was fixed; whether residual flicker from flash writes remains on the new build is **not yet re-checked by the operator** |
