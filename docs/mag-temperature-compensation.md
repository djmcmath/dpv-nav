# Magnetometer Temperature Compensation

The LIS3MDL's readings drift with its own die temperature. On this unit the drift is about
**0.68 µT per °C in the horizontal plane**. With a properly centred calibration that moves
heading by up to **~2.2° per °C**, depending on which way the scooter points. A 10 °C change
between calibration and dive can therefore cost ~20° of heading. Recalibrating can't fix it,
because the error depends on temperature and not on the unit's attitude or surroundings.

The firmware corrects the **magnetic readings**, not the heading. It subtracts a measured
coefficient × (die temperature − 21 °C) from every raw sample before any calibration runs.
The coefficients live in `/mag_temp.json`. Without that file nothing changes.

## What the manufacturer publishes

Nothing usable. Pololu's [AltIMU-10 v5 page](https://www.pololu.com/product/2739) has no
temperature information, and defers to ST's documents:

- **[LIS3MDL datasheet](https://www.pololu.com/file/0J1089/LIS3MDL.pdf)** (DocID024204 Rev 4),
  Table 3 "Magnetic characteristics". It gives a zero-gauss level of **±1 gauss typical**
  (±100 µT) at 25 °C and **no temperature coefficient for that offset**. There is no
  offset-drift or sensitivity-drift row at all. Table 4 covers the temperature sensor only:
  8 LSB/°C, refreshed at the magnetic ODR, −40 to +85 °C.
- **[AN4602](https://www.pololu.com/file/0J1090/LIS3MDL-AN4602.pdf)** (DocID027014 Rev 1),
  §6. It calls the temperature sensor "suitable for delta temperature measurement", nominally
  "8 LSB/°C and 0 output means T=25 °C". It says "the sensitivity of the magnetic sensor
  changes when the temperature changes. A temperature compensation digital block is introduced
  to compensate for the effect of temperature". The introduction says the device internally
  compensates "sensitivity drift over temperature variations". **Only sensitivity (gain) is
  claimed to be compensated on-chip.** Offset drift isn't mentioned.

So the offset drift is unspecified, not compensated by the part, and has to be measured per
unit. The ±1 gauss absolute offset is removed by the hard-iron calibration. Its *drift* is
what this document is about.

## Measurement (2026-09-13/14)

**Procedure.** The scooter sat stationary and level, pointed at a cardinal heading by the
unit's magnetic compass, logging at MID. After ~15 s of baseline, a heat gun warmed the nav
board for 45–60 s, and the unit was then left untouched to cool for 30–60 min. Logs are in
`baseline cal jsons/thermal testing/`. West was done twice, the second time at a different
location. `other south.csv` is a 7-minute unheated control taken after a reboot.

**Findings:**

| log | die range on cooling leg | x µT/°C | y µT/°C | z µT/°C |
|---|---|---|---|---|
| north | 21.3–47.9 °C | 0.477 | 0.396 | 0.293 |
| east | 23.6–44.4 °C | 0.490 | 0.449 | 0.261 |
| south | 23.6–61.3 °C | 0.514 | 0.418 | 0.231 |
| west1 | 26.0–54.4 °C | 0.506 | 0.425 | 0.320 |
| west2 | 24.6–47.9 °C | 0.498 | 0.417 | 0.286 |
| **pooled** | | **0.501** | **0.421** | **0.273** |

These slopes are in the calibrated frame (the logs' `mag_*_cal`). In the raw frame the
firmware corrects in, they come to **(0.537, 0.417, 0.278) µT/°C**. The next section explains
why the frame matters.

- **The drift is fixed to the body.** Every heading gives the same coefficients to within
  ±0.03. A field fixed to the world would change with heading, and the change in |B| does
  (0.04–0.40 µT/°C), as a body-fixed offset should. Treat the ±0.02–0.03 spread between
  headings as the real uncertainty. The per-fit standard errors (~0.001) are far too
  optimistic, because successive samples are correlated.
- **Heating and cooling match.** Heating-leg slopes agree with cooling-leg slopes
  (e.g. south x 0.508 heating vs 0.514 cooling), so the effect is reversible, with no
  hysteresis.
- **It is close to linear.** Quadratic terms are small and change sign between logs.
- **The heat gun's own field doesn't show.** Nothing steps when the gun switches off.
  Field and die temperature peak together.
- **The nose sensor adds nothing.** A second term on `water_temp_c` (MS5837 in the nose)
  changes sign from log to log over its 1–3 °C range, so there's no evidence of a separate
  "bulk" effect. **Never use `water_temp_c` for magnetometer thermal work.** It barely sees a
  board-local event (0.45 °C vs the die's 6 °C in an earlier test), and regressing against it
  gave coefficients of the wrong sign.
- **The linear model reproduces the logged heading** through the firmware's heading geometry
  to 0.85–2.1° RMS.

## Why the readings are corrected, not the heading

Temperature adds a roughly constant vector to the magnetic reading. The heading error is a
knock-on effect, and a messy one:

- Its size depends on where the reading sits relative to the origin of the calibrated plane,
  so **it depends on the calibration**. With the cal installed during the test, the same
  drift gave north +14° per 5 °C and south −3.6°. A correctly centred cal would give roughly
  +6° and −8°.
- It varies with heading, as a sinusoid.
- It is no longer linear once the offset is a noticeable fraction of the ~17 µT horizontal
  field.

A heading correction would be a 2-D table (heading × ΔT) that has to be refitted after every
calibration. Correcting the reading uses three numbers that survive recalibration. It also
fixes everything else that consumes the magnetometer:

- the Mahony filter, which uses the 3-D vector (z drift matters once the unit pitches)
- the logged `mag_*` columns
- cal sample collection
- gap-fill orientation

## How the firmware applies it

Pipeline: `readMagRaw_SensorFrame()` → **thermal correction** → axis map → hard-iron bias →
soft-iron matrix → Mahony → Fourier heading cal → motor offset.

- **Where.** `imu::readMagRaw_SensorFrame()` in [imu.cpp](../src/sensors/imu.cpp). Every
  magnetometer read goes through it, so no consumer can miss the correction. The die
  temperature is a separate 2-byte read of `TEMP_OUT` (0x2E), refreshed every 250 ms
  (`MAG_TEMP_READ_INTERVAL_MS`). Do not fold it into the field burst: an 8-byte read of
  0x28–0x2F returned `TEMP_OUT` frozen at a garbage 55.88 °C on the bench (2026-09-15).
- **Formula.** `counts -= coeff_uT_per_c × lsb_per_uT × (die_temp − ref_temp_c)`, rounded and
  clamped to int16.
- **Reference temperature: 21 °C.** Every calibration to date was collected at a die
  temperature of 20–21 °C. Normalizing readings to 21 °C leaves those calibrations valid, so
  there's no need to recalibrate when compensation is switched on. Calibrations collected
  later are fitted on normalized data automatically, since sample collection reads through
  the same correction.
- **Die temperature offset.** The sensor's absolute offset isn't factory-trimmed, but a
  constant error in it only shifts the correction by a constant, which the hard-iron bias
  absorbs. Only the **change** in temperature has to be right, and that is what the sensor is
  specified for.
- **Guards.**
  - A short I2C read or a temperature outside −40 to +85 °C keeps the previous reading.
  - Compensation stays off until a plausible reading has arrived.
  - The loader rejects a missing axis, non-finite values, |coeff| > 5 µT/°C, or a
    reference temperature outside the operating range. A rejected, deleted or absent file
    turns compensation **off** rather than leaving old coefficients running.
  - Boot and **Reload Cal Files** both reload the file.
  - The serial `[CAL]` dump shows `temp comp=on/off`.
- **Logs.** `mag_temp_c` is written at **every** log level, LOW included, so any dive log can
  be re-examined. Logs recorded with compensation active contain *compensated* `mag_*_raw` and
  `mag_*_cal` columns.

### `/mag_temp.json`

```json
{
  "ref_temp_c": 21.0,
  "coeff_uT_per_c": { "x": 0.5365, "y": 0.4171, "z": 0.2777 },
  "notes": { "...": "provenance written by the fit tool; ignored by firmware" }
}
```

- **Units.** µT per °C, in the **logical frame**: post axis map, pre calibration. That's the
  same frame as the `mag_*_raw` log columns and the `bias` in `mag_base.json`.
- **Sign.** A positive coefficient means the reading rises as the die warms.
- **Install.** Upload to the LittleFS root via `tern.local`, then press **Reload Cal Files**.
  The current file is `baseline cal jsons/mag_temp.json`.
- **Cloud sync.** The file isn't part of cloud cal sync or its backups. A
  `pio run -e nav -t uploadfs` erases it along with everything else.

## Heading guideline without compensation

For reference only: this is what the drift does to heading when *nothing* corrects it.
Values are for a correctly centred calibration (horizontal field ≈ 17 µT), and ΔT is die
temperature relative to the temperature at calibration:

| heading | °/°C (small ΔT) | ΔT −5 °C | ΔT −10 °C | ΔT −15 °C | ΔT +5 °C | ΔT +10 °C |
|---|---|---|---|---|---|---|
| N | +1.4 | −8° | −19° | −33° | +6° | +10° |
| NE | −0.3 | +2° | +4° | +9° | −1° | −2° |
| E | −1.7 | +10° | +22° | +35° | −8° | −14° |
| SE | −2.2 | +11° | +20° | +28° | −11° | −22° |
| S | −1.4 | +6° | +10° | +14° | −8° | −19° |
| SW | +0.3 | −1° | −2° | −3° | +2° | +4° |
| W | +1.7 | −8° | −14° | −19° | +10° | +22° |
| NW | +2.2 | −11° | −22° | −32° | +11° | +20° |

Rule of thumb: **error ≈ 2.2°/°C × sin(40° − heading)**. It is near zero around 040°/220°
and worst around SE/NW. It stops being linear past ~5 °C. A poorly centred calibration is
more sensitive in some sectors: with the cal installed during the test, the model put NE near
4°/°C.

Negative-ΔT columns (a dive colder than the calibration) are **outside the measured range**.
See the open items below.

## Refitting the coefficients

Use [tools/mag_temp_fit.py](../tools/mag_temp_fit.py):

```bash
# MID logs (calibrated columns): pass the cal that was INSTALLED while logging
python tools/mag_temp_fit.py "baseline cal jsons/thermal testing/"{north,east,south,west1,west2}.csv \
    --base  "baseline cal jsons/20260911 cal files/mag_base (2).json" \
    --mount "baseline cal jsons/20260911 level-fit mount v2/mag_mount.json" \
    --out mag_temp.json

# HIGH logs carry mag_*_raw: no cal files needed. Prefer HIGH for future thermal tests.
python tools/mag_temp_fit.py cold_test.csv --out mag_temp.json

# Logs recorded with compensation already active: add back what was applied
python tools/mag_temp_fit.py cold_test.csv --applied mag_temp.json --out mag_temp.json
```

What the tool does:

- Fits each log's cooling leg with one shared slope and one intercept per log.
- Skips any log whose die temperature moved less than 3 °C.
- Prints per-log slopes, so agreement across headings is visible.
- Prints the die-temperature range the result is valid over.

**For MID logs, the `--base`/`--mount` pair decides the answer.** Slopes are measured on
calibrated columns and mapped back through the inverse of the installed soft-iron matrix. With
the old tilted mount (scale 0.80/1.35), the same data gives raw x ≈ 0.65 and y ≈ 0.32 instead
of 0.54/0.42. The shipped file assumes the 2026-09-11 base plus the level-fit v2 mount. HIGH
logs avoid the question entirely.

## Open items and limits

1. **The cold side is untested.** Every test warmed from 19–22 °C, and the fit covers
   21–61 °C die. A dive colder than the calibration is extrapolation. The working
   assumption, pending a real cold-weather test (planned for winter 2026–27), is that heating
   and cooling behave the same, which they did within the warm range. Validate with a HIGH
   log.
2. **A heat gun is not a dive.** The gun heats the board locally, and the die temperature
   tracked the field perfectly there. In a dive the whole scooter cools slowly and evenly. If
   part of the measured drift came from parts near the sensor rather than the chip itself, it
   is still body-fixed, but may not follow die temperature under even cooling. The cold test
   answers this too.
3. **Coefficients belong to one unit.** They are for this magnetometer on this board.
   A replaced AltIMU needs its own test.
4. **Which mount was installed during the test** is inferred, not confirmed. See
   "Refitting" above.
5. **Calibration offset seen during the test.** The four baselines sat on a circle centred
   ~13 µT from the origin, with logged heading off by −20/+45/+6/−26° at N/E/S/W at room
   temperature. That's a calibration problem separate from temperature, and worth checking
   before judging compensated heading.
6. **`mag_temp_c` logged as `nan` for 29 s at a time (2026-09-16, unexplained).** In
   `thermal testing/wifi-toggle.csv` the column is `nan` for two sustained windows and a
   valid temperature otherwise, switching on a single sample in both directions. **The
   current source cannot produce this**: `g_magTempSeen` (imu.cpp) is set once and never
   cleared, a failed register read deliberately keeps the previous value, so
   `readMagTemp_c()` returns Ok with the last good value forever once one read succeeds.
   `timestamp_ms` is monotonic across both boundaries, so it is not a reboot. Most likely
   the flashed build predates this logic — check the boot banner's `FW_VERSION` against
   HEAD before investigating further. **Why it matters:** if `mag_temp_c` goes NaN in
   flight, thermal compensation is silently off for that whole stretch, and the user
   observed the NaN windows lining up with WiFi being off — which is the dive-mode state.
   Reproduce with a serial monitor attached before trusting compensation on a dive.
7. **Load-state magnetic offset is real but small.** `wifi-toggle.csv` (board stationary,
   pitch spanning 0.10°, roll 0.08°, both null in the fit) shows a repeatable step of
   **0.38 µT in |B| and 0.33° in heading** between the two power states, sign reversing on
   all four edges. Too small to compensate for today, but it is a genuine current-dependent
   offset that a single calibration can only absorb in one state. Note the test's own
   caveat: `DisplayCmd::TOGGLE_WIFI` in nav_main.cpp is a no-op on the radio (explicit
   TODO — it flips `gWifiEnabled` and writes NVS), so unless the toggle went through
   `setDiveMode()`, what was being switched is not confirmed to be the radio.

## History

- **2026-09-11.** Die temperature enabled (`CTRL_REG1` TEMP_EN) and logged at MID/HIGH.
  - Early estimates of ~1.1–1.3 µT/°C came from regressing against `water_temp_c` and are
    wrong.
  - A single-heading test gave 0.44 µT/°C x, consistent with this work, and "0.43°/°C heading".
    That heading figure holds only for that heading and geometry. Don't quote it as general.
- **2026-09-13/14.** Four-cardinal heat test, coefficients fitted, compensation added to
  firmware, `mag_temp_c` logged at every level.
