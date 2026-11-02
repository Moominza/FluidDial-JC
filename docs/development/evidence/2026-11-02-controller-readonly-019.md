# Live read-only controller check, 2026-11-02 (Instruction 019)

Same read-only HTTP queries as [2026-11-01](2026-11-01-controller-readonly-014.md); nothing was written or commanded.

- `?` -> `<Idle|MPos:101.000,100.000,0.000,0.000|FS:0,0>` (machine position is no longer 0,0; the machine has been moved since 2026-11-01, consistent with the operator's reported testing).
- `$G` -> G54 active, T0. `$#` -> G54..G59 offsets all `0.000` at the time of the query.
- Probe pin `gpio.48:low`; `toolsetter_pin=NO_PIN`; `must_home=false`.
- X: cycle 1, positive_direction=false, limit_neg `gpio.41`; Y: cycle 1, false, `gpio.40`; **Z: cycle 1, `positive_direction=true`, limit_neg `gpio.39`, limit_pos NO_PIN**; A: no homing settings, no limit pins.
- soft_limits=false and hard_limits=false on all four axes.

**The controller configuration is unchanged from 2026-10-30 / 2026-11-01.** The Z positive_direction / negative-limit-pin discrepancy is therefore still present in the configuration. The owner has reported PASS for "limit switches and homing" but supplied no observations, so this record cannot say whether Z homes correctly with that configuration, or why. It is documented as an unreconciled controller-configuration limitation; the pendant did not and does not change controller configuration.
