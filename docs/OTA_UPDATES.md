# Wi-Fi firmware updates (OTA) - FluidDial-JC 1.5.1

Update the pendant from a Windows PC over the local network, with the touchscreen left mounted and no USB cable. Applies to the two JC images (`jc8048w550_fullcontrol_test`, `jc8048w550`). It does not exist in the `m5dial` or `cyddial` builds.

Short procedures: [OTA_INSTALL_GUIDE.md](OTA_INSTALL_GUIDE.md). This page is the reference.

## What it is

- **Mechanism:** the stock Arduino ESP32 OTA stack (`ArduinoOTA`, the "espota" protocol) writing through the stock `Update` / `esp_ota_*` APIs. There is no custom flash writer and no custom cryptography.
- **Deliberate, never automatic.** The service is OFF after every boot. Nothing listens on the network until the operator arms it on the pendant (SETTINGS > FIRMWARE UPDATE > ARM UPDATE, confirmed). It switches itself off after 5 minutes, after a failure, after an update, or if you cancel. There is no automatic download, no automatic install and no cloud service. The pendant never reaches out to the internet for firmware.
- **Local network only.** It listens on UDP/TCP 3232 on the pendant's LAN address and advertises `fluiddial-jc.local` via mDNS only while armed. Do not port-forward it.

## Authentication - and what it does not give you

- You set an OTA password on the pendant (SETTINGS > FIRMWARE UPDATE > SET PASSWORD; 8-32 printable characters, no spaces). **There is no default password.** With none set the service cannot be armed. The text is masked while typed, wiped from the entry page, never displayed again and never written to the serial log. It is stored in the pendant's NVS (flash) in clear text, like the Wi-Fi password: anyone with physical access to the pendant can read it.
- The stock protocol is a challenge/response (an MD5 digest of password + nonces) before any firmware is accepted. A wrong password is refused and counted; five wrong passwords switch the service off.
- **Limitations, stated plainly:** the password is not sent, but **the firmware transfer itself is not encrypted** and **the firmware is not cryptographically signed**. Someone on your LAN who can watch traffic could see the image; the password check keeps an unauthorised device from installing one, but nothing here proves who built an image. MD5-based challenge/response is weak by current standards. Use it on a network you trust. Changing the password takes effect after the pendant is restarted (the library keeps the password it started with); the page says so and blocks arming until then.

## When an update is accepted (interlock)

Arming is refused unless ALL of these hold, using live data (a missing value counts as "not safe", never as Idle):

Wi-Fi up; an OTA password set; FluidNC status fresh on the active connection; the controller state is exactly `Idle` (not Run, Jog, Hold, Home, Alarm, Door, Check or Sleep); no guided positioning/probing/tool routine running; no jog unresolved; no machine command awaiting its reply. The page shows the reason when ARM is greyed out.

While armed and during the transfer **every machine-changing command is locked out on every connection** (Wi-Fi, UART and BLE all pass through the one transmit point where the lock sits - jogs, homing, offsets, hold, reset, unlock), the machine pages in the menu are blocked, and no reset/unlock/offset/FluidNC-setting change is ever sent. If the controller stops being Idle, loses its status or a routine starts while armed, the service is **withdrawn** and the lock released. Once bytes are flowing the update cannot be abandoned half way (the flash write must complete or fail on its own). The physical E-stop is independent of all of this. FluidNC stays the only motion controller.

## Image checks

1. The client sends the image's MD5; the library refuses a transfer whose bytes do not match it.
2. `Update.begin(size)` refuses an image larger than the target slot (3264 KB).
3. The library refuses a first byte that is not the ESP image magic (so an arbitrary file is refused).
4. `esp_ota_end` verifies the image header, segment checksums, the appended SHA-256 and the chip type; the boot partition is only switched after that succeeds.
5. **Target check (ours):** the freshly written slot is read back and must contain the marker `FDJC-IMAGE-TARGET:JC8048W550C_I:ESP32-S3:V1`, which exists only in JC images. An M5Dial or CYD image (or anything else) fails here, the boot partition is put back to the running image, no restart happens, and the page says the old firmware was kept. This is a compatibility check, not a signature. A JC diagnostic image can be installed over a full-control image and vice versa, on purpose.

## Flash layout (no migration needed)

16 MB flash, the stock dual-OTA table `default_8MB.csv`, now pinned in `platformio.ini` (`board_build.partitions`). It is the table the pendant was already running, so installing the OTA-capable firmware needs **no partition or bootloader change** and does not touch NVS:

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

The firmware is ~1.7 MB, about 52 % of a slot, so each slot has ~1.5 MB of headroom. (The upper 8 MB of the 16 MB chip is unused; moving to the 16 MB table would relocate app1 and is not needed.) Verified from the built table and by reading the table, otadata and bootloader back from the pendant: they match.

## Settings preservation

An OTA update writes only the inactive app slot and otadata. NVS is never written or erased, so Wi-Fi, the controller address, selected transport, touch calibration, jog presets and the other saved settings survive. The USB first-install procedure writes app-only regions and also leaves NVS alone - **never flash a "merged" full-flash image over 0x0**: its gap fill would overwrite NVS. The saved-settings format is versioned and migrates old blobs.

## Rollback and recovery

- **Slots:** a new image always goes to the *other* slot; the running image stays intact until the new one has been fully written and verified. An interrupted transfer, a power loss during the upload, a corrupt image or a wrong image therefore leaves the old firmware booting (designed behaviour; see "Tested" below for what has actually been exercised).
- **Rollback:** the bootloader is built with app rollback enabled (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`, read from the framework configuration). A freshly installed image boots as "pending verify". The firmware does **not** confirm it at start-up; it confirms it after 15 s of healthy running. A crash or reset before that makes the bootloader go back to the previous image. The page shows "Boot image: ..." (USB-installed / confirmed / new image pending confirm).
- **If an update misbehaves and rollback does not recover it:** reconnect USB and flash a known-good image with the first-install procedure in [OTA_INSTALL_GUIDE.md](OTA_INSTALL_GUIDE.md). That always works because the ESP32-S3 ROM serial loader cannot be overwritten by OTA. NVS settings are kept.

## Windows procedure (PlatformIO)

```powershell
$env:FLUIDDIAL_OTA_PASSWORD = "<the password you set on the pendant>"
pio run -e jc8048w550_fullcontrol_ota
pio run -e jc8048w550_fullcontrol_ota -t upload --upload-port 192.168.0.135    # or fluiddial-jc.local
```

`jc8048w550_fullcontrol_ota` builds the same sources and flags as `jc8048w550_fullcontrol_test` (not byte-identical: build paths are embedded; the release files come from the base environments) and differs only in using the espota upload protocol (`jc8048w550_ota` does the same for the diagnostic image). The password is read from the environment, never from the repository. On the pendant: connect to Wi-Fi, the controller Idle, SETTINGS > FIRMWARE UPDATE > ARM UPDATE > ARM, then run the upload command within 5 minutes. The progress bar moves, the pendant checks the image and restarts by itself.

Without PlatformIO: `python "%USERPROFILE%\.platformio\packages\framework-arduinoespressif32\tools\espota.py" -i 192.168.0.135 -p 3232 --auth=<password> -f firmware.bin`.

Windows firewall: the pendant opens a TCP connection **back to the PC** to pull the image, so the PC must allow inbound connections for `python.exe` on your private network. `fluiddial-jc.local` needs mDNS (Windows 10/11 resolve it; if not, use the IP address shown on the page).

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| ARM greyed, "controller is not Idle" | the machine is running/holding/alarmed, or status is stale - wait/clear it; the reason is on the page |
| "set an OTA password first" | SET PASSWORD |
| "restart the pendant to apply the new password" | tap RESTART PENDANT (or power-cycle) |
| upload says "Authentication Failed" | wrong password; five failures switch the service off, arm again |
| upload times out / "No response from device" | not armed, armed window ended, wrong IP, Wi-Fi roaming, or PC firewall blocking the pendant's connection back to the PC |
| "REJECTED: not a JC8048W550C_I image" | the file is not a JC image (for example an M5Dial/CYD build); the old firmware was kept |
| "image too large" / "failed verification" | corrupt or oversize file; old firmware kept |
| pendant restarts into the old version | the new image crashed before the 15 s confirmation and rolled back (check the serial log) |

## What has and has not been tested

| Item | Status |
|---|---|
| Policy: interlocks, arm/withdraw/timeout, lifecycle, wrong-password lockout, wrong-target rejection, error handling, command lockout across the shared transmit point, command policy unchanged, marker scanning | **Host-tested** (166 checks, mutation-checked) |
| Settings page layouts and hitboxes, all OTA states | **Host-rendered** (not the physical panel) |
| Static guards: no literal password, lockout present, OTA code only in the JC sources, markers only in JC binaries | **Tool-verified** on the built images |
| Partition table / bootloader / otadata on the pendant equal the build; NVS untouched by the app-only flash | **Device-read** |
| First OTA-capable image installed over USB (app + boot selector only, `esptool verify_flash` matched); boots, FULL CONTROL banner, saved calibration, saved Wi-Fi, TCP, LIVE; OTA service present and OFF; no OTA password yet | **Device-tested** (2026-11-02) |
| Saved settings of the previous format are migrated at start-up | **Device-tested** - a first install showed the old settings being ignored (a defect: the start-up length check rejected them); fixed, then the log shows "saved settings migrated from format 1" |
| A real wireless update, wrong password, update while the controller is not Idle, corrupt / wrong-target / oversize / interrupted transfers, power loss, rollback after a crashing image | **NOT tested on hardware** (need the owner at the pendant to set the password and arm it) |
