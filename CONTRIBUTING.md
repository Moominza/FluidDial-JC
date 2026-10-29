# Contributing to FluidDial-JC

FluidDial-JC is a community port of FluidDial for JC-series ESP32 touchscreen
controllers. It retains the upstream FluidDial history and GPL-3.0 licensing.
The JC target is experimental until its hardware-specific validation is
explicitly documented; successful compilation alone is not hardware
acceptance.

## Before contributing

- Read [AGENTS.md](./AGENTS.md) for branch, compatibility, security, license,
  publishing, and CNC safety rules.
- Fork `Moominza/FluidDial-JC`, create a focused feature branch, and submit a
  pull request. Do not commit directly to `main`.
- Preserve upstream functionality and keep JC-only hardware changes inside
  the JC target unless a shared change is necessary and tested on all affected
  targets.
- Never include Wi-Fi credentials, private information, local configuration,
  binaries, or generated build output.
- Keep copyright and third-party license notices with copied or vendored work.

## Build with PlatformIO

Install PlatformIO Core 6.1.19 or use the PlatformIO IDE extension in VS Code.
From the repository root, run:

```text
pio run -e jc8048w550
pio run -e m5dial
pio run -e cyddial
```

Run `git diff --check` before submitting. If a change affects fewer targets,
run at least the relevant environment and explain why other builds were not
needed. Do not upload firmware unless you own the device and have authorization
to flash that specific target. Never flash the CNC controller as part of a
pendant build.

## Reporting hardware compatibility problems

Open an issue with:

- Exact display/pendant SKU and any readable board revision.
- ESP32 module and flash/PSRAM specifications, if known.
- Display and touch controller, interface, and relevant wiring source.
- Operating system, PlatformIO Core, framework, and dependency versions.
- The exact build/upload/monitor commands and complete relevant diagnostics.
- What was tested physically, what was not tested, and any photos that do not
  expose credentials, private networks, or other sensitive information.

Do not guess pin assignments. Cite a vendor schematic, official sample, or
clearly identify an unverified community reference.

## Bug reports and feature proposals

For bugs, include reproduction steps, expected and actual behavior, build
environment, serial logs with secrets redacted, and whether the issue occurs
on upstream M5Dial/CYD targets, JC hardware, or both.

For new display variants or transports, propose the hardware abstraction,
required pins/libraries, licensing, memory impact, test plan, and effect on
existing targets before making broad shared changes. Keep Bluetooth LE, UART,
and network protocol work separate when practical.

## Pull requests and test evidence

- Keep changes focused and explain user-visible and hardware-specific behavior.
- Link relevant issues and update directly related documentation.
- Report every build and test command with its actual result.
- Separate compile, upload, boot, visual, touch, stability, and controller
  communication evidence. A compiler result is not proof of physical behavior.
- For any status-only CNC communication work, explain the transmit restrictions
  and verify no motion or machine-changing commands can be sent.
- Do not request a merge to `main` for an unfinished or unverified feature.

## Licenses and attribution

The project is distributed under GPL-3.0; see [LICENSE](./LICENSE). Respect
that license when distributing derivative work. Include the applicable
copyright notices and the licenses for fonts, libraries, drivers, and other
third-party assets. Do not add assets unless their license permits the
intended use and redistribution.
