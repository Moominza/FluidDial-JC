# GT911 Library

Vendored TAMC_GT911 1.0.2 from
https://github.com/TAMCTec/gt911-arduino at commit
`b3f175e65a799368be9c544e255204e1e74ad2ed`.

The original Apache-2.0 `LICENSE` is retained. Local changes:

- Keep the interrupt pin signed so `-1` continues to mean not connected, and
  skip interrupt-pin GPIO operations when it is negative. The JC8048W550C_I
  has no connected GT911 interrupt; touch is polled with `read()`.
- Preserve the unmodified sensor coordinates in `TP_Point::rawX` and
  `TP_Point::rawY` before applying the existing rotation transform.

The JC-specific coordinate clamp is applied by
`src/jc8048w550/HardwareBringup.cpp`, not by changing the driver's rotation
or calibration.
