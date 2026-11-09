# DPV-Nav — System Overview

**Start here.** This volume explains what the system is, how it works, and why it's
built the way it is. Read it before the build guides; nothing in it tells you to pick
up a tool.

- Building one? → [Manufacturing Guide](manufacturing-guide.md), then [Assembly Guide](assembly-guide.md)
- Already have one? → [User Guide](user-guide.md)

---

## What it is

A dead-reckoning underwater navigation system that mounts on a DPV ("scooter"). It
knows which way you're pointed, how fast you're moving through the water, and where
you started. From those three things it continuously estimates where you are, and
shows you bearing and range to a waypoint you chose.

That's the whole idea. The scope is deliberately small.

### What it is not

This is **not** "underwater GPS." GPS radio doesn't penetrate water, and no amount
of firmware violates that. Precise underwater positioning is a solved problem at
higher price points — acoustic transponder networks, inertial-grade IMUs — and
precision scales almost perfectly with money spent.

What this *is*: a tool that will tell you where you are far more accurately than
anything else readily available at this price point. If your current process is a
wrist compass plus elapsed time plus mental arithmetic, this is a large step up.  Frankly, if you're just reading a compass mounted on your DPV, this is a big step up.  I wish I'd had this while I was hunting for the Scout the first time -- we would have immediately realized we were practically in the wrong lake.

### How dead reckoning works

Each iteration takes the previous estimated position, applies the current heading
and speed, and produces a new estimated position:

```
x += speed × sin(heading) × dt
y += speed × cos(heading) × dt
```

At ~100 Hz, with the display refreshing at 10 Hz. Three consequences follow directly
from that loop, and they shape everything else in the system:

1. **Errors compound.** The longer since you last knew your true position, the
   farther the estimate can be from reality. There is no mechanism by which the
   estimate self-corrects.
2. **The starting fix matters as much as the heading.** An estimate is only ever
   relative to a known point. This is why GPS lives outside the main housing on a
   raisable mast, and why fix quality is on the headline of the display.
3. **Resetting beats improving.** Any time you reach a position you actually know —
   a surface fix, a charted waypoint — snapping the estimate to it (**NAV > Arrive
   WP**) zeroes the accumulated error for the next leg. Short legs between known
   points beat one long leg, every time.

### How accurate, honestly

**For planning, assume error is about 10% of the distance travelled since your last
known fix.** Submerge, run 500 m, and expect to be within 50 m of the target.

That's the planning number, not the best case. A careful calibration and a straight
run have produced 2–3% in practice. But 10% is what you should brief your buddy on,
and a 50 m radius is too large to search in one pass — plan on dropping a marker and
running a reel.

Long straight runs at constant speed give the best results. A dive spent circling a
wreck gives up a little accuracy on every turn.

> **⚠ Safety** — this is an *aid*, not an authority. It does not know your
> certification limits, your gas, your deco obligation, or your dive plan. It does
> not actually know where you are; it is estimating. It also adds a cabled device to
> your DPV, which means added entanglement risk and task loading. Dive within your
> limits and your training. See [SAFETY.md](../SAFETY.md).

---

## Architecture

Four physical modules, each with its own housing and its own wiring. The nav housing
is the core; everything else terminates there.

```
   [ GPS module ]                [ Display module ]
    on a raisable                 ESP32 + ST7789 TFT
    mast, on a cable              320×240, two buttons
          │                               │
          └──────────┬────────────────────┘
                     │  wired serial link (JSON @ 10 Hz)
            ┌────────┴────────┐
            │   NAV MODULE    │   ESP32 + 9-DoF IMU + depth sensor + battery
            │  (the core)     │   AHRS, dead reckoning, logging, tern.local web UI
            └────────┬────────┘
                     │
            [ Flow meter ]   fully potted, bolted to the nav housing
```

### Nav module

Where the math happens, where `tern.local` is served from, and where the IMU and
battery live. An ESP32 wired to a Pololu AltIMU 9-axis unit. A depth sensor in the
housing drives dive/surface mode and logs depth and water temperature. The battery
is a small flat-pack in a non-metallic housing, to keep its magnetic signature down.

Everything is arranged so the IMU — where magnetic field is measured — sits as far
from wiring as possible. **Power wiring especially.** Run battery power past the IMU
and you will get heading deflection that varies with current draw, which no
calibration can absorb, because the field changes with load.

> **💡 Note** — the IMU's installed orientation is not negotiable. All of the math
> assumes it is mounted the way the reference build mounts it. You may well be right
> that some other orientation is better; install it this way anyway, or the codebase
> won't work for you. See [ahrs-orientation.md](ahrs-orientation.md).

### Display module

Its own ESP32 driving an EyeSPI board and the TFT. It started as a dumb serial
receiver; over time real functionality migrated onto it, caching data and state to
hand back to nav for processing or upload.

Early prototypes used GoPro housings: already depth-rated, two buttons from the
factory, flat rear window, and a whole ecosystem of mounts. The cost ($60 each) and
the mechanical button alignment were the problems — dives were lost to buttons that
wouldn't press. The interior framing was re-engineered and now mostly works, but it remains
finicky. See [§4.3 Buttons](manufacturing-guide.md#43-buttons) for why this is the
strongest argument for the milled variant.

### GPS module

GPS doesn't work underwater, so the obvious question is why it's here at all — and
why it's outside the nav housing.

It's here because dead reckoning estimates from a *known* starting point, so fixing
a good initial position is critical to every subsequent step. It's outside because early
prototypes mounted GPS directly to the nav board, which works beautifully on the
beach and **fails at the descent buoy**, where you'd have to hold the whole scooter
high enough out of the water to clear surface-reflection interference. On a cable
with a mast, you unclip it and raise just the antenna while the scooter stays comfortably submerged.

A powered external antenna on the GPS board's u.fl port gave the best fixes, but the
connector proved too unstable to rely on. A GPS-only board without a battery will
still give you fixes; the reference build uses the Adafruit Ultimate GPS because its
battery-backed memory retains ephemeris data, giving much faster re-acquisition.  The most recent build includes an active 4-system antenna, and it gets fixes FAST.  So fast your dive buddies will wonder if you really bothered to get a fix.

### Flow meter

It doesn't much matter which one you get, as long as flow sensitivity, size and
depth rating are roughly right.  If you get the exact unit from the BOM, you'll be able to use the same k-factor for speed calibration.  The options all amount to the same thing: an
impeller driving a Hall-effect sensor that must be calibrated before it means
anything. Whatever you end up with goes in a box full of epoxy, with the signal
wires emerging from the potting.

It bolts to the side of the nav housing, with piping at both ends to make the flow
it sees repeatable. Alignment with the direction of travel matters.

### Power

A 3000 mAh flat-pack, which lasts comfortably longer than a dive.

> **⚠ Safety** — the referenced pack's JST plug polarity is **reversed** from what
> the Adafruit ESP32 expects. Check and correct it with a meter before plugging
> anything in. See [§9.6](assembly-guide.md#96-power-wiring-notes).  The beta builds replace the provided JST plug with an XT30 (more robust), plus a power switch and a charging connection, so it should be more robust than the alpha design.

Battery leads should be as short as possible. They carry enough current to generate
a field that will move your heading if they run anywhere near the IMU. Keep power at
the back of the board.  Do not, under any circumstances, route wires that carry power anywhere near the IMU.

### Why it's split this way

The goal was as few pressure boundaries as possible, and the first concept put
everything in one housing. It didn't survive contact with the physical constraints:

- The **IMU** must be aligned to the scooter tube, so it mounts *on the tube* — the
  alternative is a wonky alignment procedure on the handle.
- The **display** must be where you can see it: either something enormous on the
  tube, or a sane-sized screen on the handle.
- The **flow meter** inside the housing would mean an unnaturally large housing with
  plumbing running through it — a *more* complex pressure boundary than potting the
  meter and leaving it outside.
- **GPS** needs to get well clear of the water line, and I am not strong enough to tread water with a 52lb Cuda 650 over my head.

---

## Firmware

Two PlatformIO environments out of one repository:

```bash
pio run -e nav        # nav device
pio run -e display    # display device
```

**Note** There's a tentative 3rd environment for a tiny Arduino device in the GPS housing that provides yet another indication.  This may prove to be not value added, though, since this indication may be more readily available elsewhere.

**Nav device** runs the sensors (LSM6DS33 + LIS3MDL IMU, Adafruit GPS, hall-effect
flow sensor, optional MS5837 depth), the Mahony AHRS filter, the dead-reckoning
position model, logging, the web UI and all cloud traffic.

**Display device** runs the TFT, the buttons and the menu. The menu is **compiled
in** — there is deliberately no filesystem override for it.

The two are linked over a wired serial line carrying JSON packets at 10 Hz.
Both boards update together over OTA.

### Calibration chain

Calibration is the single largest determinant of accuracy, and the steps are
ordered because each depends on the one before:

| Step | Menu | What it establishes |
|---|---|---|
| 1 | `CAL > Baseline` | The magnetic signature of the board and housing alone, off the DPV. Full-sphere coverage. |
| 1a | `CAL > Fill gaps` | Patches the orientations the baseline pass missed, guided by server-graded coverage. |
| 2 | `CAL > Mounted` | How that signature changes with the scooter present. Horizontal rotations, ±30° pitch. |
| 3 | `CAL > Hdg cal` | 12-point Fourier fit against an external compass; takes residual error to ~2°. |
| — | `CAL > Speed cal` | Flow-sensor k-factor, from timed runs over a known distance. |

Each step uploads its samples, the server fits them, and the unit shows one graded
number to accept or reject. An offline Python fit exists as a WiFi-less fallback.

> **⛔ Stop** — accepting a new baseline or mounted cal changes what "indicated
> heading" means, which silently invalidates the Fourier heading fit. After any
> baseline or mounted change, the 12-point heading cal must be **re-collected from
> scratch**, never gap-filled. Nothing enforces this yet; see
> [backlog.md](backlog.md) Tier 1.

Depth: [calibration-guide.md](calibration-guide.md).

### Other subsystems

- **Positioning:** DR in a local flat-earth XY frame, with GPS position truth
  override on the surface (filtered — an SOG deadband plus a COG coherence check
  reject position-jitter noise at rest).
- **Waypoints:** up to 200 lat/lon waypoints (HOME plus user-defined) in
  `/config/waypoints.json`, managed from the web UI. SELECT navigates to one;
  ARRIVE snaps the estimate to it.
- **Logging:** three levels to LittleFS, auto-uploaded to Dive Map once the unit
  joins a known network. HIGH carries raw magnetometer columns. See
  [data-logging-guide.md](data-logging-guide.md).
- **Current measurement:** hold station upstream for up to 60 s and the impeller
  reads the current directly. Post-processing only, for now.
- **Thermal compensation:** magnetometer readings are temperature-corrected from
  measured per-board coefficients. See
  [mag-temperature-compensation.md](mag-temperature-compensation.md).
- **Op mode:** the depth sensor switches dive mode (GPS + WiFi off) on at ~3 m and
  back off after 15 s shallower than 2 m. Heading is never gated on either radio —
  by design, GPS is optional and dive mode kills it.

---

## Design goals

1. **Design for Manufacturability.** This isn't a $10,000 Italian-engineered underwater-equivalent-of-a-Ferrari.  It's a $500 DIY project.  It needs to balance effective functionality against cost and ability of a typical DIY-er to build.
2. **Design for Simple Usability.** Minimize complexity wherever possible.  Tech divers do mental math all the time, approximations are typically good enough, and getting lost in menus and button pushing at depth is NOT what anyone wants to do.  The operation should remind people more of an iPad Touch than a Sony Alpha.
3. **Fail loudly, never silently.** The failure mode that matters is bad data that
   looks fine and gets discovered a week later. The backlog is ordered by this, not
   by what's easiest.
4. **Readily available parts**, and a build that stays within reach of a determined
   hobbyist.
5. **Source-available and DIY-first.** Build one, modify it, share what you learn.

---

## About the name

Named after the Arctic Tern, which navigates pole to pole every year — mating in the
Antarctic, raising young in transit, maturing in the Arctic, then migrating back to
do it again. If a bird that size can manage that annually, a DIY device ought to
manage a few hundred metres of lake with confidence.
