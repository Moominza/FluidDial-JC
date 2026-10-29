# JC-series FluidDial port plan

## Scope and status

This document records the Phase 0 investigation for a separate FluidDial
hardware target tentatively named `jc8048w550`. It does not change the M5Dial
or CYD targets, and it does not authorize or perform a device flash.

The selected workspace originally contained only its `.code-workspace` file,
not a Git repository. The upstream repository was therefore cloned into the
`FluidDial-JC` subdirectory, retaining its Git history, and work is on
`feature/jc8048w550`.

**Investigation baseline:** upstream `main` at
[`47f8489dd49b8b8446c7d3a5fb6c5ba5d2fc2e9f`](https://github.com/bdring/FluidDial/commit/47f8489dd49b8b8446c7d3a5fb6c5ba5d2fc2e9f),
dated 2026-10-20. The upstream repository declares GPL-3.0 and includes its
`LICENSE`; preserve the license, copyright notices, and attribution in
derivative code.

**Hardware identification:** the user supplied a board photo showing the
module label `LCD Module/JC8048W550C_I` and a matching JC8048W550 board, plus
the manufacturer specification and 5-inch getting-started manual. The exact
SKU is now confirmed as the capacitive-touch `_C_I` variant. The supplied
vendor demo source and I/O spreadsheet provide the RGB/sync pin map and the
GT911/backlight assignments used by the isolated target.

**Build and hardware test record:** PlatformIO Core 6.1.19 is installed under
the user profile but is not on `PATH`; it was invoked by absolute path. The
unchanged `m5dial` and `cyddial` builds pass (details below). The isolated JC
target was compiled and uploaded to the operator-confirmed JC8048W550C_I on
COM3. In Phase 1C, a local, documented patch to the pinned GT911 driver
removed operations on its unconnected interrupt pin; the corrected firmware
booted without the former GPIO warnings. The supplied 37.78-second video
shows touch activity and release states, and the operator reports a center
reading of `(390,240)`. Raw coordinates and an unambiguous labeled five-point
serial record are still outstanding, so final touch acceptance remains
partial. No CNC motion command was included or issued.

## Phase 0 findings

### Upstream structure and current build targets

- The application and scene framework are in `src/`; hardware backends are
  selected by PlatformIO source filters in `platformio.ini`.
- `m5dial` uses `espressif32@6.9.0`, Arduino framework package
  `3.20017.0`, M5Unified/M5Dial, `USE_M5`, and `USE_WIFI`.
- CYD builds extend `cyd_base`, use `espressif32@6.9.0`, Arduino framework
  package `^3.20016.0`, LovyanGFX from its `develop` branch, and select
  `Hardware2432.cpp` and `SystemArduino.cpp`. `cyddial` enables Wi-Fi and
  continues to coexist with the other CYD variants.
- `default_envs` remains `m5dial, cyddial`. The intended JC environment must
  be opt-in and must not modify those defaults or existing source filters.
- Shared dependency pins include Mitch Bradley's json-streaming-parser
  `charp-1.0.2` and GrblParser commit
  `9108f54a3e30526b66194898676af34e96a87da9`. The existing build also uses
  `git-version.py`, LittleFS, `build_merged.py`, and per-developer
  `platformio.local.ini` overrides.
- `USE_WIFI` routes the pendant's FluidNC protocol through the existing
  WebSocket/Wi-Fi layer. Wi-Fi setup uses the existing setup flow; credentials
  must remain user-supplied and outside committed source.

### Hardware, graphics, and input abstraction

- M5Dial has a `HardwareM5Dial.cpp` backend that obtains the display, sprite,
  touch, buttons, and speaker from M5Unified/M5Dial.
- CYD has a `Hardware2432.cpp` backend built around LovyanGFX. It configures
  an SPI display, board-specific panel and touch drivers, backlight, button
  inputs, and encoder pins. Its capacitive touch variants use CST816S or
  FT5x06; the resistive variant uses XPT2046. These mappings are CYD-specific
  and must not be copied to the JC board.
- `System.h` and `Drawing.h` expose the existing shared display/sprite,
  color, drawing, and event interfaces. `Scene.cpp` dispatches touch
  callbacks, button events, and encoder events to shared scenes.
- CYD `screen_button_touched()` maps screen regions to the three legacy
  red/dial/green button actions; `screen_encoder()` is currently a no-op.
  Touch points and clicks/flicks are already available to scenes, but many
  scene layouts and art assets are sized and placed for the 240-pixel CYD
  presentation. The M5Dial backend also uses a 240x240 sprite.
- The shared `SystemArduino.cpp` initializes a fixed 240x240 canvas and
  contains the UART and Wi-Fi protocol plumbing. A landscape target needs a
  deliberate sprite-size/layout strategy, not just a panel rotation change.
- Existing scenes and FluidNC model/protocol code are promising reuse points.
  Touch controls need to be designed and tested for each motion action; UI
  enlargement alone does not establish that a scene's operation is safe.

### JC board investigation and compatibility risks

The user supplied a board photo whose label reads `LCD Module/JC8048W550C_I`;
the PCB is marked `JC8048W550` and carries a Guition label. The supplied
manufacturer specification, `JC8048W550 Specifications-EN`, identifies
`JC8048W550C_I` as the capacitive-touch SKU, distinct from the N (no touch)
and R (resistive touch) variants. It specifies a 5-inch 800x480, 16-bit RGB
panel driven by ST7262, ESP32-S3-WROOM-1, 16 MB flash, 8 MB PSRAM, 5 V supply,
and approx. 320 mA consumption. The specification does not state the PSRAM
interface/mode or provide a complete GPIO-level timing/panel hookup table.
The supplied `Getting started 5.0 Inch` manufacturer manual recommends
Arduino_GFX for sample applications, but does not document the FluidDial
hardware configuration or provide a complete readable RGB GPIO map.

A Tactility device definition for “Guition JC8048W550C” and an independent
JC8048W550 community project publish plausible RGB pin maps and timings.
These are useful cross-checks, not a Guition-controlled schematic. The
Tactility definition flags touch reset/interrupt wiring as conflicting and
unverified. The vendor demo provides the GPIO38 GT911 reset assignment used
in the bring-up target.
The supplied specification's interface diagram shows physical connectors
but not the complete MCU-to-panel RGB GPIO mapping, and the getting-started
manual recommends Arduino_GFX for its samples without publishing the
FluidDial configuration. The supplied vendor demo source and I/O spreadsheet
do give the following concrete configuration, so these values are used by
the isolated bring-up target:

| Signal | Vendor-provided configuration |
| --- | --- |
| RGB data bus | `Arduino_ESP32RGBPanel` constructor: R `{45,48,47,21,14}`, G `{5,6,7,15,16,4}`, B `{8,3,46,9,1}` |
| Sync/control | DE `40`, VSYNC `41`, HSYNC `39`, PCLK `42` |
| Timing | 800x480; H/V polarity `0`; front porch `8`, pulse `4`, back porch `8`; PCLK active-negative; 16 MHz |
| Backlight | GPIO `2`, output HIGH |
| Touch | GT911; SDA `19`, SCL `20`, INT `-1` (not connected), reset `38`; normal rotation; 800x480 |
| Diagnostics serial | UART0 TX/RX on GPIO `43`/`44` per I/O spreadsheet |

The sources are the supplied vendor bundle's
`1-Demo/Demo_Arduino/3_3-1_TFT_HelloWorld/HelloWorld/HelloWorld.ino`,
`1-Demo/Demo_Arduino/3_3-4_TFT-LVGL-Widgets/LvglWidgets/touch.h`, and
`5-IO pin distribution/5.0 inches IO pin distribution.xlsx`. This
manufacturer-provided sample evidence resolves the earlier GPIO-map blocker
for an opt-in bring-up build. Community Tactility and ESPHome configurations
remain cross-checks, not the basis for pin assignment. The physical test
pattern is visible in operator photos, with distinct RGB/white/black swatches,
corner labels and straight reference lines. The operator reports 15-30 minutes
of stable operation after startup. Exact touch-to-corner mapping is not fully
documented. GT911 I2C and touch activity work, but the library's handling of
the unconnected interrupt pin emits invalid-GPIO diagnostics during reset.
Because the pin spreadsheet also labels GPIO19/20 as native USB D-/D+ while
assigning them to touch I2C, the JC environment leaves native USB CDC-on-boot
disabled and uses Arduino `Serial`/UART0 for diagnostics to avoid claiming
those shared pins for USB and GT911 at the same time.

The board specification confirms 8 MB PSRAM but does not state its bus type.
The JC environment uses PlatformIO's `qio_opi` memory mode (octal PSRAM),
following the community N16R8 board identification. The bring-up explicitly
calls `psramInit()` and stops with a serial error if initialization fails;
the exact memory mode still requires confirmation from startup diagnostics.

The 800x480 panel is an RGB parallel display, not the SPI display used by the
CYD. At RGB565, one full frame is 768,000 bytes (about 750 KiB); double
framebuffers alone consume about 1.5 MiB. A full-size scene sprite or
additional frame buffers increase PSRAM and bandwidth requirements. A
16-bit RGB frame at 60 Hz would imply roughly 46 MB/s of active pixel data
before timing overhead; refresh rate, pixel clock, porch values, and buffer
mode must be measured/validated for the actual panel.

The current pinned upstream ESP32 platform/Arduino packages are from the
Arduino-ESP32 2.x generation. RGB-panel APIs, PSRAM initialization and
allocation, and LCD DMA behavior depend on the exact ESP-IDF/Arduino core,
board PSRAM mode, LovyanGFX version, and controller. Do not assume the CYD
combination (or a moving `develop` branch) supports this RGB panel. Evaluate
LovyanGFX's RGB bus against the selected toolchain first; if that is
incompatible or unstable, compare a pinned Espressif `esp_lcd` RGB driver or
Espressif's display-panel component and keep an adapter at the FluidDial
display boundary. A toolchain upgrade, if needed, must be isolated to the JC
environment rather than silently changing the existing targets.

### Build verification

From the clone at upstream commit
`47f8489dd49b8b8446c7d3a5fb6c5ba5d2fc2e9f`, PlatformIO Core 6.1.19 ran
`pio run -e m5dial -e cyddial` successfully:

| Environment | Result | Duration | Scope |
| --- | --- | ---: | --- |
| `m5dial` | PASS | 8m 10s | Existing upstream target |
| `cyddial` | PASS | 1m 37s | Existing upstream target |

This verifies the unchanged upstream targets compile with their pinned
ESP32 platform 6.9.0 and Arduino framework 2.0.17 in this environment. It is
not a physical display/touch test or a CNC communication test.

The isolated JC environment was compiled with `pio run -e jc8048w550` using
ESP32 platform 6.9.0, Arduino core 2.0.17, Arduino_GFX 1.2.9
(`20d9ad25557e6d07668ad2192395a6058092af1c`), and TAMC_GT911 1.0.2
(`b3f175e65a799368be9c544e255204e1e74ad2ed`):

| Environment | Result | Duration | Scope |
| --- | --- | ---: | --- |
| `jc8048w550` | PASS | 39s | Isolated display/touch bring-up firmware; no CNC transport |

The initial JC result is compile-only. The final JC build, after the explicit
PSRAM check and leaving native USB CDC disabled, passed in 47 seconds and was
uploaded to the physical board; the separate hardware observations are
recorded in Phase 1B below. After adding the opt-in JC environment,
`pio run -e m5dial -e cyddial` was rerun; both existing targets passed again
(m5dial 1m 11s, cyddial 1m 03s).

### Phase 1B — physical hardware validation

**Status: PARTIAL; build, upload, boot, panel output, and touch activity
verified.** The operator confirmed the Guition `JC8048W550C_I` was connected
on `COM3`; the port enumerated as a USB-serial CH340, and esptool identified
an ESP32-S3 during upload. This was the isolated `jc8048w550` bring-up image;
it contains no CNC transport, scenes, or motion commands. No CNC controller
was flashed or contacted.

From PowerShell in the repository root:

```powershell
$pio = 'C:\Users\etien\.platformio\penv\Scripts\pio.exe'
& $pio device list
& $pio run -e jc8048w550
& $pio run -e jc8048w550 -t upload --upload-port COM3
& $pio device monitor -p COM3 -b 115200
```

If the JC board enumerates on a different COM port, replace `COM3` in both
commands with the port shown by `pio device list`. Close any other serial
monitor before uploading. If auto-reset does not enter the bootloader, follow
the vendor manual's boot procedure using the board's BOOT/RESET controls,
then repeat only the upload command. Once the monitor is open, reset/power
cycle the JC board to capture a clean startup. Stop the monitor with
`Ctrl+C`. Do not run any upload command for `m5dial` or `cyddial`.

Observed startup diagnostics (free-heap values can vary by boot):

```text
JC8048W550C_I isolated hardware bring-up
chip: ESP32-S3, revision: 0
flash: 16777216 bytes
PSRAM initialized: yes, size: 8386279 bytes
PSRAM heap: 8388607 bytes
PSRAM free heap: 8386035 bytes
internal free heap: 366980 bytes
display: ST7262 RGB initialized; resolution=800x480
backlight: GPIO2 driven HIGH (full-brightness test).
touch: GT911 I2C 0x5D ACK
touch: configured SDA=19, SCL=20, RST=38, INT=NC, rotation=normal.
Ready. This image has no CNC transport or motion commands.
touch: count=1 x=<observed panel coordinates> y=<observed panel coordinates> size=<value>
touch: released
```

The upload completed and esptool verified the flashed image hash. The
115200-baud serial monitor captured startup diagnostics, GT911 ACK, touch
coordinates and release events. Startup also emitted GPIO errors because the
pinned TAMC_GT911 library attempts GPIO operations on the configured
unconnected interrupt pin `-1`; touch reads continued to work. This is a
non-fatal but unresolved driver diagnostic, not evidence that the interrupt
line is connected.

#### Physical validation checklist

- [x] Verify the operator-confirmed board identity and COM3 upload target;
      esptool identified an ESP32-S3 and flash hash verification succeeded.
- [x] Capture actual startup diagnostics: ESP32-S3 revision 0; 16,777,216
      bytes flash; 8,386,279 bytes PSRAM; PSRAM heap total/free
      8,388,607/8,386,035 bytes; internal free heap 366,980 bytes.
- [x] Confirm firmware reports display initialization at 800x480 and an
      operator photo shows the complete landscape test pattern.
- [x] Operator photos show distinct RED, GREEN, BLUE, WHITE, and BLACK
      swatches, all four corner labels, and horizontal/vertical reference
      lines. No obvious RGB order error, cropping, or shifted image is visible.
- [x] Operator photos show the backlight illuminated and touch coordinate
      readouts; the serial log confirms GT911 `0x5D` ACK, touch events, and
      release events.
- [x] Coordinate values visible in photos include `(1,431)`, `(8,3)`,
      `(134,480)`, `(785,4)`, and `(789,453)`, spanning near the panel bounds.
      The photos/log do not document a complete named five-point tap sequence
      or center value, so exact orientation/calibration is not declared fully
      verified.
- [x] Phase 1C resolves the TAMC_GT911 invalid-GPIO diagnostics by retaining
      `INT=-1` and skipping interrupt GPIO operations; no interrupt GPIO is
      assigned.
- [x] Operator reports 15-30 minutes of stable operation after upload, with
      a few flashes and colors during startup. No subsequent reset, display
      corruption, or touch failure was reported; the captured serial log does
      contain the separately documented invalid-GPIO diagnostics.
- [x] Keep FluidNC communication and CNC motion out of the JC build; no CNC
      controller firmware/configuration was changed.

### Phase 1C — final hardware validation

**Status: PARTIAL.** The GT911 no-interrupt fault was corrected without
assigning an undocumented pin. The patched driver retains polling and reset
GPIO38 behavior, and the existing display GPIO/timing configuration is
unchanged. The corrected image built and uploaded to COM3 successfully; its
captured startup log has no invalid-GPIO warnings. M5Dial and CYDDial both
rebuilt successfully. The operator-supplied 37.78-second video shows the
test pattern during touch activity, touch-release indications, and a center
reading reported by the operator as `(390,240)`. The recording does not
include serial diagnostics, so it cannot establish raw-to-mapped values or
label every corner reading. No long-duration stability period for this
corrected image has been reported.

The vendored GT911 source is based on TAMC_GT911 1.0.2 at commit
`b3f175e65a799368be9c544e255204e1e74ad2ed` and retains its Apache-2.0
license. See `lib/TAMC_GT911/README.md` for the upstream pin and local patch
description. The upstream constructor accepted the “not connected” interrupt
value as `uint8_t`, converting `-1` to `255`, and `reset()` then called
`pinMode`/`digitalWrite` on that invalid GPIO. The vendored patch stores the
interrupt pin as signed `int` and skips all interrupt GPIO operations when it
is negative. `read()` already polls the GT911 point-info register; polling
continues every 20 ms, while reset GPIO38 is still pulsed for address
selection and controller reset.

Coordinate handling preserves the existing `ROTATION_NORMAL` transform and
calibration: `rawX/rawY` retain the sensor readings, `driver_x/driver_y`
retain the driver's rotated values, and only the displayed `ui_x/ui_y` are
clamped to X `0..799` and Y `0..479`. No axis or calibration was changed.
Because the GT911 library transforms using `width - raw` and `height - raw`,
its driver values may equal 800 or 480 at an edge; the UI clamp makes these
valid while the raw and driver diagnostics preserve that evidence.

```text
JC8048W550C_I isolated hardware bring-up
chip: ESP32-S3, revision: 0
flash: 16777216 bytes
PSRAM initialized: yes, size: 8386279 bytes
PSRAM heap: 8388607 bytes
PSRAM free heap: 8386035 bytes
internal free heap: 366956 bytes
display: ST7262 RGB initialized; resolution=800x480
touch: GT911 I2C 0x5D ACK
touch: configured SDA=19, SCL=20, RST=38, INT=NC, rotation=normal.
Ready. This image has no CNC transport or motion commands.
```

| Phase 1C acceptance check | Result and evidence |
| --- | --- |
| Corrected JC build | PASS: `pio run -e jc8048w550`, 49.11 seconds |
| JC-only upload | PASS: COM3, ESP32-S3 identified, firmware/partition hash verification passed |
| Startup diagnostics | PASS: PSRAM initialized, 800x480 display, GT911 `0x5D` ACK; former invalid-GPIO warnings absent |
| No CNC transport/motion in image | PASS: target source filter includes only `src/jc8048w550/*.cpp`; no transport, FluidDial scenes, or motion code |
| Touch polling and release | PARTIAL: video shows touch activity/release; no serial capture of corrected raw readings |
| Five-point touch accuracy | PARTIAL: operator reports centre `(390,240)`; video shows corner-area touches but does not provide a complete labeled raw/mapped serial set |
| UI coordinate bounds | PASS by code inspection: final display coordinates clamp to X `0..799`, Y `0..479`; raw and driver coordinates are logged separately |
| Stability on corrected image | PARTIAL: 37.78-second video only; no reset observed in that clip, but no 15-30-minute run reported for this version |
| Existing target regression builds | PASS: `pio run -e m5dial -e cyddial`; m5dial 69.45 seconds, cyddial 61.87 seconds |
| CNC controller / FluidNC changes | NONE |

Phase 1C is not a full hardware acceptance because the corrected firmware's
serial log has not captured raw and mapped readings at all five named
positions, and stability beyond the supplied clip is unverified. Keep the
report at PARTIAL until those measurements are recorded. Do not begin Phase 2
without the operator's approval.

### Preliminary architecture and interface plan

1. Keep the upstream M5Dial and CYD environments, source filters, and default
   build environments unchanged.
2. Add an opt-in `[env:jc8048w550]` only after identifying the exact board and
   revision. Use a JC-only hardware backend; do not make `Hardware2432.cpp`
   conditional pin logic serve both products.
3. Keep the landscape target at 800x480, with a display abstraction for RGB
   scanout, backlight, touch reads/calibration, and diagnostics. Choose the
   concrete RGB driver only after a minimal build against the verified board
   and framework version.
4. First make an isolated bring-up image that initializes PSRAM, display, and
   backlight; draws diagnostic colors/geometry; reports touch coordinates and
   startup diagnostics over USB serial; and does not compile or call the CNC
   transport, scene actions, or any motion-command path.
5. After bring-up is validated, integrate the backend with the existing
   `System`/`Drawing` boundary and reuse FluidNC model logic and scenes where
   their dimensions and event semantics remain suitable. Introduce
   landscape-specific layouts only where needed.
6. Preserve `USE_WIFI` and the existing WebSocket/setup path for subsequent
   CNC communication testing. Do not embed credentials. Keep all machine
   motion interaction out of the Phase 1 image.
7. Map legacy encoder/button operations to explicit, large touch controls.
   For jogging, require a deliberate press-and-hold interaction that stops
   on release, show the selected axis, step size, and connection state, and
   make the motion action unavailable when touch is lost or the pendant is
   locked/disconnected. Validate stop-on-release behavior with a simulator
   or disconnected machine before any real CNC motion test. Confirm the
   required behavior with the user before designing more nuanced control
   gestures.

## Milestone checklist

### Phase 0 — investigation (complete)

- [x] Preserve upstream Git history and GPL-3.0 attribution.
- [x] Inspect upstream targets, build flags, dependencies, source filters,
      display/touch/encoder interfaces, and shared scene model.
- [x] Create `feature/jc8048w550` from the upstream baseline.
- [x] Record the upstream baseline commit and honest build/test status.
- [x] Document display, touch, PSRAM, framework, and scene-reuse risks.
- [x] Build the unchanged upstream `m5dial` and `cyddial` targets; both passed
      with PlatformIO Core 6.1.19 (see build record).
- [x] Confirm RGB, sync, touch, and backlight configuration from the supplied
      vendor demo and I/O pin spreadsheet; physical checks remain below.
- [x] Confirm the installed `JC8048W550C_I` SKU and capacitive-touch variant
      against the board label, photograph, and manufacturer specification.
- [x] Select and pin Arduino_GFX and GT911 libraries and compile a hardware
      configuration spike for the isolated target.
- [x] Record that no exact PCB revision marking was legible in the supplied
      photos; the installed SKU and vendor sample wiring are identified.
- [x] Confirm actual panel output, touch activity, and illuminated backlight
      on the physical board; document remaining stability and GT911 GPIO
      diagnostic limitations in the Phase 1B record.

### Phase 1 — minimal hardware bring-up

- [x] Add an isolated, opt-in `jc8048w550` environment without altering
      `m5dial`, `cyddial`, or `default_envs`.
- [x] Add PSRAM initialization/diagnostics and stop visibly if required
      PSRAM is absent.
- [x] Add RGB panel, test pattern, backlight, and GT911 initialization.
- [x] Log startup/chip/flash/PSRAM diagnostics and touch coordinates over USB.
- [x] Build the isolated target with `pio run -e jc8048w550`.
- [x] Draw the color-bar/grid test and verify visible panel output, colors,
      corner labels, reference lines, and touch activity using operator
      photos and serial diagnostics.
- [x] Confirm PSRAM initialization, 800x480 display output, GT911 ACK, and
      touch/release reporting on the physical board. Exact five-point mapping
      and measured long-duration stability remain outstanding.
- [x] Keep FluidNC UART/network command paths and all CNC motion actions out
      of this bring-up image using a JC-only source filter.

### Later integration (not Phase 1)

- [ ] Integrate the tested backend with shared graphics and scene APIs.
- [ ] Reflow reusable scenes for landscape, preserving supported target UX.
- [ ] Implement accessible touch replacements for encoder/buttons, including
      tested jog press/release safety behavior.
- [ ] Enable Wi-Fi/WebSocket for initial communication testing using
      user-configured credentials only.
- [ ] Run desktop/simulator and all supported PlatformIO environment builds;
      check existing M5Dial and CYD behavior for regressions.

## Sources and references

- User-supplied manufacturer documents: `JC8048W550 Specifications-EN .pdf`
  (SKU and product specifications, pp. 3-5) and `Getting started 5.0 Inch.pdf`
  (Arduino setup and Arduino_GFX sample guidance, pp. 2, 11-18).
- User-supplied manufacturer demo bundle:
  `1-Demo/Demo_Arduino/3_3-1_TFT_HelloWorld/HelloWorld/HelloWorld.ino`
  (RGB pins, timing, and backlight), `1-Demo/Demo_Arduino/3_3-4_TFT-LVGL-Widgets/LvglWidgets/touch.h`
  (GT911 configuration), and
  `5-IO pin distribution/5.0 inches IO pin distribution.xlsx` (connector and
  GPIO distribution).
- Pinned bring-up libraries:
  [Arduino_GFX 1.2.9](https://github.com/moononournation/Arduino_GFX/tree/20d9ad25557e6d07668ad2192395a6058092af1c)
  and
  [TAMC_GT911 1.0.2](https://github.com/TAMCTec/gt911-arduino/tree/b3f175e65a799368be9c544e255204e1e74ad2ed).
- Upstream project, build configuration, and license:
  [FluidDial repository](https://github.com/bdring/FluidDial),
  [platformio.ini](https://github.com/bdring/FluidDial/blob/47f8489dd49b8b8446c7d3a5fb6c5ba5d2fc2e9f/platformio.ini),
  [GPL-3.0 license](https://github.com/bdring/FluidDial/blob/47f8489dd49b8b8446c7d3a5fb6c5ba5d2fc2e9f/LICENSE).
- Board-family lead (third-party, not manufacturer confirmation):
  [JC8048W550 ESPHome configuration and hardware notes](https://github.com/baget/JC8048W550-ESPHome).
- Variant/flash/PSRAM lead (third-party configuration, not manufacturer
  confirmation):
  [JC8048W550C_I board config](https://github.com/MaxGramser/homeassistant_espscreen/blob/80f6fb793e33ffdac4a93029331b5bec2fb6b414/packages/boards/guition-jc8048w550c-i.yaml).
- Board/revision lead and pin/timing data (third-party; the source itself
  flags some wiring as unverified):
  [Tactility Guition JC8048W550C device definition](https://github.com/TactilityProject/Tactility/blob/0e276da2076950de4370162ee5cf60194d99737d/Devices/guition-jc8048w550c/guition%2Cjc8048w550c.dts).
- RGB framebuffer modes, memory and bounce-buffer considerations:
  [ESP-IDF RGB LCD peripheral documentation](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/lcd/rgb_lcd.html).
