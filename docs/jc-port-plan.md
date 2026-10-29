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

### Phase 2A — upstream architecture findings and integration boundary

The original application is built around a global `Scene*` and a scene stack
in `Scene.cpp`. Scenes receive state, DRO, touch, button, encoder, and polling
callbacks. `dispatch_touch()` depends on the M5 touch event-state API and
routes screen regions to legacy buttons/encoder before forwarding scene touch
events. This provides reusable interaction semantics, but it is coupled to
the upstream `m5::Touch_Class` and 240-pixel CYD/M5 layouts.

Rendering uses `Drawing.h` helpers backed by a global `LGFX_Device` and
`LGFX_Sprite`. `SystemArduino.cpp::init_system()` unconditionally creates a
240x240 canvas; `Hardware2432.cpp` and `HardwareM5Dial.cpp` supply separate
hardware implementations, panel setup, touch/button handling, and display
layout. The upstream scene assets and coordinates therefore cannot simply be
scaled to the 800x480 RGB panel. The JC image currently uses Arduino_GFX and
has no `LGFX_Device` or shared scene implementation.

The shared CNC model in `FluidNCModel.h/.cpp` exposes controller state,
positions, parser callbacks, `send_line()`, `send_jog_line()`, and
`send_jog_cancel()`. `MultiJogScene.cpp` and `ProbingScene.cpp` invoke those
command APIs directly; they are inappropriate for a mock-only demo until a
separately reviewed safe interaction layer is integrated.

`SystemArduino.cpp` routes the existing `fnc_putchar()`/`fnc_getchar()` byte
stream according to the saved `TransportMode`: wired UART, Wi-Fi/Telnet
(despite legacy WebSocket naming), or ESP-NOW. `FileParser` and the pinned
GrblParser provide the existing line/protocol and machine-state logic. There
is no Bluetooth LE transport. `TransportScene.cpp` allows manual selection
between UART, Wi-Fi, and optionally ESP-NOW, and a mode change can shut down
Wi-Fi and reboot the controller. These runtime paths must not be pulled into
the mock JC target, which must not connect or send command bytes.

**Phase 2A decision:** keep the JC source filter isolated from all upstream
scenes and CNC/protocol implementations. Build the first landscape view
directly on Arduino_GFX, reusing the FluidDial branding, axis/state concepts,
and familiar transport labels. Add a transport-independent byte-stream
contract plus explicit Wi-Fi/WebSocket, wired UART, and BLE selection IDs,
but do not instantiate a transport in the demo. When a later target integrates
the real controller protocol, an adapter can bind that contract to the
existing `fnc_putchar()`/`fnc_getchar()` and FileParser/GrblParser logic
without duplicating the protocol; physical jogging remains outside Phase 2A.

The touchscreen-first home screen will use a 4-axis DRO row for X/Y/Z/A,
prominent DEMO and IDLE indicators, connection/transport status, selected
increment, large axis and increment selectors, discrete local-only demo jog
controls, a visible STOP/CANCEL control, and Settings/Connection navigation.
Touch actions are processed once on a new single-touch transition, use
bounds-checked display coordinates, and update only local mock state.
Settings expose manual transport selection as a configuration choice; that
selection does not initiate a connection.

#### Phase 2A implementation and verification

The Phase 1 test pattern was replaced by a JC-only `PendantUi` rendered
directly through the existing Arduino_GFX RGB display. The home view displays
X/Y/Z/A simultaneously with local mock coordinates `125.450`, `-42.100`,
`18.250`, and `90.000`; X/Y/Z use `mm`, and A uses `°`. Axis selection is
independent. Linear step choices are 0.1/1.0/10 mm and A-axis choices are
0.1/1.0/10 degrees. Discrete demo jog controls change only the selected
in-memory axis and the STOP/CANCEL button is inert apart from showing
feedback. The screen identifies DEMO and CNC OFFLINE.

Settings navigate to connection options. The connection page permits manual
selection of Wi-Fi/WebSocket, wired UART, or Bluetooth LE, but selection is
volatile and does not start a connection. `JcTransport.h` defines a
transport-neutral byte-stream interface and the three transport identities;
it has no implementations or controller adapter in this phase. The existing
FluidDial line parser/protocol logic is identified for later reuse, but is
not compiled into this mock image. Bluetooth is only represented as a future
selection; no BLE code is present.

The first operator report described roughly 10-second control delays and
freezing after repeated touches. Code inspection found that every selection
and local jog repainted the entire screen, while jog feedback expiry also
redrew the whole home view. Page transitions still redraw the full page;
selection redraws only the two affected axis tiles and controls, and a jog
redraws only the selected DRO value band, feedback button, and status strip.
The value band is 167x48 pixels rather than repainting the full 183x101 tile.
Changing a touch coordinate logs the raw/rotated/bounded diagnostic at most
once per 100 ms, while logging the initial touch and every release; each
touch action logs its synchronous processing time in milliseconds. This is a
code-level optimization only: responsiveness improvement has not yet been
confirmed by a post-upload operator test, and no benchmark result is claimed.

#### Typography update

The initial JC dashboard used Arduino_GFX's built-in 5x7 bitmap font with
integer `setTextSize()` enlargement (sizes 1, 2, and 3). Enlarging those
bitmap pixels caused the reported blocky appearance. The replacement uses
Roboto Regular/Medium/SemiBold for labels and Roboto Mono Regular for DRO
digits. The font originals and their SIL Open Font License 1.1 texts are in
`assets/fonts/`; `tools/generate_jc_fonts.py` reproducibly creates the
checked-in glyph header from them.

Glyphs are rasterized separately at each size as 4-bit grayscale masks, then
blended into RGB565 color and sent to Arduino_GFX at native pixel size. This
is anti-aliased raster text, not enlarged small glyphs or an LVGL runtime font.
Roboto Mono's fixed-width digits provide tabular numerals. The hierarchy is
16 px secondary text, 20 px controls/units, 24 px headings, and 30 px normal
DRO values. A value can use a separate pre-rasterized 28..18 px Roboto Mono
size when needed to fit the 167 px DRO value band. The band keeps each unit
on the same baseline, immediately after its value; X/Y/Z show `mm`, and A
shows `°`. The 18 px fallback fits signed values through `-999999.999` plus
the 20 px `mm` unit without clipping.

Generated glyph bitmap data is 123,405 bytes across 11 fixed sizes; glyph
metrics add about 8.5 KB in flash. The renderer uses one 40x40 RGB565 glyph
scratch buffer (3,200 bytes of static internal RAM); it allocates no PSRAM.
PlatformIO reports 23,088/327,680 bytes RAM (7.0%) and 459,541/3,342,336
bytes of application flash (13.7%). The complete upload image is 459,904
bytes. These are build-time resource figures; no runtime frame-rate or
perceived-latency benchmark has been recorded.

The dashboard jog path now clears and redraws only the value/unit band rather
than repainting its entire DRO tile. This also addresses the repeated-press
code path implicated by the operator's report that only the first press
felt quick. The firmware's touch-action timing remains available in serial
diagnostics, but the improvement still needs operator confirmation.

| Phase 2A deliverable | Result |
| --- | --- |
| Hardware baseline commit | `08dc0f4` (`Add JC8048W550C_I hardware bring-up target`), local feature branch; not pushed |
| Four-axis landscape mock UI | Implemented; prior operator photo shows X/Y/Z/A together, A in degrees, DEMO/CNC OFFLINE, jog controls |
| Touch/navigation | Implemented for dashboard, setup, connection selection, axis/step selection, local +/- changes, and STOP/CANCEL; touch action is one-shot per contact |
| Transport design | ByteTransport contract and Wi-Fi/WebSocket, UART, BLE selection identities; no transport is instantiated or connected |
| CNC motion | Not included; mock jog only changes local position values |
| JC build | PASS: final `pio run -e jc8048w550`; 7.98 seconds; 459,541 bytes app flash and 23,088 bytes RAM |
| Existing target builds | PASS: `m5dial` 115.56 seconds and `cyddial` 112.39 seconds after the typography code was integrated; neither target source/config was changed |
| JC-only upload | PASS: final image flashed on COM3; ESP32-S3 identified; firmware and partition hashes verified |
| Post-upload boot | PASS: 16 MB flash, PSRAM 8,386,279 bytes, 8,386,035 bytes PSRAM free, internal heap 363,708 bytes; ST7262 800x480 initialized; GT911 `0x5D` ACK; demo/offline startup |
| Typography visual review | PENDING: successful build/upload/boot do not establish legibility; operator photo requested |
| Repeated-press responsiveness | NOT YET VERIFIED by operator; no latency benchmark claimed |
| Long-run stability on Phase 2A image | NOT YET VERIFIED |

Phase 2A files added/updated after the Phase 1 baseline commit are
`src/jc8048w550/JcPendantUi.cpp`,
`src/jc8048w550/JcPendantUi.hpp`,
`src/jc8048w550/JcTransport.h`, and
`src/jc8048w550/HardwareBringup.cpp`. The JC-only source filter continues to
exclude the upstream scenes, controller parser, Wi-Fi implementation, UART
controller driver, and all CNC motion paths. The original `m5dial`,
`cyddial`, and default PlatformIO targets are unchanged.

**Known issues and remaining acceptance:** The user-reported sluggishness
prompted the partial-redraw optimization, but its physical response time still
needs confirmation on the newly uploaded image. Touch raw/mapped serial values
at all five named points and long-duration stability remain open Phase 1C
acceptance items. The Phase 2A connection-selection page is a local design
prototype; its setting is not persisted and the transports are not
implemented. No photo of the post-optimization interaction or operator
confirmation of the responsiveness fix has been received yet.

**Recommended Phase 2B scope:** first verify responsiveness, touch mapping,
and state retention on hardware. Then extract a transport-independent
controller/protocol adapter around the existing FluidDial parser/model,
define explicit selection persistence and connection lifecycle/error states,
and implement a read-only CNC status connection for one approved transport.
Keep all jog/probe command paths disabled until separately designed and
reviewed with physical safety interlocks and a simulated/disconnected test.
Do not implement BLE before a distinct authorization and design review.

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

## Phase 2A — guided touch calibration

The JC demo now opens the five-point calibration page on first boot when no
valid mapping exists, before showing the dashboard. It presents one target at
a time in this order: top-left, top-right, bottom-right, bottom-left, and
centre. Corner targets are 60 px inward from the panel edges. The existing
GT911 rotation, reset configuration, RGB pins, and normal mapping are
preserved until a passing result is explicitly saved.

Each touch attempt requires nine single-touch polling samples and a confirmed
release. Samples are reduced to per-axis medians; an attempt is rejected if
any raw or rotated-driver coordinate differs from its median by more than
20 px, if fewer than nine samples arrive, or if multi-touch is detected. Serial
diagnostics retain raw GT911, rotated driver, current mapped UI, target,
candidate mapped UI, and fit-error values.

The four corners derive a six-coefficient affine mapping, which can represent
axis swapping, inversion, scale, and offsets. The centre is excluded from that
fit and used as an independent accuracy check. The on-screen result reports
maximum/average corner error, centre error, axis swap, and inversion. A fit is
eligible to save only when maximum corner error is at most 35 px and centre
error is at most 30 px. The candidate does not affect touch handling until
Save Calibration is selected.

Accepted mappings are stored in ESP32 NVS under namespace `jctouch`, key
`map`, with schema version 1 and finite/range/determinant checks on load.
Failed saves leave the active mapping unchanged. Setup offers a confirmed
factory reset. Recovery is available independent of mapped coordinates by
holding one finger anywhere on the screen for five seconds, provided the hold
starts within ten seconds of boot; release, then begin target 1. Once a valid
mapping is saved, the dashboard is the normal startup page and calibration
remains available from Setup.

| Validation item | Result |
| --- | --- |
| `pio run -e jc8048w550` | PASS at Phase 2A implementation; final acceptance rebuild on 2026-10-29 passed in 6.12 s; 480,697 bytes app flash and 23,492 bytes RAM |
| `pio run -e m5dial`, then `pio run -e cyddial` | PASS sequentially on 2026-10-29; M5Dial 16.47 s, CYDDial 14.57 s; JC-only source remains excluded from both |
| `git diff --check` | PASS on 2026-10-29 |
| JC-only upload to COM3 | PASS; ESP32-S3 identified and firmware/partition hashes verified |
| Final serial boot | PASS; ESP32-S3 revision 0, 16 MB flash, PSRAM initialized (8,386,279 bytes), 8,386,035 bytes PSRAM free, internal free heap 363,112 bytes; wizard opened before dashboard because no map was saved |
| Display and touch initialization | PASS in serial diagnostics; ST7262 800x480, backlight GPIO2 HIGH, GT911 address 0x5D ACK, SDA19/SCL20/RST38/INT unconnected |
| CNC communication and motion | EXCLUDED; JC source filter remains limited to `src/jc8048w550/*.cpp`; no controller code or motion path is included |
| Five physical targets and orientation | PASS, operator logs captured three times; GT911 driver X and Y both inverted, no axis swap |
| Calibration fit | PASS on three earlier runs: max/average corner error 3.16/3.16 px, centre error 7.91 px; 2.29/2.29 px, centre 8.93 px; 6.84/6.84 px, centre 7.32 px. Additional COM3 captures on 2026-10-29: 9.55 px max/average corner error, 16.01 px centre error; and 1.38 px max/average corner error, 7.59 px centre error. Both reported X/Y inverted, no swap |
| Explicit Save and NVS write | PASS; latest COM3 capture recorded `cal: version=1 saved; map=[-1.038324 0.029125 815.862; 0.005397 -1.009969 471.359]` and `cal: accepted mapping applied; returning to settings.` An earlier acceptance-session map was `[-1.022496 0.040038 794.848; -0.001630 -1.084273 499.244]`. The Save hit-test bug was fixed by separating wizard control mapping from dashboard mapping |
| Retry and cancel wizard | PASS, operator reports both actions completed |
| Persistence after latest save | PASS; after the saved-map capture, COM3 showed a fresh ESP-ROM `rst:0x1 (POWERON)` boot and `cal: version 1 mapping loaded from NVS`, followed by dashboard startup with `saved calibration active` |
| Factory-reset cancellation | PASS; latest serial confirmed `cal: factory reset cancelled; existing mapping retained.` |
| Existing calibration backup before reset | Saved version-1 map coefficients were captured before reset: `[-1.038324 0.029125 815.862; 0.005397 -1.009969 471.359]`. The earlier map `[-1.022496 0.040038 794.848; -0.001630 -1.084273 499.244]` was confirmed loading after a fresh power-on. Preserve these captured values; recovery remains available via the five-second one-finger hold during the first 10 seconds after reboot. |
| Confirmed factory reset and recovery | PASS per operator report; prior saved map coefficients were captured before reset. COM3 evidence for the reset action itself was not present in the captured log. Recovery uses a five-second one-finger hold begun within 10 seconds of reboot, then recalibrate all five points and explicitly save. |
| Dashboard controls and visual review | PASS with evidence limits; operator dashboard photo received 2026-10-29 shows all X/Y/Z/A DROs and units, DEMO/CNC OFFLINE indicators, axis and step selectors, and local jog/STOP controls. Operator reports very responsive controls and no flicker. COM3 captured selection of Wired UART and Bluetooth LE with `connection not started`. Operator calibration PASS/RETRY photos exposed header/target remnants; the centred-header redraw correction was uploaded and operator confirmed the latest calibration page/result header look correct. A separate Setup-page photo and individual X/Y/Z/A selection log were not supplied. |
| Stability run | PASS per operator confirmation on the latest firmware: a continuous observation within the requested 15–30-minute interval completed without reported resets, lockups, display corruption, touch issues, or flicker. Exact elapsed minutes were not recorded. |
| Overall Phase 2A acceptance | PASS; operator approved the Phase 2A checkpoint on 2026-10-29 after confirming the 15–30-minute stability run. Setup-page photo and exact run duration are documentation gaps, not operator-reported defects. |

#### Phase 2A final acceptance checkpoint (2026-10-29)

The local working branch is `feature/jc8048w550`. During acceptance-session
serial capture, the operator completed two five-point runs and explicitly
saved passing maps. The second run's max/average corner error was 1.38 px and
its independent centre error was 7.59 px. The latest map is recorded above.
Reset cancellation was confirmed in serial; selection of Wired UART and
Bluetooth LE was logged without starting either connection. The previously
saved map was observed loading after reboot. The JC environment remains
opt-in and compiles only `src/jc8048w550/*.cpp`; M5Dial and CYDDial retain their
existing source filters and both rebuilt successfully after the JC build.
Review of the isolated JC sources found no controller transport or motion
implementation: transport identifiers and labels are selection-only, demo
jogging updates local coordinates, and STOP/CANCEL has no controller to stop.
Font originals retain the SIL Open Font License texts. No credentials,
firmware binaries, or temporary test output are intended project deliverables;
`.pio` build output is ignored. On 2026-10-29 the operator explicitly approved
the Phase 2A checkpoint and confirmed a continuous 15–30-minute observation
of the latest firmware without reported resets, lockups, display corruption,
touch issues, or flicker. The exact duration and a separate Setup-page photo
were not recorded.

The operator subsequently reported completing wizard Retry/Cancel and
confirmed factory reset/recovery, described the touchscreen as very
responsive with no flicker, and identified stale calibration text/target
graphics remaining between points. `JcPendantUi.cpp` now clears the label and
progress bands and previous target marker when advancing, and clears the body
when showing the result, avoiding a full-area wipe on each step. Operator
photos showed the PASS and RETRY result pages but also revealed target-ring
graphics intruding into the header. The calibration page now uses a compact
centred header clear of the corner targets and redraws that header after
clearing the result page. The final JC rebuild passed on 2026-10-29 (480,909
bytes app flash, 23,492 bytes RAM); upload to COM3 passed with verified flash
hashes. Post-upload serial confirmed ESP32-S3 boot, PSRAM, 800x480 ST7262,
GT911 ACK, and version-1 calibration loaded from NVS. The operator confirmed
that the latest target transitions and result header look correct.

## Phase 2B — read-only FluidNC status over Wi-Fi (implementation in progress)

The pinned upstream Wi-Fi client is not a WebSocket client: its FluidNC
connection is raw Telnet-style TCP on port 23. The JC target therefore labels
the transport as Telnet TCP and does not compile the upstream shared
`WiFiConnection.cpp`, whose general-purpose transmit path can send
machine-changing commands and other control bytes.

The JC-only `JcReadOnlyFluidNc` client uses a manual connect action, stores
SSID/password/host in local Preferences NVS, performs DNS lookup away from the
UI polling loop, and polls the TCP socket with bounded work. It accepts a
bounded status frame only after validating a recognized machine state, exactly
four finite MPos or WPos values, and the parser-required FS pair. The pinned
GrblParser then supplies the four-axis status callbacks. The displayed
coordinate reference is retained as MACHINE or WORK rather than converted
implicitly. A stale connection retains its last valid live values but clearly
marks them stale; LIVE mode never substitutes simulated positions.

All socket output is guarded by `parser_putchar()`: only the single `?` status
query is sent. Any other byte is blocked and counted. The source has no
G-code, jog, stop, probe, reset, cycle-start, or hold send path. The JC source
filter remains limited to `src/jc8048w550/*.cpp`; M5Dial/CYDDial source filters
are unchanged. Their builds were rerun below as regression checks.

The Wi-Fi page is reached only by selecting Wi-Fi in Connection Options.
It provides a touch keyboard for SSID, masked password, and host/IP entry.
Tapping any field explicitly opens and redraws the keyboard. An asynchronous
Wi-Fi scan displays SSID, signal strength, channel, and BSSID for each access
point; selecting a row pins that BSSID and channel in local Preferences, and
the ESP32 connection/retry calls pass the selected BSSID to `WiFi.begin()`.
Manual SSID editing clears the pin. NVS Save and network Connect are explicit
actions, with no startup auto-connect. The dashboard can switch to LIVE only after a fresh valid
four-axis report. Axis/increment selection remains local, while jog and
STOP/CANCEL controls are visibly disabled and guarded in LIVE mode. Live
positions update incrementally instead of redrawing the entire dashboard.

Operator feedback found that the Wi-Fi page's keyboard was not appearing on
field edit, and the long read-only warning text on the Connection Options
screen overlapped. Field selection now forces a page redraw with the keyboard
visible; connection warning copy is shortened and centered to fit. Hardware
visual verification of these fixes and the new AP scan/BSSID selection is
pending.

Credentials are not logged or committed, but this prototype does not enable
NVS encryption. The FluidNC status connection uses unencrypted TCP on the
local network and has no authentication in this client; use only on a trusted
LAN. No credentials have been entered, and no FluidNC connection or CNC
command has been made during this implementation/build session.

| Phase 2B verification item | Result |
| --- | --- |
| `pio run -e jc8048w550` | PASS after keyboard/scan/BSSID changes; 950,941 bytes app flash, 54,536 bytes RAM, 10.77 s. Compile output shows only `HardwareBringup.cpp`, `JcPendantUi.cpp`, and `JcReadOnlyFluidNc.cpp` from the application source. |
| `pio run -e m5dial` | PASS sequentially; 1,262,241 bytes app flash, 71,616 bytes RAM, 16.99 s. |
| `pio run -e cyddial` | PASS sequentially; 1,185,829 bytes app flash, 70,456 bytes RAM, 15.55 s. |
| `git diff --check` | PASS after the final three builds. |
| Socket transmit audit | PASS by source inspection: one direct `send()` call, guarded to exactly one `?`; the parser callback blocks and counts all other bytes. |
| JC upload to COM3 | PASS for prior read-only firmware on 2026-10-29 after COM3 became available. Esptool identified ESP32-S3 revision v0.2; wrote bootloader, partitions, and application to COM3 and verified each flash hash. No other device was selected. |
| Boot diagnostics after prior upload | PASS; final serial log shows ESP32-S3 revision 0, 16,777,216-byte flash, PSRAM initialized (8,386,279 bytes; free PSRAM 8,386,035 bytes), internal free heap 323,980 bytes, ST7262 800x480, GPIO2 backlight HIGH, GT911 0x5D ACK, and version-1 touch calibration loaded from NVS. Startup clearly reports DEMO by default, manual read-only status only, and motion commands disabled; auto-connect is disabled. |
| Upload of scan/BSSID and keyboard/text fixes | PASS on COM3; esptool recognized the ESP32-S3 and verified bootloader, partition table, and application flash hashes. |
| Boot of scan/BSSID and keyboard/text firmware | PASS; serial diagnostics show PSRAM/display/GT911 initialization and saved touch calibration load. No Wi-Fi credentials are configured; auto-connect remains disabled. Touch navigation to Setup was logged. |
| Startup network configuration | PASS; reports `local configuration not configured; auto-connect disabled` with no missing-key error logs. No credentials were entered. |
| Header SETUP/LINK controls | Operator photo showed the header controls missing. Root cause: a live link-state refresh redrew the full header after the controls had been drawn, covering them. The refresh now redraws SETUP and LINK after the header. JC build passed (946,869 bytes flash; 53,608 bytes RAM), upload to COM3 passed with verified hashes, and the final firmware booted with PSRAM/display/touch/calibration diagnostics. Awaiting operator visual confirmation that both buttons are visible and tappable. |
| Touch keyboard visibility, access-point scan, BSSID pin/persistence, text fit, and operator visual check | Firmware is uploaded and booted; OPERATOR TEST PENDING. |
| Live protocol / four-axis status / reconnect / stale behavior on physical FluidNC | NOT YET TESTED. |
| Credential persistence, LIVE/Demo toggle, real DRO, and stability on updated firmware | NOT YET TESTED. No credentials were entered, and no FluidNC connection has occurred. |

**Current classification: PARTIAL / implementation, builds, JC upload, and
boot verified.** Before claiming Phase 2B acceptance, have the operator test
that field taps reveal the keyboard, scan and select the intended BSSID, and
confirm warning text fits. Then enter credentials locally and confirm the
configured controller host is `192.168.0.134`. Verify four reported axes,
coordinate reference, freshness, disconnect/reconnect, and read-only behavior
without sending anything except `?`. Operator visual and stability evidence
is also still required. No push, merge, or CNC-controller operation is
authorized.

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

## Phase 2A acceptance record

The operator confirmed the Phase 2A dashboard and Setup page and reported
15-30 minutes of stable operation. Phase 2A is accepted; its checkpoint is
commit `ed9f700`.

## Phase 2B simulated FluidNC test harness (instruction 002B.1)

`tools/sim_fluidnc_server.py` is a simulated FluidNC status server. It must
listen on TCP port 23, because the JC client's port is fixed. Run it on the PC
(allow it through the firewall) and enter the PC's LAN address as the JC
"FluidNC host". It never contacts a real controller. Scenarios: `normal`
(negative values, A in degrees), `work` (WPos + WCO), `fragmented`,
`malformed`, `three-axis` (JC must reject; it requires four axes), `close`
(reconnect) and `silent` (stale). The server audits every byte received and
exits non-zero if any byte other than `?` arrives.

| Item | Result |
| --- | --- |
| Simulator self-check on loopback (`close` scenario) | PASS: `?` queries answered; connection closed after three frames as designed |
| Firmware run against simulator scenarios | NOT YET RUN. Needs the operator to enter Wi-Fi credentials on the JC and point it at the PC. |
| Automated host-side unit tests of the firmware parser | NOT DONE. `JcReadOnlyFluidNc` depends on ESP32 Wi-Fi/lwIP and has no host stubs. |
| Jog step selector | 50 mm / 100 mm added for X/Y/Z (rotary: 45 / 90 deg). Local DEMO values only; builds and uploads to COM3 passed. Visual confirmation pending. |
| Status header overlap | Subtitle moved right to avoid the header text. Visual confirmation pending. |

The firmware's only transmit path remains `parser_putchar()`, which sends
exactly one `?` byte and blocks and counts everything else. This is verified
by source inspection only. No machine motion is enabled.
