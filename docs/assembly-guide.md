---
title: "DPV-Nav Assembly Guide"
description: "Soldering, wiring, sealing, leak testing and firmware for a Tern DPV-Nav dead-reckoning navigation system."
hardwareRev: "v0.x"
firmwareMin: "0.7.x"
lastUpdated: 2026-11-09
draft: true
---

# DPV-Nav Assembly Guide

**Volume 2 of 3.** This volume takes a box of finished parts and turns it into a
working, dry, flashed device sitting on your bench.

| | |
|---|---|
| ← Before this | [Manufacturing Guide](manufacturing-guide.md) — choose a variant, make the parts |
| → After this | [User Guide](user-guide.md) — calibrate and dive |
| Background | [System Overview](overview.md) — why it's wired this way |

Callout conventions are defined in the [docs index](README.md#callout-conventions).
A **📝 TBD** block marks a section that is outlined but not yet written.

> **⛔ Stop** — do not start this volume until you have cleared the checkpoint at
> the end of the [Manufacturing Guide](manufacturing-guide.md#9-potting-the-flow-meter):
> all parts made, test-fit dry, potting cured and inspected.

---

## 8. Electronics

*Section numbers continue from the Manufacturing Guide, so a reference to "§9.1"
means the same thing in both volumes.*

### 8.1 Soldering

> **📝 TBD** — per-board build order. General rules: lowest-profile parts first,
> SMD before through-hole, connectors last. Call out anything orientation- or
> polarity-critical, anything heat-sensitive, and anything you should **socket**
> rather than solder down (the ESP32 module, the sensor breakouts) so it can be
> swapped without a rework station. Photos per board at the "done" state.

> **💡 Note** — socket what you can. A bad joint on the nav board's display-link
> pin once broke display→nav commands entirely, and the symptom looked like a
> firmware bug for a long time. Sockets and continuity checks are cheaper than
> firmware theories.

### 8.2 Bench smoke test

> **📝 TBD** — power up on the bench with a current-limited supply. Give expected
> rail voltages and expected idle current draw, as actual numbers, checked before
> and after each board is added. Confirm the device enumerates and boots.

> **⛔ Stop** — do not install a board into a housing that hasn't passed §8.2.
> Getting it back out is the expensive part.

---

## 9. Wiring

This is the highest-value section in the document, and the one where this guide is
most prescriptive. Wire colors and junction technique are the things builders get
wrong and then can't debug.

### 9.1 Wire color and signal map

> **📝 TBD** — **the** master reference table: signal | color | gauge | from | to |
> notes, one row per conductor, grouped by harness. Keep it anchored and linkable;
> people will bookmark it and come back to it mid-build.
>
> State the *convention* as well as the table — e.g. red = switched V+, black =
> ground, and what each signal color means — so a builder extending the system stays
> consistent with it.

> **⚠ Safety** — **housing wire colors do not reliably indicate function.**
> A replacement flow sensor's lead colors did not match the previous one's. Bench-ID
> every wire on a new sensor with a meter before you plug it into anything.

Current nav-board connector assignments worth knowing before you wire:

| Connector | Carries | Notes |
|---|---|---|
| J2 | 5V / GND / IO34 | Flow sensor. Pinout changed 2026-10; confirm against the board rev you have. |
| J3 | I²C | Depth sensor (MS5837-30BA). Optional at build time. |
| J5 | Charging | Nav board Rev D and later. |

> **📝 TBD** — fold the above into §9.1's master table once it's written, and delete
> this stub so there's only one source of truth.

### 9.2 Harness overview

> **📝 TBD** — which harnesses exist, what crosses a pressure boundary, and cut
> lengths with slack allowances. A diagram here saves a thousand words.

### 9.3 Junctions outside pressure vessels

Junctions live **outside** the pressure vessels on purpose: you want each pressure
boundary to be a single continuous conductor, with no splice anywhere you can't
inspect it.

> **📝 TBD** — the full how-to: strip lengths, the mechanical joint (lineman's /
> Western Union, or a crimp barrel — pick one and standardize), solder technique,
> wicking control, strain relief, and the fact that **the joint carries the load,
> not the solder**.

### 9.4 Heat shrink

> **📝 TBD** — step-by-step with photos:
> - adhesive-lined vs. plain, and when each
> - sizing (shrink-ratio math)
> - **pre-load the shrink onto the wire BEFORE you solder** — the classic mistake,
>   worth its own callout
> - stagger joints in a bundle so the diameter doesn't stack
> - double-shrink over the whole joint
> - heat gun technique and temperature
> - how to tell when it's actually *sealed* rather than merely shrunk

### 9.5 Glands and penetrators 🔁

> **📝 TBD** — sealing procedure for both variants. For the cheap-gland path this is
> *the* critical technique section: prep, epoxy inside the gland if required, torque,
> and the fact that [§11](#11-sealing-and-leak-testing) is what qualifies it.

### 9.6 Power wiring notes

> **⚠ Safety** — the referenced Amazon battery pack has its **JST plug polarity
> reversed** from what the ESP32 expects. Historically, making this mistake has not
> fried a board, but Adafruit repeatedly says it will. You will probably need to swap
> the polarity at the JST connector (switch the black and red wires) before plugging
> it in. Double-check against the enclosed diagram **and** with a voltmeter before
> you connect anything.

Two routing requirements, both about the magnetometer rather than about electrical
safety:

1. **Battery leads as short as possible, and kept at the back of the board.** They
   carry enough current to generate a field that moves your heading. Because the
   field scales with load, calibration cannot absorb it — the error changes as draw
   changes.
2. **Route all power wiring away from the IMU.** Same reason. See
   [Overview → Nav module](overview.md#nav-module).

> **✅ Checkpoint** — full harness built, continuity-checked end to end, no shorts to
> ground or between rails. Power-on test with the harness attached, **outside** the
> housings.

---

## 10. Mechanical assembly

### 10.1 Internals into housings

> **📝 TBD** — order of operations per module. Which connectors mate before the sled
> goes in. Cable routing and service loops: leave enough slack to pull the guts out
> without desoldering anything. Photos at each stage.

### 10.2 O-rings and closing up

> **📝 TBD** — o-ring sizes, inspection, the correct grease and how much (a thin
> film, not packed), seating, endcap alignment, fastener sequence and torque,
> desiccant pack and moisture indicator card.

### 10.3 Mounting to the DPV

> **📝 TBD** — mount geometry and fastener spec.

Two constraints that are requirements, not suggestions:

- **Magnetic separation from the motor and the battery cables.** This is a hard
  minimum distance, for the reason given in [§9.6](#96-power-wiring-notes): a
  load-dependent field cannot be calibrated out.
- **Repeatable alignment of the IMU axes to the vehicle centerline.** This is a
  calibration input. Consistent offset is fine; *slop* is not, because it changes
  between dives and nothing downstream can correct for that. See
  [Manufacturing §7.4](manufacturing-guide.md#74-post-processing-and-test-fit).

---

## 11. Sealing and leak testing

This is the gate between "assembled" and "wet," and it comes **before** firmware.
Don't let anyone skip it.

Before putting any electronics in housings, test your work against leaks. Do a
careful assembly of everything: hand-check all the glands for tightness, clean and
lubricate every o-ring and seating surface.

**1. Vacuum check.** A good first test. Plug in your MityVac, draw 10–15 inches of
mercury, mark what you've pulled it to, and step away for 5 minutes. It shouldn't
drop at all. Any drop means **something** is leaking.

**2. Bucket test.** For some leaks the best thing is just to drop it all into the
deepest bucket you have available. You may need to attach a lead weight to make it
sink.

- *Look for bubbles.* Some bubbling around the outside for a moment is often normal,
  but it should slow and then stop fairly quickly. What you're looking for is a
  consistent stream — that's a rapid leak.
- *Leave it overnight.* Not enough to guarantee it won't leak at depth, but it gives
  you confidence there's no slow leak in the shallows.

**3. Take it on a dive, empty.** The best test available at this point. Mount it with
no electronics inside and take it to depth. In practice, if one of these housings is
going to fail at technical depths, it'll generally do so at recreational depths — you
don't need to go to 100 m to discover it leaks at 20 m.

**4. Then do it before every dive, forever.** You've now demonstrated that you can
build a housing that stays dry *once*. Before every dive: clean the seating surfaces,
lubricate the o-rings, double-check the glands, and run a vacuum test. Murphy says
that the dive you skip the vacuum check is the dive it leaks.

> **📝 TBD** — the numbers and the diagnosis procedure:
> - vacuum test spec: pull-to pressure, hold time, acceptable leak-down — actual
>   numbers
> - what to do when it doesn't hold: bisecting which seal is leaking
> - 🔁 hydrostatic / pressure test if you have the capability: test to depth ×
>   safety factor, hydraulic not pneumatic. **Mandatory for the milled variant.**
> - shallow wet test with the housing sealed and empty (or carrying a dummy mass)
>   before it ever holds electronics in water
> - post-test inspection: moisture card, o-ring witness marks

> **⛔ Stop** — the system does not go in the water with electronics inside until
> every housing has passed §11.

---

## 12. Firmware

### 12.1 Toolchain

Install [PlatformIO](https://platformio.org/) (the VS Code extension is the easiest
path), then clone the repository:

```bash
git clone <repo-url> dpv-nav
cd dpv-nav
```

> **📝 TBD** — which board definitions and which USB serial driver, per host OS.

### 12.2 Flashing

Both boards build from this one repository, as two environments:

```bash
pio run -e nav                 # build nav
pio run -e display             # build display
pio run                        # build both

pio run -e nav -t upload       # flash nav
pio run -e display -t upload   # flash display
```

Upload ports are set per environment in `platformio.ini` (`upload_port`). Edit them
to match your machine.

**Do not run `pio run -t uploadfs`.** Neither board needs a filesystem image. There
is no `data/` directory: every file on the nav board's filesystem is written at
runtime, and the display board's filesystem is unused. Running it is **destructive** —
it erases dive logs and every calibration file.

Watch the boot output with:

```bash
pio device monitor
```

> **📝 TBD** — what a successful flash looks like, and bootloader/recovery if a board
> bricks.

> **💡 Note** — after the first flash, firmware updates go over the air from
> `tern.local`. OTA updates **both** boards in one pass, so they stay in step.

### 12.3 Configuration

Compile-time constants live in [`src/config.h`](../src/config.h). The ones a builder
usually needs to touch:

| Constant | Why you'd change it |
|---|---|
| `FLOW_CROSS_SECTION_M2` | Set once to match your DPV's intake geometry. |
| `DEFAULT_BASELINE_LAT` / `_LON` | Local XY origin; set near your dive area. |
| `DISPLAY_MODE` | 0 = navigation, 1 = bench debug screen. |

Everything else a diver needs — units, WiFi credentials, waypoints, cloud account
pairing — is set at runtime, not compiled in. See
[User Guide → First-Time Setup](user-guide.md#first-time-setup).

> **📝 TBD** — the full annotated config walk-through, and the declination story
> (currently a hard-coded constant; see [backlog.md](backlog.md) Tier 3).

### 12.4 First boot

> **📝 TBD** — expected serial output, annotated line by line. What sensor discovery
> looks like when the IMU / depth / flow meter are found, and what a missing one looks
> like. Link each failure signature straight to [§13](#13-troubleshooting).

What to expect in outline:

- The nav board prints a self-test block, one row per subsystem, each `ok` or `FAIL`.
  A board with no depth sensor shows a `Depth .. FAIL` row and is working correctly.
- **Gyro and accel calibration run automatically on first boot**, if their files are
  absent, and they prompt **on the serial port** — not on the display. Keep USB
  connected for the first power-up. Gyro is 10 s at rest; accel is a six-orientation
  sequence, 2.5 s each.
- The display shows `NO LINK` until the nav board's packets arrive.

> **💡 Note** — "online but a feature is silent" is almost always an unflashed build.
> Read the boot log before auditing code.

> **✅ Checkpoint** — all sensors enumerate, all rails stable, the display lights up
> and shows live data on the bench.
>
> **→ Continue to the [User Guide](user-guide.md), starting with
> [First-Time Setup](user-guide.md#first-time-setup) and then calibration.**

---

## 13. Troubleshooting

Build-time problems only. For calibration and in-water behavior, see the
[User Guide's troubleshooting section](user-guide.md#troubleshooting).

| Symptom | Likely cause | Check |
|---|---|---|
| `NO LINK` on the display | Serial link wiring between the boards; a cold or cracked joint on the link pin | Continuity-check the link pair before theorizing about firmware |
| Sensor not found at boot | I²C address, wiring, or a cold joint | WHO_AM_I values and bus continuity |
| 3.3 V rail sags on WiFi connect | Power wiring / supply headroom | See [§9.6](#96-power-wiring-notes) |
| Heading error concentrated in one compass sector | Declination or an axis-convention error | Not a wiring fault; see the User Guide |
| Clean RMS but bad real-world heading | Calibration pass criteria, not assembly | See [user-calibration-guide.md](user-calibration-guide.md#what-good-looks-like) |
| Flow meter reads zero or erratic | Potting void, wrong lead identified, or mounting orientation | Bench-ID the leads; re-read [Manufacturing §9](manufacturing-guide.md#9-potting-the-flow-meter) |
| Moisture card triggered after a dive | Seal | [§10.2](#102-o-rings-and-closing-up), [§11](#11-sealing-and-leak-testing) |
| Online but a feature seems missing | Unflashed build | Read the boot log and check the reported version |

> **📝 TBD** — grow this table every time you or a builder hits something new. Keeping
> it in the repo is what makes it PR-able, and this is the section that benefits most
> from other people's failures.

---

## 14. Maintenance and servicing

> **📝 TBD** — o-ring inspection and replacement cadence, post-dive rinse, desiccant
> replacement, buffer LiPo replacement interval and disposal, storage, and the
> firmware update procedure.

**Recalibration triggers** — any of these invalidate work you've already done:

- **A new dive site** with a different magnetic declination.
- **Anything remounted.** Moving the nav unit on the tube changes the mounted cal.
- **Motor or battery cable changes.** New field, new mounted cal.
- **DPV service or a new impeller.** Redo speed cal with RESET+ACCEPT.
- **A new baseline or mounted mag cal** invalidates the 12-point heading cal, which
  must then be **re-collected from scratch, never gap-filled.** Nothing enforces this
  yet — see [backlog.md](backlog.md) Tier 1.

---

## Appendices

- **A. Pinout tables** — 📝 TBD, per board and per connector
- **B. Wire color master table** — see [§9.1](#91-wire-color-and-signal-map)
- **C. Serial command reference** — 📝 TBD, every console command
- **D. Changelog** — 📝 TBD, document changelog, separate from firmware versions
