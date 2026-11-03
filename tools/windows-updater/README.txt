FluidDial Updater 1.5.2
=======================

Updates the FluidDial-JC pendant (Guition JC8048W550C_I) over your local Wi-Fi network.
Nothing needs to be installed: no Python, no PlatformIO, no Visual Studio Code, no ESP-IDF, no
Arduino IDE, no administrator rights. It runs on Windows 10 and 11 (64-bit).

QUICK START
-----------
1. Extract this ZIP anywhere (for example your Desktop). Keep the "firmware" folder next to the .exe.
2. Open FluidDialUpdater.exe.
3. Check the pendant's IP address (default 192.168.0.135; it is shown on the pendant under
   SETTINGS > FIRMWARE UPDATE). Change it if yours is different. "Test connection" checks it.
4. Choose the firmware. The normal choice is the FULL CONTROL image:
      fluiddial-jc-1.5.2-fullcontrol.bin
   The DIAGNOSTIC (motion disabled) image is also included. Installing it turns the pendant into a
   read-only one, so the program asks you to confirm.
5. Type the OTA password (the one you set on the pendant under SETTINGS > FIRMWARE UPDATE >
   SET PASSWORD). The program never saves it.
6. Make sure the machine is idle and nothing is running on the pendant. The pendant only accepts an
   update while the controller reports Idle with fresh status. There is no arming step: if the pendant
   page says UPDATE: READY the update is accepted.
7. Click Update. Watch the progress bar. Do not power off the pendant.
8. The pendant checks the image, restarts by itself and the program confirms that it came back on the
   network. Then check the version on the pendant (SETTINGS > FIRMWARE UPDATE). Your Wi-Fi, touch
   calibration and settings are kept.

WHAT IT CHECKS BEFORE SENDING
-----------------------------
It only sends genuine FluidDial-JC application images. It rejects M5Dial and CYD firmware, bootloader and
partition files, truncated or oversized files, and anything that does not match the SHA-256 in
firmware\MANIFEST.txt. The pendant checks the image again on its own.

SECURITY - PLEASE READ
----------------------
* The update is protected by a password only. The transfer over your network is NOT encrypted and the
  firmware is NOT cryptographically signed. Use it on a network you trust. Do not expose the pendant to
  the internet.
* Choose your own OTA password on the pendant (at least 8 characters). Do not reuse a password that has
  been written in a message or a log.
* Five wrong passwords make the pendant pause the update service for 5 minutes.

TROUBLESHOOTING
---------------
Pendant not found / "No answer from the pendant"
   - Check the IP on the pendant page; the PC and pendant must be on the same network.
   - The pendant page must say UPDATE: READY. If it says UNAVAILABLE it tells you why: no password set,
     Wi-Fi down, or the controller is not Idle / has no fresh status. Fix that first (this program
     never sends machine commands).
   - After a pendant restart the controller connection can take a minute or two to come back.
   - "Find pendant" only works while the service is available (fluiddial-jc.local).

Wrong password
   - Retype it. If you forgot it, set a new one on the pendant, then restart the pendant.

Firewall blocking the callback ("did not connect back")
   - The pendant opens a connection back to this PC to fetch the image. When Windows asks, allow
     FluidDial Updater on PRIVATE networks. If you missed the prompt: Windows Security > Firewall >
     Allow an app through firewall > FluidDial Updater > tick Private.

Upload timeout / interrupted
   - Try again closer to the router. An interrupted update never damages the pendant: the new image is
     written to the other flash slot and used only after it is fully verified.

Invalid firmware
   - Use a fluiddial-jc-*.bin application image from this package or from the release page.

Pendant does not restart
   - Look at its screen. If it shows a failure the old firmware was kept. If it shows "restart pending",
     tap RESTART PENDANT. If it stays dark, use the USB recovery procedure (docs\OTA_INSTALL_GUIDE.md in
     the project repository).

Files
-----
FluidDialUpdater.exe       the program
firmware\*.bin             FluidDial-JC firmware images (application images, not full flash images)
firmware\MANIFEST.txt      SHA-256, kind and version of each image
SHA256SUMS.txt             checksums of every file in this package
VERSION.txt                version and source information
