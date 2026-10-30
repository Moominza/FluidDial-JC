# Real controller configuration read, 2026-10-30

Source: passive serial capture of the JC (COM3), ordinary read-only firmware
(`env:jc8048w550`, `JC_MACHINE_CONTROL_ENABLED=0`), connected LIVE to FluidNC v4.1.1 at
192.168.0.134:23. The operator tapped READ CONTROLLER three times; the third read is the one
shown in the operator's photograph (08:37, "50 answered, 3 refused, 2261 ms").

What this is: the firmware's own log of the **parsed result** of each 53-query read
(`caps:` lines, verbatim, in capture order). The firmware does not log each raw query / reply
line, so this is NOT a byte-level transcript of the exchange.

Not included: the rest of the serial log (touch coordinates, page changes). It was checked
and contains no credentials or addresses. It also contains no `jogtest:`, `SAFETY:` or
dispatch line: nothing but `?` and read-only queries was transmitted.

Notation: `(n)` = not queried / `error:N` = the controller refused the query with error N.
Booleans: 0 = false, 1 = true. Pins: the pin text the controller reported.

```
caps: read-only capability read started
caps: complete answered=50 refused=3 ms=2285 fw=[4.1 FluidNC v4.1.1 (esp32s3-wifi)] wcs=G54 tool=0
caps: axis X limits_confirmed=1 envelope_read=1 steps_per_mm=40 max_travel_mm=1000 max_rate_mm_per_min=6000 acceleration_mm_per_sec2=250 homing/cycle=1 homing/positive_direction=0 soft_limits=0 motor0/hard_limits=0 motor0/limit_neg_pin=gpio.41:low motor0/limit_pos_pin=NO_PIN motor0/limit_all_pin=NO_PIN homing/mpos_mm=0
caps: axis Y limits_confirmed=1 envelope_read=1 steps_per_mm=40 max_travel_mm=2300 max_rate_mm_per_min=6000 acceleration_mm_per_sec2=250 homing/cycle=1 homing/positive_direction=0 soft_limits=0 motor0/hard_limits=0 motor0/limit_neg_pin=gpio.40:low motor0/limit_pos_pin=NO_PIN motor0/limit_all_pin=NO_PIN homing/mpos_mm=0
caps: axis Z limits_confirmed=1 envelope_read=1 steps_per_mm=200 max_travel_mm=150 max_rate_mm_per_min=1500 acceleration_mm_per_sec2=100 homing/cycle=1 homing/positive_direction=1 soft_limits=0 motor0/hard_limits=0 motor0/limit_neg_pin=gpio.39:low motor0/limit_pos_pin=NO_PIN motor0/limit_all_pin=NO_PIN homing/mpos_mm=0
caps: axis A limits_confirmed=1 envelope_read=1 steps_per_mm=1000 max_travel_mm=3000 max_rate_mm_per_min=1000 acceleration_mm_per_sec2=25 homing/cycle=error:3 homing/positive_direction=error:3 soft_limits=0 motor0/hard_limits=0 motor0/limit_neg_pin=NO_PIN motor0/limit_pos_pin=NO_PIN motor0/limit_all_pin=NO_PIN homing/mpos_mm=error:3
caps: probe_pin=[gpio.48:low] toolsetter_pin=[NO_PIN] must_home=false
caps: read-only capability read started
caps: complete answered=50 refused=3 ms=1955 fw=[4.1 FluidNC v4.1.1 (esp32s3-wifi)] wcs=G54 tool=0
caps: axis X limits_confirmed=1 envelope_read=1 steps_per_mm=40 max_travel_mm=1000 max_rate_mm_per_min=6000 acceleration_mm_per_sec2=250 homing/cycle=1 homing/positive_direction=0 soft_limits=0 motor0/hard_limits=0 motor0/limit_neg_pin=gpio.41:low motor0/limit_pos_pin=NO_PIN motor0/limit_all_pin=NO_PIN homing/mpos_mm=0
caps: axis Y limits_confirmed=1 envelope_read=1 steps_per_mm=40 max_travel_mm=2300 max_rate_mm_per_min=6000 acceleration_mm_per_sec2=250 homing/cycle=1 homing/positive_direction=0 soft_limits=0 motor0/hard_limits=0 motor0/limit_neg_pin=gpio.40:low motor0/limit_pos_pin=NO_PIN motor0/limit_all_pin=NO_PIN homing/mpos_mm=0
caps: axis Z limits_confirmed=1 envelope_read=1 steps_per_mm=200 max_travel_mm=150 max_rate_mm_per_min=1500 acceleration_mm_per_sec2=100 homing/cycle=1 homing/positive_direction=1 soft_limits=0 motor0/hard_limits=0 motor0/limit_neg_pin=gpio.39:low motor0/limit_pos_pin=NO_PIN motor0/limit_all_pin=NO_PIN homing/mpos_mm=0
caps: axis A limits_confirmed=1 envelope_read=1 steps_per_mm=1000 max_travel_mm=3000 max_rate_mm_per_min=1000 acceleration_mm_per_sec2=25 homing/cycle=error:3 homing/positive_direction=error:3 soft_limits=0 motor0/hard_limits=0 motor0/limit_neg_pin=NO_PIN motor0/limit_pos_pin=NO_PIN motor0/limit_all_pin=NO_PIN homing/mpos_mm=error:3
caps: probe_pin=[gpio.48:low] toolsetter_pin=[NO_PIN] must_home=false
caps: read-only capability read started
caps: complete answered=50 refused=3 ms=2261 fw=[4.1 FluidNC v4.1.1 (esp32s3-wifi)] wcs=G54 tool=0
caps: axis X limits_confirmed=1 envelope_read=1 steps_per_mm=40 max_travel_mm=1000 max_rate_mm_per_min=6000 acceleration_mm_per_sec2=250 homing/cycle=1 homing/positive_direction=0 soft_limits=0 motor0/hard_limits=0 motor0/limit_neg_pin=gpio.41:low motor0/limit_pos_pin=NO_PIN motor0/limit_all_pin=NO_PIN homing/mpos_mm=0
caps: axis Y limits_confirmed=1 envelope_read=1 steps_per_mm=40 max_travel_mm=2300 max_rate_mm_per_min=6000 acceleration_mm_per_sec2=250 homing/cycle=1 homing/positive_direction=0 soft_limits=0 motor0/hard_limits=0 motor0/limit_neg_pin=gpio.40:low motor0/limit_pos_pin=NO_PIN motor0/limit_all_pin=NO_PIN homing/mpos_mm=0
caps: axis Z limits_confirmed=1 envelope_read=1 steps_per_mm=200 max_travel_mm=150 max_rate_mm_per_min=1500 acceleration_mm_per_sec2=100 homing/cycle=1 homing/positive_direction=1 soft_limits=0 motor0/hard_limits=0 motor0/limit_neg_pin=gpio.39:low motor0/limit_pos_pin=NO_PIN motor0/limit_all_pin=NO_PIN homing/mpos_mm=0
caps: axis A limits_confirmed=1 envelope_read=1 steps_per_mm=1000 max_travel_mm=3000 max_rate_mm_per_min=1000 acceleration_mm_per_sec2=25 homing/cycle=error:3 homing/positive_direction=error:3 soft_limits=0 motor0/hard_limits=0 motor0/limit_neg_pin=NO_PIN motor0/limit_pos_pin=NO_PIN motor0/limit_all_pin=NO_PIN homing/mpos_mm=error:3
caps: probe_pin=[gpio.48:low] toolsetter_pin=[NO_PIN] must_home=false
```
