# Calibrating DPV-Nav

A step-by-step walk through the whole calibration chain, with the reasoning behind
each step. If you only want the button presses, every step has a **Do this**
block. If you want to know why a step exists, or why it has to come where it does,
read the **Why** block above it — that's the part that makes a bad result
diagnosable instead of mysterious.

This is the diver's guide. [dev-calibration-guide.md](dev-calibration-guide.md)
is its engineering companion — file formats, fitting tools, gate constants, frame
conventions, serial diagnostics and the decision records behind several of the
behaviours described here. The two were one document until the knowledge was
split by audience; if something here raises a "but why does it work that way"
question, the answer is usually over there.

---

## Contents

- [What calibration is actually fixing](#what-calibration-is-actually-fixing)
- [The chain at a glance](#the-chain-at-a-glance)
- [Why the order is what it is](#why-the-order-is-what-it-is)
- [Before you start](#before-you-start)
- [How every cal screen works](#how-every-cal-screen-works)
- [Step 0 — Gyro and accelerometer (automatic)](#step-0--gyro-and-accelerometer-automatic)
- [Step 1 — Baseline](#step-1--baseline-cal--baseline-1)
- [Step 1a — Gap fill](#step-1a--gap-fill-cal--fill-gaps-1a)
- [Step 2 — Mounted](#step-2--mounted-cal--mounted-2)
- [Step 3 — Heading](#step-3--heading-cal--heading-3)
- [Step 4 — Speed](#step-4--speed-cal--speed-cal)
- [Step 5 — Motor offset (optional)](#step-5--motor-offset-optional)
- [Temperature (optional, advanced)](#temperature-optional-advanced)
- [What good looks like](#what-good-looks-like)
- [Checking a finished calibration](#checking-a-finished-calibration)
- [When to recalibrate, and what invalidates what](#when-to-recalibrate-and-what-invalidates-what)
- [When it goes wrong](#when-it-goes-wrong)

---

## What calibration is actually fixing

DPV-Nav navigates by dead reckoning: it integrates **heading × speed × time** to
keep a running position. There is no GPS underwater to argue with it, so every
error it makes is permanent until you tell it where you are again (**Arrive WP**)
or surface.

Heading dominates that error budget. One degree of heading error puts you about
**1.7 m off course per 100 m travelled** — 17 m over a kilometre. Ten degrees puts
you **175 m** off over the same kilometre, which is the difference between
arriving at the wreck and swimming past it.

Heading comes from the magnetometer, which measures the earth's magnetic field
(about 50 µT here) and works out which way north is. The problem is that it
measures *every* magnetic field it's sitting in, not just the earth's:

- **The unit's own field.** The PCB, the battery, the screws, the housing
  hardware. These sit in a fixed position relative to the sensor, so they add a
  fixed offset in the sensor's own frame — **hard iron** — and bend the field's
  shape around the sensor — **soft iron**.
- **The scooter's field.** Motor iron, battery pack, steel structure. Far
  stronger than the unit's own, and also fixed relative to the sensor once the
  unit is clamped in place.
- **Everything else that's left.** The scooter isn't a tidy magnetic shape. What
  survives the first two corrections is a smooth, repeatable error that depends
  on which way you're pointing.

Here's the useful mental picture. Sweep a *perfect* magnetometer through every
possible orientation and plot the readings: you get a **sphere**, centred on zero,
with a radius equal to the local field strength. Sweep a real one and you get an
**ellipsoid**, squashed and shifted off-centre. Calibration is the job of finding
where that ellipsoid's centre is (hard iron) and how it's squashed (soft iron),
so every future reading can be mapped back onto the sphere it should have been on.

Each of the three problems above gets its own step, for a reason: they have
different shapes, they need different data to solve, and they change
independently. If you ever swap scooters, only the second and third have to be
redone.

---

## The chain at a glance

| # | Step | Menu | Where | Roughly | Solves for |
|---|---|---|---|---|---|
| 0 | Gyro + accel | automatic at first boot | bench, still | 30 s | Gyro bias, accel bias and scale |
| 1 | **Baseline** | `CAL > Baseline (1)` | **off** the scooter, in your hands | 5–10 min | The unit's own hard and soft iron, fit over a full sphere |
| 1a | **Gap fill** | `CAL > Fill gaps (1a)` | **off** the scooter | 3–5 min | The orientations step 1 missed. Optional, usually worth it |
| 2 | **Mounted** | `CAL > Mounted (2)` | **on** the scooter, on a stand | 10–20 min | The scooter's magnetic signature, on top of the baseline |
| 3 | **Heading** | `CAL > Heading (3)` | on the scooter, against a known compass | 10–20 min | Whatever heading error survives steps 1–2 |
| 4 | **Speed** | `CAL > Speed cal` | in the water, known distance | 3–6 runs | The flow sensor's k-factor |
| 5 | Motor offset | post-processing | in the water, motor running | — | The constant shift a running motor adds. Optional |

Plan on an hour for the bench steps the first time through, less once you know
the rhythm. Steps 1–3 are all indoors and dry; only step 4 needs water.

Which of the three magnetic problems each stage actually touches:

| Distortion | Baseline | Mounted | Heading |
|---|---|---|---|
| Hard-iron bias (sensor, PCB, housing) | ✓ fitted | refined | — |
| Soft-iron coupling (PCB, enclosure) | ✓ fitted | refined | — |
| The DPV's own magnetic signature | — | ✓ fitted | — |
| Whatever shape is left over, as a function of heading | — | — | ✓ fitted |
| Drift with sensor temperature | — | — | — (handled separately, [see below](#temperature-optional-advanced)) |

---

## Why the order is what it is

Each step is fitted **on top of** the one before it. That is the whole reason the
order matters, and the reason redoing an early step forces you to redo the later
ones.

**We start with the baseline because the only way to find the centre and shape of
an ellipsoid is to collect points all over it** — a full sphere of orientations,
pointed every direction and rolled every way. You can do that with the unit loose
in your hands in about five minutes. You absolutely cannot do it with the unit
clamped to a scooter. So the baseline comes first, off the scooter, where
full coverage is physically possible. Doing it off the scooter also means the
scooter's iron isn't in the data at all, which is exactly what we want: this fit
describes the *unit*, and it stays valid even if you move the unit to a different
scooter.

**That first pass will be incomplete, so gap fill exists to finish it.** Nobody
tumbles a handheld object evenly through a full sphere, and while you're
collecting there's no trustworthy way to know what you've missed — working out
which way the unit is pointing requires the very calibration you're in the middle
of creating. (The unit does show a live coverage grid once it has roughly located
north, but that's for *steering*, not grading.) So the server grades the coverage
properly after the fit, cell by cell, and hands back the ones that came out thin
or empty. Gap fill re-collects only those and merges the patch into the baseline
you already have, instead of making you start over. It's optional because a
well-covered baseline doesn't need it. In practice, most first passes do.

**Then we mount it on the scooter and do a constrained fit, to measure the
interference the scooter adds in the configuration you'll actually dive.** On the
stand you have maybe ±30° of pitch and a bit of roll — nothing like a sphere. But
you don't need a sphere any more: the baseline already explains the unit's own
distortion, so this pass only has to solve for the much smaller correction the
scooter adds on top. Far fewer unknowns, far less coverage needed — three pitch
rows of twelve headings is enough. And the attitudes you can reach on the stand
are, not coincidentally, the attitudes you ride in. A calibration fitted over a
full sphere and a calibration fitted over your actual dive envelope are not the
same calibration, and the second one is the one that makes your heading right
where it matters.

**Then heading cal cleans up what's left, and ties the unit to north.** After both
magnetic stages the field is corrected, but 5–20° of heading error typically
survives, because the scooter's magnetic geometry isn't a tidy ellipsoid and the
first two stages can only fit the ellipsoid part. What's left over is smooth and
repeatable — it depends only on which way you're pointing — so it can be captured
as a curve: you take twelve readings at 30° apart against a known-good compass,
and the server fits a Fourier series through them. This is also the only step in
the chain that references anything *outside* the unit. Steps 1 and 2 make the
unit self-consistent; nothing in them knows which way north is. That's what makes
your reference compass part of the instrument — an error in it goes straight into
every heading you will ever read. See [Before you start](#before-you-start).

**Speed cal is independent, and can happen whenever you're next in the water.**
It has nothing to do with the magnetometer; it just measures how many impeller
pulses your particular intake produces per litre of water. It's in the chain
because position needs both halves of `heading × speed`.

**The motor offset is last because it can only be measured underway.** Heading cal
is a dry bench procedure with the motor off, and a running motor adds its own
roughly constant field on top.

---

## Before you start

**A compass you trust.** This is the single most important prerequisite, and the
easiest to get wrong. Heading cal aligns the unit to an absolute reference, so
whatever error your reference has becomes the unit's error, permanently and
invisibly. Most tech divers have a compass mounted on the scooter; it may or may
not be accurate. Mine was about 12° off, which produced a long run of confusing
calibration results before I found it.

Check it before you use it: point the scooter precisely at a distant object on a
known bearing (use the alignment sights, and take the time to get it genuinely
square), then note whatever error the compass shows. That gives you one trusted
bearing. If the compass is otherwise round — consistent all the way around the
card — you can work from that correction.

Worth knowing: every link in that chain costs you something. Sighting down the
tube to a landmark, reading a 2-inch compass on the handle, and the unit itself
each carry roughly 1–2° of their own. That's most of the explanation for why a
calibration that checks out at ±1° on the bench shows ±3° residuals in the water,
and the 12-point check at the end of heading cal is blind to it, because it
compares the unit against the same compass that may be wrong.

**A magnetically clean space.** A living room over a wooden floor is ideal. A
garage with rebar in the concrete slab is not; neither is anywhere near a car,
a steel workbench, a speaker, or a laptop. The baseline in particular is fitting
the earth's field, so anything else in the room ends up in your calibration.

**Room temperature, ideally around 21 °C.** The magnetometer's zero point drifts
with the sensor's own die temperature — enough to matter — and the compensation
file the unit ships with is referenced to 21 °C. Calibrating somewhere unusually
hot or cold will cost you accuracy at dive temperature. See
[Temperature](#temperature-optional-advanced).

**WiFi and a linked account, if you can.** Every step uploads its raw samples to
Dive Map, which does the fitting and grades the result, and hands it back within
seconds for you to accept or reject on the unit itself. Without WiFi everything
still works — the samples are always saved to the unit — but you'll either upload
them later from `tern.local` or run the Python tools by hand. See each step's
offline note.

**A stand for the scooter** that holds it level and lets you turn it through a
full circle, and tilt it nose-up and nose-down about 30°.

---

## How every cal screen works

All four magnetic steps share one screen layout and one set of button rules, so
once you've done one you know the rest.

Across the top: the **step strip** (`1 1a 2 3`) shows which step you're on, and
the number on the right counts toward whatever ends it — samples, cells or marks.
Near the bottom, one line says what to do right now, colour-coded: **red** means
something is blocking you, **yellow** means adjust, **green** means good. Under
that are two keys, one per button — **BTN1 on the left, BTN2 on the right** —
each labelled with what that button does at this moment.

- **BTN2 keeps going:** FINISH, MARK, ACCEPT, OK.
- **Hold BTN1 for 2 seconds to throw the work away:** QUIT while collecting,
  REJECT on a result. The left key fills red while you hold; let go early and
  nothing happens. A quick tap only shows a reminder, so a stray press can't
  cost you a collection.
- Holding **both** buttons for 2 seconds still resets the display, and never
  quits a cal.

**The cloud round trip is the same every time.** When a collection ends, the unit
uploads the samples, the server fits them and grades the fit, and the display
shows **Uploading** and then the result: one number, with **GOOD**, **OK** or
**POOR** beside it. **ACCEPT** (BTN2) installs it immediately — no reboot — and
the next screen tells you what's next. Holding BTN1 to **REJECT** keeps whatever
was active before, and costs you nothing but the collection you just did.

Nothing is installed behind your back. A fit you don't accept doesn't take effect.

---

## Step 0 — Gyro and accelerometer (automatic)

**Why.** The gyro has a small constant bias — it reports a slow turn when it's
sitting perfectly still — and left uncorrected that bias integrates straight into
drifting heading. The accelerometer is how the unit knows which way is down, which
is what makes the heading *tilt-compensated*: it's what lets you read a correct
heading while the scooter is nose-down at 20°. Both are easy to measure and
neither needs a decision from you, so the unit just does them.

**Do this.** Nothing, usually. On a brand-new unit with no calibration files,
these two run automatically at first boot: 10 seconds stationary for the gyro,
then six orientations for the accelerometer, each held for a couple of seconds
while it samples. Put the unit on a stable, level surface and don't bump it.

Two things to know:

- **This runs over the USB serial port**, not the display. If a new unit powers up
  but never connects to the display, plug in USB and expect to walk through this
  sequence.
- The six orientations are nose-up, nose-down, resting on its left side, resting
  on its right side, upside down, and level. If you're not sure which physical
  face is which, the `accel_orient` serial command names the orientation it
  currently sees as you rotate the unit in your hand — see
  [dev-calibration-guide.md](dev-calibration-guide.md#serial-diagnostic-commands).

Magnetometer calibration is *not* automatic. There's no sensible mag cal a unit
can run on its own, so a new unit runs with no magnetic correction at all until
you do steps 1 and 2.

---

## Step 1 — Baseline (`CAL > Baseline (1)`)

### Why

This is the full-sphere fit of the unit by itself: hard iron and soft iron, the
magnetic distortion that the device carries around with it wherever it goes. It
has to be done with the unit **off the scooter** for two independent reasons.

First, coverage. Finding an ellipsoid's centre and shape needs readings from all
over its surface, which means pointing the sensor in every direction and rolling
it every way at each one. That's a job for two hands and five minutes; it is not
something you can do to a mounted scooter.

Second, separation. Keeping the scooter out of this fit means the result describes
the *unit*, and nothing else. It stays valid across a battery change, a re-mount,
or a different scooter — all of which only invalidate step 2.

### Do this

1. Take the unit **off** the DPV and go somewhere magnetically clean.
2. Menu (BTN1) → **CAL > Baseline (1)**.
3. The screen opens on axis-range bars reading **Finding north...**. Tumble the
   unit every which way — roll, pitch, yaw, and the diagonals. **A spin about one
   axis alone will never lock**, because a single ring of readings can't tell the
   unit where the centre of the field is. After about 5–10 seconds of real
   tumbling the unit has enough of a fit to know roughly where north is, and the
   screen switches to the coverage grid with every cell as a target.
4. Steer into the red and yellow cells (see
   [the grid](#reading-the-grid), below — it's the same grid gap fill uses).
5. Press **FINISH** (BTN2) when you've got good coverage. A full grid does *not*
   end the collection; the only automatic finish is the sample buffer filling at
   2160, which the counter top-right counts toward and the status line warns you
   about from around 1944.

A pattern that works well:

- Hold the unit by one end, pointed straight down. Raise it slowly through level
  to straight up. Rotate about 15–20°. Lower it slowly. Keep going until you've
  come all the way round the circle.
- Now rotate the unit 90° about its long axis, so that when you raise it to
  horizontal it's lying on one side. Do the same circle of up-and-downs again.
- Rotate 90° again. Another full circle.
- And once more. Four circles, four rolls.

**Aim for about 1400 samples.** Under 1000 will leave real gaps. Over 1800 is
mostly redundant for a baseline — you're collecting more data with no idea whether
it's the data you're short of. Get the fit graded and run gap fill rather than
tumbling indefinitely.

### After collecting

With WiFi, the samples upload and the result comes back in a few seconds as an
RMS percentage with GOOD / OK / POOR. **ACCEPT** (BTN2) installs it on the spot
and *also* downloads the gap-fill map for the new baseline, so the unit knows
which cells to ask you for. The **Installed** screen then tells you how many cells
need work and what's next: **1a FILL GAPS**, or straight to **2 MOUNTED** if
coverage is already good.

If the map couldn't be downloaded the screen says why and points you at
`tern.local`'s **Check for updates**. Either way the old map is deleted, so gap
fill can never run against the previous baseline's cells.

Optionally, on map.terndiving.com, open the device's **Calibration History** and
look at the coverage heatmap. You should see mostly tolerable coverage of the
sphere with some obvious weak regions — which is exactly what gap fill is for.

*(TODO: screenshot of the coverage heatmap)*

**No WiFi?** The display says **No WiFi** and nothing is fitted. The raw CSV is
saved to the unit regardless, so you can upload it later from `tern.local`, or
run the offline Python fit and upload `mag_base.json` yourself — see
[dev-calibration-guide.md](dev-calibration-guide.md#running-baseline-calibration).

---

## Step 1a — Gap fill (`CAL > Fill gaps (1a)`)

### Why

A hand-tumbled sphere is always lumpy, and the lumps are invisible while you're
making them. Once the server has actually fitted your baseline, though, it can
grade the coverage properly: it divides the sphere into 60 cells (12 headings ×
5 elevation bands) and flags the ones that came out thin or empty. Gap fill shows
you those specific cells, walks you into them, and merges the resulting patch back
into your existing baseline — so you're refining a fit rather than starting over.

There's a second kind of gap it closes, which is worth understanding because the
screen will ask you to do something that looks pointless otherwise. The grid
tracks *where the sensor is pointing* — heading and elevation — but deliberately
not how the unit is **rolled** about that axis. That roll-invariance is exactly
what makes tilt-compensated heading work. But it means a cell can read completely
full while every single sample in it came from the same roll: a real blind spot in
the fit that the main grid can't show. So each cell also grades roll diversity
across four poses — upright, on its right side, upside-down, on its left side —
and a cell only finishes when it has all four.

Still **off** the scooter: this patches the baseline, and the baseline is the
unit by itself.

### What you need first

- A baseline calibration must already be installed on the unit.
- The unit needs the server's target map for that baseline. Accepting the
  baseline on the unit downloads it automatically. If you accepted it on the
  website instead, merged a previous gap fill there, or the Installed screen said
  the map wasn't updated: on `tern.local`, under **Calibration Cloud Sync**,
  press **Check for updates**. Every press refreshes the map, not just the first.
  If the map can't be synced, the reason appears in the result text on that page.

Without both, **Fill gaps** refuses with **Can't start** and the reason. Nothing
was collected, so there's nothing to retry.

### Reading the grid

```
 1 [1a] 2  3  FILL GAPS                     4/7
     N           E           S           W
 60° [  ][  ][  ][▓▓][  ][  ][  ][  ][  ][  ][  ][  ]
 30° [  ][██][  ][  ][  ][  ][  ][  ][  ][  ][  ][  ]
  0° [  ][  ][  ][  ][  ][  ][  ][  ][  ][  ][  ][  ]
-30° [  ][  ][  ][  ][  ][  ][  ][  ][▓▓][  ][  ][  ]
-60° [  ][  ][  ][  ][  ][  ][  ][  ][▓▓][▓▓][  ][  ]
3 cells left        284° -37°              [\/]
──────────────────────────────────────────────────
Fill red & yellow cells
 ( hold: QUIT )            (    FINISH     )
```

*(TODO: screenshot of the gap-fill screen)*

The grid is every orientation the sensor can point: 12 heading columns of 30°
across, five elevation rows down — straight up at the top, level in the middle,
straight down at the bottom. Only the flagged cells are your job:

| Cell colour | Meaning |
|---|---|
| Dark gray | Not a target. The server is happy with this cell; leave it alone. |
| Red | Target, no samples yet. |
| Yellow | Target, partly filled. The number in the cell is its sample count. |
| Green | Target finished, including its roll coverage. |

- The **white border** marks the cell you're pointing into right now; the **white
  dot** shows where inside that cell. Steer the dot into a red or yellow cell and
  hold it there.
- **`4/7`** top-right counts finished targets against the targets for this
  session — not the whole grid. **`3 cells left`** is the same thing the other way
  round.
- **`284° -37°`** is the heading and elevation you're pointing at.

**The orientation box** (lower right) is the roll coverage for whichever cell
you're currently in: a square split into four triangles — top is upright, right
is on its right side, bottom is upside-down, left is on its left side. They use
the same colours as the grid (green covered, yellow partial, red none, dark gray
no live orientation), and the **white outline** marks the triangle matching your
current roll. Some may already be green from your earlier baseline.

A cell only turns green once all four triangles are green. **So if the grid says
you're in the right cell but the box is still showing red, keep pointing the same
way and roll the unit onto the side the box is asking for.** Upright needs about
3× the samples of the other three, since normal handling is upright-biased
anyway and the other three only need enough angular diversity to break the
blind spot.

### Tips that matter

- **Pause, don't sweep.** While you're moving the unit, the accelerometer reads
  your hand's motion as part of "down", so the grid and the roll box lag and
  jump. Get near the target, *then* hold still.
- **Top and bottom rows: aim for about 65°, not straight up or down.** This is
  the single most useful technique in the whole step. The outer rows cover
  everything past ±60°, so the instinct is to point straight up and spin — and
  that scatters your samples across the entire top row instead of finishing one
  cell. Two things go wrong at once near vertical: roll is read from the
  component of gravity *across* the pointing axis, which has almost vanished
  (0.04 g at 88°, i.e. mostly noise), and heading gains a `tan(elevation)` factor
  so that a 3° hand tremor sweeps you across three whole heading columns:

  | Holding at | Roll signal left | Roll reading noise | Heading smear from a 3° wobble |
  |---|---|---|---|
  | 65° | 0.42 g | ±1.4° | 6° — a fifth of a column |
  | 80° | 0.17 g | ±3.3° | 17° — half a column |
  | 85° | 0.09 g | ±6.5° | 34° — a full column |
  | 88° | 0.04 g | ±16° | 86° — three columns |

  At 65° you're still inside the top row, with heading and roll both roughly
  **12×** steadier than at 88°. Point up — but not all the way. (This is
  geometry, not a defect in the unit: near vertical there is genuinely almost no
  gravity left across the pointing axis to read roll from.)
- **FINISH (BTN2) ends it early.** Collection finishes on its own when every
  target is green, but some targeted cells may be physically unreachable the way
  your unit is built. Take what you can get and press FINISH.

### After collecting

1. The patch uploads automatically. The **Uploaded** screen is information only,
   with no accept/reject — a patch is never installed by itself, because on its
   own it's a fit of a handful of cells. Press **OK**.
2. On Dive Map, open the device's **Calibration History**. Your latest baseline
   row shows a **"New collection available"** badge. Expand it to see what the
   patch covers — a coverage thumbnail, which gaps it fills, the sample count —
   then combine it. That produces a new pending baseline, which you review and
   **Accept** like any other fit.
3. On `tern.local`, press **Check for updates** to pull the merged baseline onto
   the unit. **Until you do this, the unit is still running the old one.**
4. If you gap-filled an installation that *already* had mounted and heading cals,
   redo **Mounted (2)** and then **Heading (3)** — you just changed the baseline
   they were fitted on top of. In the recommended order you haven't done them yet,
   so there's nothing to redo.

Two rounds of baseline + gap fill to reach a good number is normal, not a sign
something is wrong.

---

## Step 2 — Mounted (`CAL > Mounted (2)`)

### Why

Now the unit goes on the scooter, in its real installed position, with the
motor, batteries, and everything else magnetic present and at operating distance.
This step measures what the scooter does to the field.

You can't tumble a scooter, so this is not another sphere fit — it's a
**constrained** fit layered on top of the baseline. Because the baseline already
accounts for the unit's own distortion, this pass only has to solve for the
additional correction the scooter introduces, which is far fewer unknowns and so
needs far less coverage: three pitch rows (level, nose up 30°, nose down 30°) of
twelve headings each.

And the limited range of motion is a feature, not a compromise. The attitudes you
can reach on a stand are the attitudes you ride in. A calibration fitted over
your actual dive envelope is more accurate *in that envelope* than one smeared
over a full sphere you'll never visit.

One more thing explains the screen's behaviour: **each cell is filled by one
steady hold, not by swinging through it.** The fit takes "down" from the
accelerometer, so any sample captured while the scooter is in motion carries a
false tilt and has to be thrown away. Sweeping past a cell records nothing at all.

### The screen

```
 1  1a [2] 3  MOUNTED                       9/36
     N           E           S           W
+30  [  ][██][  ][  ][  ][  ][  ][  ][  ][  ][  ][  ]
  0  [██][██][██][██][██][██][██][  ][  ][  ][  ][  ]
-30  [  ][  ][  ][  ][  ][  ][  ][  ][  ][  ][  ][  ]
┌────────┐  MOTION [still                    ]
│  sky   │  HOLD   [██████████████           ]
│[--●--] │
│ ground │  210° P+02 R-01
└────────┘
──────────────────────────────────────────────────
Holding...
 ( hold: QUIT )            (    FINISH     )
```

*(TODO: screenshot of the mounted cal screen)*

**The grid** is three rows of 12 heading cells. **`9/36`** counts recorded cells
against all 36 — every cell is asked for. Green cells are done, red cells are
still open. The **highlight** marks the cell you're in: **white** when your
attitude is inside that row's window (so the cell can fill), **yellow** when the
row is merely the nearest one and a hold can't start yet. The **white dot** shows
where you are inside the cell.

**The artificial horizon** (lower left) works like an aircraft's: it tilts with
roll and slides with pitch. It shows your error from *the row you're working on*,
exaggerated 2× so it's readable from the far end of the scooter — it is not your
raw attitude. The black mark in the middle is the nose. The **brackets** mark how
much error is allowed; get the white horizon line between them and level, and
they turn from yellow to **green**.

- Level row: within 10° of level, pitch and roll.
- Tilted rows: nose up or down 20–40°, within 15° of roll.

**The MOTION bar** must be **empty**. The word inside says what's moving: `still`,
`turning` (gyro), or `pushed` / `shaking` (accelerometer). If it won't empty with
the scooter sitting untouched on its stand, that sensor's calibration is off —
that's not your hands.

**The HOLD bar** fills in cyan while the unit settles and records. Full means the
cell is recorded.

| Status line | What to do |
|---|---|
| `Level it` / `Nose up to 30` / `Nose down to 30` | Pitch is outside the row's window. |
| `Level the roll` | Pitch is fine; roll isn't. |
| `Turn a bit more` | You're within 3° of the boundary between two cells. A hold won't start there. |
| `Hold still` | Attitude is right, but the unit is moving. Watch the MOTION bar. |
| `Holding...` | Recording. Don't move until the HOLD bar is full. |
| `Got it - turn` | Cell done. On to the next. |
| `Row done - tilt` / `Row done` | Row complete. Move to a tilted row. |
| `Done - saving` | All 36 cells in, or you pressed FINISH. |

### Do this

The rhythm for every cell is: **aim the dot at the centre of the next box, let go,
wait for MOTION to empty, wait for HOLD to fill.** Aim at the centre, not the
edge — near a boundary the unit refuses to start a hold.

1. Mount the unit on the DPV in its normal installed position, everything
   magnetic present and at operating distance. Put the scooter on its stand.
2. Menu (BTN1) → **CAL > Mounted (2)**.
3. **Level row first, all 12 headings.** Turn about 30°, let go, wait for green,
   repeat.
4. **Then each tilted row, all 12 headings.** Pitch the nose up (or down) until
   the brackets go green, then the same rhythm.
5. At 36/36 — or whenever you press **FINISH**, available once at least one cell
   is in — the samples upload and the result comes back.

If the server can't fit what you gave it, the screen says **Fit rejected** with
the reason. The upload itself worked, so collect again rather than retrying.

**No WiFi?** Same as baseline: the CSV is saved, upload it later from
`tern.local`, or run the offline fit and upload `mag_mount.json` — and note that
the filename matters on that path, see
[dev-calibration-guide.md](dev-calibration-guide.md#running-mounted-calibration).

Two runs to get a good mounted fit is normal.

---

## Step 3 — Heading (`CAL > Heading (3)`)

### Why

Even with both magnetic stages installed, 5–20° of heading error typically
survives. That's not a failure of the previous steps — it's the part of the
problem they can't represent. An ellipsoid fit can correct a field that's been
offset and stretched; it can't correct the lumpier distortion a real scooter
produces, with iron that saturates and structure that isn't symmetric.

What's left over has a useful property: it depends only on **which way you're
pointing**, and it's smooth and repeatable. That makes it a curve, and a curve
can be fitted from samples. You take twelve readings at 30° intervals around the
circle, the server fits a Fourier series through them (1–4 harmonics, chosen
automatically), and the residual error typically drops to around 2°.

This is also the step that ties the unit to the outside world. Steps 1 and 2 make
the unit internally consistent — nothing in either of them knows which direction
is actually north. Here you tell it, twelve times, using your compass. Which is
why your compass has to be right first.

### Do this

**First, check your compass.** Go back and read
[Before you start](#before-you-start) if you skipped it. This is the step where a
bad reference silently becomes a bad instrument.

1. Menu → **CAL > Heading (3)**.

   ```
    1  1a  2 [3] HEADING                    0/12
   Align to external compass
   TARGET                              000°
   INDICATED HDG                        004°
   ┌────────┐  HOLD [██████████████        ]
   │horizon │  P+0.8 R-0.4
   └────────┘  Avg err --
   ─────────────────────────────────────────
   Ready - press MARK
    ( hold: QUIT )            (     MARK      )
   ```

   **TARGET** is the heading to line the DPV up on, by your compass. **INDICATED
   HDG** is the unit's own uncorrected reading — smaller and gray on purpose.
   **Do not try to make the two numbers match.** The difference between them is
   the entire measurement; your job is only to aim accurately at TARGET.

2. Align the DPV to **0° (North)** on your calibrated compass, **level** and
   still. Let the compass settle and wait for the heading readout to stop moving.

   *Why level matters:* the correction is a function of heading alone, so it has
   no way to tell an attitude change from a heading error. A point marked while
   tilted is fitted as if it were heading error at that heading, and it poisons
   the curve. That's what the gate is for — the status line reads **Not level**
   (red) until pitch and roll are both within 3°, then **Hold still** (yellow)
   while it settles, then **Ready - press MARK** (green) with the MARK key lit
   green.

3. Press **MARK** (BTN2). It does nothing until the key is green. **MARKED**
   flashes with that point's error, then the next target appears: 030°, 060°, and
   so on through all twelve.

4. After the twelfth mark, the samples upload and the result comes back in about
   15–20 s:

   ```
    1  1a  2 [3] HEADING                  RESULT
   1.8°                                    GOOD
   likely worst error
   ─────────────────────────────────────────
   Press ACCEPT to install
    ( hold: REJECT )          (    ACCEPT     )
   ```

   That number is the **likely worst error**, not the average and not how neatly
   the curve threads its own twelve points. It's a leave-one-out estimate: each
   point measured against a curve fitted *without* it, which is a fair guess at
   how wrong the unit will be at a heading the calibration never saw. GOOD is
   under 5°, OK is 5–8°, POOR is 8° and up.

   **ACCEPT** (BTN2) installs it immediately, no reboot.

5. Press **OK** on the **Installed** screen to go back to navigation.

**No WiFi?** You'll see **No WiFi**; the CSV is saved either way. Run
`python tools/fourier_fit.py hdg_samples.csv`, upload `hdg_fourier.json` to the
unit, and reboot.

### If one section of the circle comes out thin

Twelve evenly-spaced *targets* don't necessarily produce twelve evenly-spaced
*readings* — if the deviation is large in one region, the indicated heading
swings fast across it and leaves a hole in the data where the fit has nothing to
constrain it. The server checks for this automatically and flags gaps wider than
45°, and Dive Map shows a **"N thin sector(s)"** badge on that calibration.

You don't need to redo all twelve points. A small web form fills just the gap:

1. On Dive Map, open the device's **Calibration History** and expand the flagged
   row. It names the gap and suggests bearings to steer, e.g. *"Between your 337°
   and 066° points the compass swung 90° — steer 015°, 050°."* Those are bearings
   to steer, not readings to produce.
2. **On the unit, set DISPLAY > Heading to RAW.** The fit needs the uncorrected
   magnetic heading, which is what RAW shows — the digits turn yellow with an `R`
   suffix so it can't be mistaken for a navigable heading. Readings taken in TRUE
   or MAG mode will make the calibration *worse*.
3. For each suggested bearing: aim the DPV at it with the same compass you used
   originally, read the indicated heading off the display, and type the
   `(actual, indicated)` pair into the form.
4. Submit. The server combines your typed rows with the original upload and
   refits, producing a new pending calibration.
5. Review and **Accept** it on the website.
6. **On `tern.local`, press Check for updates.** Accepting on the website only
   updates the database — this is the step that actually pulls it onto the unit.
   Skip it and the website will show an accepted calibration the unit isn't
   using.

RAW isn't sticky and reverts to MAG on the next boot, but set it back when you're
done anyway.

A thin sector usually means an unusually poor baseline or mounted cal underneath.
If steps 1–2 went well, heading cal should only be smoothing 3–4° at the worst
heading, and the flag shouldn't trigger at all.

---

## Step 4 — Speed (`CAL > Speed cal`)

### Why

Position is `heading × speed × time`. Steps 1–3 bought you the heading; this one
buys you the speed. The flow sensor counts impeller pulses, and converting pulses
to speed needs a **k-factor** — pulses per litre — that depends on your specific
impeller and intake geometry. The unit measures it the only honest way available:
you travel a known distance, it counts the pulses, and it divides. Elapsed time
cancels out, so only distance and pulse count matter.

This step is independent of everything magnetic. Do it whenever you're next in
the water.

### Do this

1. Menu → **CAL > Speed cal**. A distance-selection screen appears.
2. **BTN1** cycles 150 → 200 → … → 500 ft; **BTN2** confirms.
3. **Start the DPV.** Timing begins automatically once flow exceeds about
   0.3 m/s. A large elapsed-time counter appears.
4. Travel the selected distance in a straight line at your normal cruising speed.
5. The run ends by itself when flow drops below ~0.08 m/s, or when heading changes
   more than 90° after at least 30 s of steady heading — so crossing the finish
   and swinging the scooter is a clean way to signal you're done. Runs under 5 s
   are discarded.
6. On the result screen, **BTN1** cycles the choice and **BTN2** confirms:

   | Choice | When |
   |---|---|
   | **ACCEPT** | A normal run. Adds to a rolling average of up to 6 measurements. |
   | **RESET+ACCEPT** | First cal on a new installation, or after the DPV has been serviced or the impeller changed. Clears the history so measurements from different hardware don't pollute the average. |
   | **REJECT** | The run was suspect — aborted early, speed changed mid-run. |

The k-factor updates immediately.

### Tips

- A calm straight stretch of at least 150 ft: a pool lane, a wall, a pier.
- **You don't need the scooter for this.** Duct-tape the unit to a stick and drag
  it through shallow water while wading a measured distance.
- Longer is more accurate.
- After 3–6 runs the rolling average settles down. Use ACCEPT each time.
- Use RESET+ACCEPT after service, not ACCEPT — otherwise the new hardware's
  measurements get averaged with the old hardware's.

---

## Step 5 — Motor offset (optional)

### Why

Heading cal is a dry bench procedure with the motor off, because that's the only
way to get twelve careful, level, still readings. A running motor adds its own
magnetic field, which shows up as a roughly constant heading offset — it doesn't
much depend on which way you're pointing or how fast the motor is turning. One
number corrects it.

Expect it to be small. If it comes back large, suspect the measurement before you
install it: a single bad leg or a heading error elsewhere in the dive can
masquerade as a motor offset.

### Do this

Run reciprocal legs on a known bearing with the motor running, and compare
indicated heading to expected. Or, more easily, let Dive Map do it: upload a dive
with reciprocal legs between two known positions, and the track correction reports
the heading offset directly. The mechanics of writing `motor_cal.json` are in
[dev-calibration-guide.md](dev-calibration-guide.md#motor-on-heading-correction).

---

## Temperature (optional, advanced)

The magnetometer's zero point moves with its own die temperature — about
0.68 µT/°C, which can be as much as a couple of degrees of heading per degree C.
That's why [Before you start](#before-you-start) asks you to calibrate near 21 °C:
without compensation, a calibration is only accurate near the temperature it was
collected at, and a cal done in a warm living room then used in a cold lake will
have drifted.

The unit handles this with a per-device coefficient file. When it's installed,
every raw reading is normalized to 21 °C before any calibration is applied —
including during sample collection — so a cal collected warm and a dive done cold
line up anyway.

Fitting those coefficients takes a multi-hour unattended heat test, so it isn't
part of the normal calibration chain. The procedure and the file format are in
[mag-temperature-compensation.md](mag-temperature-compensation.md).

---

## What good looks like

A real reference run on this hardware, start to finish:

| Step | Result | Notes |
|---|---|---|
| Baseline + gap fill | **1.2% RMS** | Took **two** rounds to get there |
| Mounted | **6% RMS**, graded *marginal* | Two runs. Still the best mounted number this unit has produced |
| Heading, pre-correction | 4–6° raw error all the way round | Well within what the Fourier fit absorbs |
| Heading, final | **<1° on the unit, 1.6° max on the website** | |
| Spot check at the four cardinals | within ~1° | Which is the resolution of the reference compass |

Two things to take from that. First, **needing a second round is normal** — at
both baseline and mounted. Don't treat the first attempt as the attempt. Second,
**a mounted fit graded "marginal" still produced sub-2° heading.** The mounted
grading bands are conservative, and the number that actually matters to you is
the heading result at the end of the chain.

---

## Checking a finished calibration

Do this the night before a dive, and any time you suspect something's moved.

**The 4-point check.** Set **DISPLAY > Heading** to **MAG** — your compass reads
magnetic, so compare like with like. Point the DPV at N, E, S and W on your
calibrated compass and compare each against the display.

- Each should agree within a couple of degrees.
- **Any cardinal off by more than ~5°:** redo **CAL > Heading (3)**.
- **Errors large (>10°) or inconsistent between cardinals:** the problem is
  underneath the heading cal. Redo **CAL > Mounted (2)** first, then heading.
- **Every cardinal off by about the same ~15°:** you're comparing TRUE against a
  magnetic compass. Set the display to MAG. This is by far the most common cause
  of "everything is off by the same amount" — the unit always computes true
  heading internally, and the display setting is what converts it.

Set the heading display back to whatever you navigate in when you're done.

**If you have a motor offset installed, expect it to show up here.** The motor
correction is added to the *displayed* heading, but heading cal records the
reading from before it is applied — so the motor offset is invisible to the fit
and appears as a constant bias in this check, even on a perfectly good
calibration. Either expect it and subtract it, or zero it while you're checking.

One honest caveat: this check compares the unit against the same compass the
calibration was built from, so it can't detect an error in that compass. It
verifies that the calibration took, not that it's pointed at true north. For that
you need an independent reference — a sight line to a landmark on a known bearing.

---

## When to recalibrate, and what invalidates what

Because each step is fitted on top of the one before it, changing something early
invalidates everything after it. The rule is simple: **redo the step that changed,
then everything downstream of it.**

| What changed | Redo |
|---|---|
| New baseline accepted | Mounted, then Heading |
| Gap-fill patch merged into the baseline | Mounted, then Heading |
| New mounted accepted | Heading |
| New heading accepted | Nothing downstream |
| Unit re-mounted, moved, or re-clocked on the scooter | Mounted, then Heading |
| Battery pack changed or moved; light canister added; power wiring re-routed | Mounted, then Heading |
| Different scooter | Mounted, then Heading (the baseline still holds) |
| Unit opened, hardware changed inside | Baseline, then everything |
| DPV serviced, impeller changed | Speed cal, with **RESET+ACCEPT** |
| 4-point check fails | See [above](#checking-a-finished-calibration) |

A note on power wiring in particular: the field from a current-carrying wire
depends on the load, so it isn't constant and a calibration can't absorb it.
Route power away from the sensor rather than trying to calibrate it out.

---

## When it goes wrong

| Symptom | Likely cause | What to do |
|---|---|---|
| Every heading off by about the same 15° | Comparing TRUE against a magnetic compass | Set **DISPLAY > Heading** to **MAG** when checking against a compass |
| Heading off by a consistent amount (not ~15°) | Residual bias | Redo **Heading (3)** |
| Heading error changes with tilt | Soft iron not fully corrected | Redo **Mounted (2)**, then **Heading (3)** |
| One region of the compass is much worse than the rest | Thin sector in the heading fit | Check Dive Map for a "thin sector(s)" badge; use the web form rather than redoing all 12 points |
| Baseline never leaves "Finding north..." | Spinning about one axis only, or a magnetically noisy room | Tumble through all three axes; move somewhere cleaner |
| Mounted MOTION bar won't empty on a still stand | Gyro or accel calibration is off | Not your hands — see [Step 0](#step-0--gyro-and-accelerometer-automatic) |
| Mounted cells won't fill although the attitude looks right | You're sweeping, or sitting on a cell boundary | Aim at cell centres, let go, wait for MOTION to empty |
| **Fill gaps** says "Can't start" | No baseline installed, or no target map | Accept a baseline on the unit, or press **Check for updates** on `tern.local` |
| Website shows an accepted calibration the unit isn't using | Web-side accepts don't push to the device | `tern.local` → **Calibration Cloud Sync** → **Check for updates** |
| Fit quality is poor in a clean room | Complex DPV magnetic geometry, or something loose and ferrous on the scooter | Check for loose magnetic components; redo the step |

For anything deeper — the file formats, the Python fitting tools, the serial
diagnostics, forcing recalibration by deleting files — see
[dev-calibration-guide.md](dev-calibration-guide.md).
