# JC jog preview: operator acceptance checklist (Instruction 007)

Firmware: `env:jc8048w550` (ordinary, motion disabled, non-motion jog test mode). **No motion is
possible.** The CNC may stay powered but nothing is ever sent to it except the read-only `?` status
query and the allowlisted read-only `$` queries. Do not flash the live-jog build.

Mark each line PASS / FAIL and note what you saw. Photograph the screens marked (P).

## A. Marking
1. Every page you visit shows "PREVIEW ONLY - MOTION DISABLED" (or a shorter PREVIEW wording) in the header. (P: MENU, JOGGING, SETTINGS)
2. Dashboard shows the PREVIEW banner at the top right. (P)

## B. Jog page (MENU > JOGGING), CNC connected, status LIVE
3. Select X, then Y: the selected axis is highlighted. Z shows "Z  ON HOLD" and A shows "A  DISABLED"; both jog buttons are dimmed.
4. Step buttons 10 / 50 / 100 mm are dimmed. Tap 10 mm, then JOG: the jog is REFUSED with a clear message ("Test limits: X/Y 1 mm @ 100/min only"). (P)
5. Select feed 500 or 1000 /min: a jog is refused as well.
6. Tap "ARM PREVIEW" without a capability read: refused, tells you to read the controller. Then SETTINGS > MACHINE > CAPABILITIES > READ FROM CONTROLLER, return to JOGGING.
7. Tap "ARM PREVIEW": the seven-line dialog appears; CANCEL arms nothing; "I ACKNOWLEDGE" arms X ("ARMED nnn s"). (P)
8. Tap JOG +: confirmation dialog; CANCEL sends nothing. Tap JOG + again, CONFIRM: "SIMULATED JOG - NOTHING SENT". (P)
9. Result line shows "SIM: ... ACCEPTED ...", then "SIM: COMPLETED (X): moved +1.000". The real DRO does not change.
10. Tap Y without acknowledging Y, JOG +: blocked ("not acknowledged"), asks you to ARM for that axis.
11. Tap the ARMED button while armed: it disarms (button returns to ARM PREVIEW).
12. Tap Z or A, JOG: explained refusal dialog; Z/A can never be armed.

## C. Scenarios (SETTINGS > JOG PREVIEW TEST > NEXT SCENARIO)
For each scenario arm X, JOG +, confirm, watch the jog page result line:
13. REJECTS: "SIM: REJECTED by controller (code 15)" and an ACKNOWLEDGE RESULT button. Acknowledge it.
14. ALARM: "SIM: ALARM (code 1)", acknowledge.
15. NO REPLY: after about 2 s "NO REPLY (timeout)", acknowledge.
16. SLOW ACK: result line waits, then completes.
17. IGNORES CANCEL: start a jog, tap CANCEL JOG: after about 3 s "SIM: CANCEL NOT CONFIRMED - USE THE E-STOP". (P)
18. NORMAL: start a jog, tap CANCEL JOG: "SIM: CANCELLED" after the simulated Idle.
19. JOG PREVIEW TEST page: "SENT 0" stays 0 throughout. (P)

## D. Arming and acknowledgement are cleared
Arm and acknowledge X, then:
20. leave the jog page (BACK): returning shows ARM PREVIEW again.
21. SETTINGS > CONNECTIONS > DISCONNECT: after reconnecting, ARM PREVIEW again.
22. Switch to DEMO and back to LIVE: disarmed.

## E. Wi-Fi loss (router or controller access point off for about 60 s)
23. Arm and acknowledge X. Switch the Wi-Fi source off. The header goes LIVE STALE, then RECONNECTING / WIFI CONNECTING; the jog page shows it disarmed. (P)
24. Switch it back on. The panel reconnects by itself (no touch), returns to LIVE, and arming is NOT restored (ARM PREVIEW).
25. The capability data is discarded after the loss (Capabilities page asks for a new read).

Send photographs and the pass/fail list. A serial capture of the session is recorded separately.

## F. Instruction 008 additions (ordinary, motion-disabled firmware)
27. DASHBOARD (LIVE): header row shows title, LIVE chip, link chip, blue PREVIEW banner, MENU, LINK without overlap; X Y Z A cards fully visible. No "NO JOG HERE" / "NO CANCEL HERE" buttons. Lower panel shows the six status cells, "Endpoint: host:port   Age: n ms" and the PREVIEW line. (P)
28. MACHINE / WORK button still switches the reference; all four axes show values.
29. MENU > JOGGING in LIVE with the machine Idle: the page starts one automatic read (button shows READING n/55, then ARM PREVIEW; the line under the panel says "REAL controller: Machine settings read n s ago"). (P)
30. Disconnect the controller (or Wi-Fi), reconnect: the page shows "NOT read" with READ SETTINGS; after reconnect one automatic read runs; arming is not restored.
31. Tap READ SETTINGS while the machine is not Idle (not possible to test without motion; skip) or in DEMO: a message explains why.
32. LINK page: the Wi-Fi card shows the saved endpoint and "TAP TO EDIT HOST / IP"; tapping opens the Wi-Fi page with the host field. SETTINGS > CONNECTIONS shows "saved endpoint host:port" and EDIT HOST / IP.
33. JOGGING page photo: axis row, STEP, FEED, ARM button, JOG - / CANCEL JOG / JOG +, the four status rows and the bottom warning are all readable and none overlaps. (P)
