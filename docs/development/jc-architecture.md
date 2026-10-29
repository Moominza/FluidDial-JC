# JC application architecture

All JC code lives in `src/jc8048w550/` and is compiled only by the
`jc8048w550` environment; `m5dial` and `cyddial` are unaffected.

| Concern | Files |
| --- | --- |
| Hardware / display / touch bring-up | `HardwareBringup.cpp`, `JcTouchCalibration.*`, `JcFontRenderer.*`, `JcFonts.generated.hpp` |
| Dashboard, calibration, Wi-Fi page (legacy shell) | `JcPendantUi.*` |
| Application shell, navigation, settings I/O, backlight | `JcApp.*`, `JcNav.hpp` (pure) |
| Pages (one file each) | `JcScreen.*` (base, staged command flow), `JcScreenMenu/Jog/Homing/Probing/Offsets/Files/Macros/Tools/Settings/Connections/Diagnostics/Input.cpp`, declarations in `JcScreens.hpp` |
| UI primitives and dialogs | `JcUiKit.*` |
| Machine state | `JcMachineModel.hpp` (pure), `JcLinkTypes.hpp` (pure) |
| CNC protocol | `JcStatusFrame.hpp`, `JcFrameAssembler.hpp` (pure; GrblParser from the pinned upstream library is still used by the Wi-Fi link) |
| Transport | `JcReadOnlyFluidNc.*` (Wi-Fi TCP), `JcUartLink.*` (UART), `JcLinkTracker.hpp` (pure), `JcTransport.h` |
| Command generation | `JcCommandBuilder.hpp` (pure) |
| Authorization | `JcCommandAuth.hpp`, `JcDispatch.hpp` (pure) |
| Controller replies / queries | `JcResponse.hpp`, `JcStreamSplitter.hpp`, `JcQuery.hpp` (allowlist), `JcCommandExec.hpp` (ack/timeout), all pure |
| Capabilities | `JcMachineCaps.hpp` (validation, profile), `JcCapsLoader.hpp` (query sequence), pages `JcScreenCapabilities.cpp`, `JcScreenDryRun.cpp`, `JcDryRun.hpp` |
| Sequenced sending (host tests only) | `JcCommandLink.hpp` (sink + reply tracking), `JcDispatch.hpp` (`advance_sequence`) |
| Persistent settings | `JcSettings.hpp` (pure encode/validate), stored by `JcApp` in NVS namespace `jc_app` |

## Command flow

Page builds a `JcCommand` -> preview + confirmation dialog (`JcScreen::stage*`)
-> `JcDispatcher` -> `JcCommandGate` -> (no sink) -> blocked result dialog.
Pages never touch a socket or UART. See `jc-command-safety.md`.

## Reuse of upstream FluidDial

* Command formats mirror upstream `MultiJogScene`, `ProbingScene`,
  `HomingScene`, `FilePreviewScene`, `MacroMenu`.
* `GrblParser` (pinned) parses Wi-Fi status frames.
* The upstream scenes, `FluidNCModel`, `FileParser` and `WiFiConnection` are
  not compiled for the JC: they are built around the encoder/CYD hardware and
  a general transmit path. Their logic was reused as formats and rules, not
  linked, to keep the JC read-only.

## Testing

`python tools/run_host_tests.py` (unit tests, shipping policy and with the
authorization rules compiled in) and `python tools/run_sim_integration.py`
(real TCP session with `tools/sim_fluidnc_server.py` feeding the real JC
assembler/parser/model/tracker). Both run in CI. Neither runs the ESP32
firmware itself.
