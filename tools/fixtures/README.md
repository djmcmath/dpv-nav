# Frame fixtures — captured hardware ground truth

`frame_fixture.csv` and `frame_fixture_cal.json` are the recorded output of the
`axis_test` serial command: what the sensors actually reported with the unit
held in a set of known physical attitudes.

**These are measurements, not derived values.** Nothing in this directory should
ever be regenerated from a formula, hand-edited to make a test pass, or
"corrected." If a check against them fails, the code is wrong, or the capture
was bad and needs re-taking on hardware. That is the only reason they are worth
checking in — every other orientation test in this repo is synthetic and can
only prove the code is self-consistent.

## Files

| File | What it is |
|---|---|
| `frame_fixture.csv` | one row per pose: the ground-truth heading/pitch/roll the operator held, and the averaged raw logical-frame mag counts, calibrated accel (g), and calibrated gyro (rad/s) recorded there. `verified` is 1 when `classifyAccelOrientation()` confirmed the pose; `still` is 1 when the unit was not moving during the capture. |
| `frame_fixture_cal.json` | the mag bias and soft-iron matrix **in effect at capture time**. The counts in the CSV are meaningless without it, and a fixture that silently picks up whatever calibration happens to be installed when it is replayed is not ground truth. |

## Re-capturing

Run `axis_test` over serial on the nav device and save the two blocks it prints
between the `---8<---` markers. Do it away from steel furniture, speakers, and
laptops, with a real baseline calibration installed.

Re-capture when the sensors physically move on the board, when the `AxisMap`s in
`nav_main.cpp` change, or when the mag calibration is redone. A re-capture
retires the previous fixture — the old one is not ground truth for the new
geometry, so replace it rather than keeping both.

Poses aim at bin *centres* (bearings 015/105/195/285, roll 0/±90/180, pitch
0/±45/±90) so that ordinary aiming error never changes which cell a pose belongs
to. Due north would sit exactly on the sector 11 / sector 0 boundary.
