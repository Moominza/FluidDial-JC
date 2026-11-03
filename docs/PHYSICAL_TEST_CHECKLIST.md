# v1.5.2 RC2 physical test checklist (needs the pendant and the machine)

Nothing below has been verified on the physical panel yet. Tick each item only after seeing it.

## Display
- [ ] Idle dashboard for 10 minutes: no flashing, no tearing.
- [ ] Live coordinates changing (jog or machine motion): digits update without full-screen flashes.
- [ ] Menu navigation, jog page, axis and feed selection, repeated taps: no flash on touch.
- [ ] Settings and diagnostics pages: stable.
- [ ] OTA update: the backlight goes off during the transfer and comes back on success.
- [ ] OTA update that is interrupted (unplug the PC cable or stop the updater): the backlight comes back by itself.
- [ ] Soak: leave it running 1 hour; capture the serial `health:` lines (every 30 s: worst_poll, heap, psram).

## Jogging (machine safely clear, stop system ready)
- [ ] One tap on JOG + / JOG - = exactly one move; holding does not repeat.
- [ ] X, Y, Z, A each at the smallest step and slowest feed first.
- [ ] CANCEL JOG and FEED HOLD stop motion.
- [ ] The 6000 / 1500 / 1000 feeds are greyed out if the controller limit is lower.

## Other
- [ ] Reboot the pendant and time how long until the dashboard shows LIVE and until OTA is offered.
