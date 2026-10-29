# JC feature status matrix

Legend: **Verified** = operator-confirmed on hardware. **Uploaded** = built and
flashed to the JC, not yet operator-confirmed. **Unit-tested** = host tests
only. **Designed** = code exists, not wired to the UI. **Not started**.
A successful compile is never counted as hardware verification.

| Area | Status | Evidence / blocker |
| --- | --- | --- |
| Display, GT911 touch, calibration, NVS, Roboto fonts | Verified | Phase 2A acceptance (15-30 min stable) |
| Four-axis dashboard, DEMO mode | Verified | Phase 2A |
| Wi-Fi settings, keyboard, AP scan / BSSID pin | Uploaded | Operator saw LIVE / FluidNC connected / Idle (photo, 2026-10-29); keyboard and scan still need confirmation |
| Read-only live status (state, X/Y/Z/A, MPos/WPos) | Uploaded | Photo showed connected and Idle with all axes 0.000. Live motion tracking, A axis and reconnect not yet tested |
| Status parser (4-axis, WCO, FS, Pn, malformed) | Unit-tested | `test/host/test_jc_core.cpp` |
| Machine/work conversion | Unit-tested | Refuses when WCO unknown |
| Command generation: jog, home, WCS, zero, probe sequence, file, macro | Unit-tested | Pure functions; not wired to any transmit path |
| Command authorization gate | Unit-tested; wired to DEMO jog as preview | Shipping build blocks everything; log line `preview: [...] NOT SENT` |
| Jog increments incl. 50/100 mm | Uploaded | DEMO only; sends nothing |
| Simulated FluidNC server | Tool | `tools/sim_fluidnc_server.py`; firmware not yet run against it |
| Dashboard: work coordinate system, feed/spindle, alarm indicator | Not started | Parser extracts the data; UI not built |
| Jogging UI with feeds, continuous jog, limits | Designed | Builder only; transmission disabled |
| Homing / probing / offsets / tool workflows UI | Designed | Builders only; no screens, no confirmation dialogs |
| SD file browser, job start/pause/resume/cancel, progress | Not started | Upstream `FileParser`/`FileMenu` are not compiled for JC |
| Macro browser and execution | Not started | Upstream `MacroMenu` not compiled for JC |
| Wired UART transport | Not started | `TransportKind::WiredUart` placeholder only; pins undocumented |
| Bluetooth LE transport | Not started | FluidNC exposes no BLE GATT service; no compatible controller side |
| Transport selection under Settings | Partial | UI lists transports; only Wi-Fi works |
| Settings: brightness, precision, diagnostics, About | Not started / partial | Wi-Fi and calibration exist |
| Machine capability discovery (`$/axes/*`) | Not started | Required before enabling motion |
| Physical CNC command transmission | Disabled by design | `JC_MACHINE_CONTROL_ENABLED=0`; needs safety review and owner approval |
