# JC page renderings (host)

These are **renderings of the real page code**, not photographs of the panel. They are produced by
`python tools/run_ui_preview.py --png <dir>`, which compiles the real JC page code and fonts against a
host stub of the display, drives the pages with simulated touches and simulated controller state (the
accepted 2026-10-30 real-controller read, replayed through the real capability loader), and checks every
frame for clipping and overlap.

What they show: layout, wording, which controls are present and in what state.
What they cannot show: the colours and brightness of the physical panel, touch response, or anything
about the real controller. The operator's photographs remain the real visual check.

`live_*` = the experimental live-jog build's UI (`JC_LIVE_JOG_TEST=1`). `ordinary_*` = the ordinary
firmware's UI. Nothing here was transmitted to any controller.

`preview_files_*`, `preview_tools_*`, `preview_toolref_*`, `preview_toolmeasure_*`, `preview_toolchange_*` and
`preview_ble_*` (Instruction 010) show the SD files and job page, the Tools page, the guided tool routines and
the Bluetooth LE page of the ordinary image (`preview_` = the ordinary firmware with the simulated controller).
The file list in those frames is a SIMULATED card; the BLE devices are a MOCK radio's.
