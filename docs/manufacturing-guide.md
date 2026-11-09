---
title: "DPV-Nav Manufacturing Guide"
description: "Choosing a build variant and making the parts for a Tern DPV-Nav dead-reckoning navigation system."
hardwareRev: "v0.x"
firmwareMin: "0.7.x"
lastUpdated: 2026-11-09
draft: true
---

# DPV-Nav Manufacturing Guide

**Volume 1 of 3.** This volume covers everything up to the point where you have a
box of finished parts: deciding what to build, ordering it, and making it.

| | |
|---|---|
| ← Before this | [System Overview](overview.md) — what it is and why it's shaped this way |
| → After this | [Assembly Guide](assembly-guide.md) — solder, wire, seal, flash |
| → Then | [User Guide](user-guide.md) — calibrate and dive |

Callout conventions are defined in the [docs index](README.md#callout-conventions).
A **📝 TBD** block marks a section that is outlined but not yet written.

---

## 1. Read this first

### 1.1 Safety

> **⚠ Safety** — DPV-Nav is a dead-reckoning aid, not a precision navigator, and
> not a substitute for a dive plan. It has no idea what your plan is, how much gas
> you have, or whether you're on the guide line. If it floods, the screen goes dark
> and you are on your own for the trip back to the beach.
>
> Don't blindly follow DPV-Nav *anywhere*, let alone somewhere your better instincts
> as a technical diver tell you not to go. Dive within your limits and your
> training, and don't let a piece of bolt-on hardware talk you into something dumb.
>
> **Explicitly: you assume all risk.** See [SAFETY.md](../SAFETY.md).

### 1.2 What this guide is and isn't

This is the "how to build your own" guide, and it comes with absolutely no
guarantee. It works for the reference build, and I hope it works for yours, but I
make no guarantee of any kind that it does. Expect at least some level of discovery
and debugging along the way. This is a DIY hobbyist item, not a polished commercial
product.

### 1.3 Who this is for

If you're building from scratch, you should be a reasonably accomplished builder with
a broad swath of manufacturing capability.

If you're outsourcing parts of the process to friends with more skills, that's fine
and expected — you may not own a soldering iron, but if you have a savvy friend,
that's a great answer. Not everyone has a 3D printer, but almost everyone knows
someone who does.

### 1.4 License and reuse

Tern DPV-Nav is source-available and free for noncommercial use. Hardware designs
and documentation are under CC BY-NC-SA 4.0; firmware is under PolyForm
Noncommercial 1.0.0. Build one, modify it, share your improvements. Commercial use
requires a separate license.

See [LICENSE.md](../LICENSE.md) for what covers what, and
[CONTRIBUTING.md](../CONTRIBUTING.md) if you'd like to contribute back — build
reports and troubleshooting entries are especially welcome.

---

## 2. What you're building

> **📝 TBD** — hero section. Lead with photos, minimal text:
> - 3–5 photos of the completed system: on the bench, mounted to the DPV, display
>   in-hand, in-water if available
> - optional short video of it running
> - capability summary in plain language, 4 or 5 bullets
> - spec table: depth rating, runtime, position accuracy (state the test conditions
>   honestly), update rate, dimensions, mass in air / in water
> - cost and build-time ranges by variant, as a teaser for §4

For what the system does and why it's split into four modules, see the
[System Overview](overview.md).

**→ Bill of Materials** — see [§5](#5-bill-of-materials). Link out early; people
price the build before they read anything else.

---

## 3. Before you start

### 3.1 Skills you'll need

- **Soldering** — all of it is through-hole or wire junctions. There is no SMD
  soldering. The smallest components are the PicoBlade connectors, which take some
  finesse but no special equipment.
- **Heat shrink** — in most cases the best way to junction wires. You'll need to be
  comfortable sizing and applying it.
- **3D printing**, plus the follow-on work that makes printed parts actually useful
  on your setup. On the reference printer, parts often need a little trimming with a
  utility knife, or sanding of surfaces that came out not quite to spec.
- **Drilling plastic** — 🔁 commercial-housing variant. If you go with the
  BlueRobotics and GoPro housings you'll be drilling holes in plastic, not metal.
  They need to be precisely placed and sized to work (templates are included), with
  clean seating surfaces and no burrs.
- **CNC milling** — 🔁 milled variant. You'll need access to a mill; they're
  surprisingly common. If you're fantastically bold you may be able to mill
  components by hand with a vise and a router, but getting the o-ring grooves right
  will be a challenge.
- **Firmware flashing** — install PlatformIO on a computer with USB ports and be
  comfortable flashing embedded targets (here, a pair of ESP32s).
- **Cable glands** — a significant part of this process. Stripping cables to the
  right length, keeping seating surfaces clean, and applying appropriate torque are
  basic mechanical skills, but they are the skills that keep water out.

This guide walks you through all of it, so being unfamiliar with any one item isn't
necessarily a show-stopper. But be honest that you're learning while you build, and
expect several attempts at whatever parts of the process are new to you.

### 3.2 Tools

> **📝 TBD** — table: tool | required or optional | notes and substitutes. Split
> into electronics bench, fabrication (printer / mill), assembly hand tools, test
> gear (multimeter required; a scope is optional-but-you'll-want-one), and the
> vacuum pump + test plug needed for leak testing in the
> [Assembly Guide §11](assembly-guide.md#11-sealing-and-leak-testing).

### 3.3 Consumables

> **📝 TBD** — solder, heat shrink in several sizes, potting epoxy, o-ring grease,
> thread locker, desiccant. Quantities per build.

### 3.4 Time and cost

> **📝 TBD** — honest ranges per variant. Keep "hands-on hours" separate from
> "elapsed time": PCB fab lead time, print time, potting cure and shipping dominate
> the calendar and none of them are work.

---

## 4. Choose your build variant

Decide this **before** you read the BOM detail or fabricate anything. The choice
cascades into which BOM lines apply, which parts you print, and which assembly steps
you follow. Everything downstream that branches is tagged 🔁.

### 4.1 Housings — commercial vs. milled

| | Commercial (BlueRobotics + GoPro) | Milled Delrin |
|---|---|---|
| Cost | ~$200 (nav) + ~$60 (display) | 📝 TBD |
| Tooling required | None | CNC mill (or buy a kit) |
| Time | 📝 TBD | 📝 TBD |
| Risk | Known-good, tested to depth | You own the pressure analysis |
| Button options | GoPro mechanical | Piezo (Perdix-style) |

BlueRobotics builds high quality gear for serious underwater applications. Their
DIY/hobbyist submersible line is competent equipment designed for significantly
deeper work than most technical divers are capable of, so it is over-engineered for
this application. It's also fantastically convenient — the glands and housings are
well built, easy to work with, and require basically no special skills. That
convenience and engineering come at a cost: the BlueRobotics housing runs about
$200.

The GoPro housing is similarly straightforward: it's already a housing, it works
with commercially available o-rings in a known size, and you drill one hole for the
gland and you're done. But it costs $60, and it locks you into a mechanical button
solution that forces alignment problems into the design.

If you mill your own, you own the precision. The provided model works for the
reference build and it is not my fault if you build it wrong. It's designed to about
a 4:1 safety factor — more to tolerate low-precision milling than out of any belief
that over-design matters here — so the most likely failure mode is a leaky o-ring or
gland rather than a collapsed housing.

### 4.2 Penetrators vs. cable glands

BlueRobotics makes genuinely high quality penetrators. They're great. What's not
great is paying for them: at $13 each and five glands per build, it adds up fast.

- **Cheap but demanding:** the penetrators from `rovmaker.org` look identical to the
  BlueRobotics units for under $5 each.
- **Really cheap:** add chromed-brass glands to your DigiKey order. They're
  typically $3–5 and work well *if installed correctly*.

> **⛔ Stop** — whichever path you take, plan on buying a vacuum test kit. The
> [leak test](assembly-guide.md#11-sealing-and-leak-testing) is the only thing that
> qualifies your choice here, and a cheap gland is only cheap if it holds.

### 4.3 Buttons

This is one of the strongest arguments for the milled housing. The printed GoPro
housing frame is pretty good, but if it slips out of alignment during a dive you
don't get to push any buttons until you can open the housing again later.

The milled housing has room for proper piezo buttons — the familiar Shearwater-style
ones — which are delightfully reliable. The catch: piezo buttons aren't technically
rated for pressure exposure. We're hacking an automotive component into an
underwater application.

### 4.4 Optional modules

**There are none.** Every module is load-bearing:

- No GPS → no time and no initial position → you can't navigate.
- No flow meter → no in-water speed → you can't navigate.
- No IMU → no heading → you can't navigate.
- No display module → you can't control the device or see its output.

> **🔁** Once you've picked, note your variant. Every step tagged 🔁 in this volume
> and in the [Assembly Guide](assembly-guide.md) branches on these choices.

---

## 5. Bill of materials

> **📝 TBD** — keep the canonical BOM as a variant-tagged CSV in the repo
> (`docs/bom/bom.csv`): one file, one source of truth, with columns for
> qty / variant / supplier / PN / link / unit cost.
>
> This section should be a short reader's guide to that file, not a duplicate of it:
> - what's variant-specific vs. universal
> - long-lead items to order first
> - acceptable substitutions, and what **not** to substitute (o-rings, the
>   transducer, the regulators)
> - subtotal by subsystem

---

## 6. PCB fabrication

### 6.1 Ordering the boards

> **📝 TBD** — which boards, how to order them (fab outputs live in
> [`hardware/`](../hardware/)), house rules for the fab, and incoming inspection.
> Include renders from the KiCad layout.

Current board revision: nav board **Rev D**. Schematics and layouts for the nav,
display and GPS boards are under [`hardware/schematic/`](../hardware/schematic/).

> **💡 Note** — a known documentation gap: the PCB files don't yet show the display
> backlight jumper on GPIO4, which the firmware uses to kill the backlight in OFF.
> See [backlog.md](backlog.md) Tier 2.

### 6.2 Incoming inspection

> **📝 TBD** — what to check on arrival, before you solder anything to it.

---

## 7. 3D printed parts

### 7.1 Material

Material selection matters less than you might think. None of the printed parts are
particularly load-bearing and nothing is a pressure boundary. Print what you've got,
then verify the finished work fits correctly into the designed space.

The most structurally significant components are the mounts to the scooter tube, and
even those are more about spacing and alignment than structure.

> **📝 TBD** — compare PETG vs. ASA vs. nylon for the record: water absorption, UV,
> creep under clamp load, and temperature inside a black housing in the sun.

### 7.2 Print settings

> **📝 TBD** — layer height, walls, infill, supports, orientation. Orientation
> matters most for the structural mounts; call it out per part.

### 7.3 Part list

Models are under [`hardware/3D Printed Parts/`](../hardware/3D%20Printed%20Parts/),
grouped as:

- **Nav Module** — housing internals to mount the board, and the BR-housing-to-
  scooter-tube mounts
- **GoPro Display Housing** — internals for the display 🔁
- **Milled Display Housing** — internals for the milled variant 🔁
- **Flow Meter** — the potting mold / holder

> **📝 TBD** — proper table: filename | qty | material | structural or cosmetic |
> notes, grouped by DPV mounts, housing internals (sleds / trays / endcap carriers),
> and flow-meter potting mold.

### 7.4 Post-processing and test fit

General rule: ream holes, install heat-set inserts, and **test-fit everything dry
before you commit.**

**Display housing — button alignment.** This is the single most critical and most
failure-prone part of the whole assembly. Put it all together, then visually confirm
that the buttons in the housing line up with the installed internal mechanical
buttons. If they don't line up, the buttons will not work, and you will not be able
to use the device at depth. Figure out what's keeping them from aligning and adjust
your print accordingly.

> **📝 TBD** — photo of correct vs. misaligned button stack.

**Display housing — display placement.** The second thing worth getting right. On
the reference printer, with all its various inaccuracies, the front piece prints just
right so the display *clicks* neatly into place. If your printer is more
true-to-form you may need to remove a little material, or modify the model to add a
little, at the display mounts.

**Heat-set inserts.** The BlueRobotics housing adapter calls for a pair. Heat up a
soldering iron, place an insert on the end of the iron (without burning yourself),
and press the insert into place in the mounting structure (without burning
yourself). Make sure the inserts go in **straight** — a cockeyed insert means a
cockeyed bolt, which won't hold the board correctly.

**Tube-to-scooter alignment parts.** These will probably need support structure
removed, and depending on how well your printer renders detail, some shaving or
sanding to get them flush. On an elderly Taz3 it takes a fair bit of coercion to get
the bits to align perfectly.

> **⛔ Stop** — this is important to get right. Slop here means slop between the
> direction of scooter travel and the IMU. *Consistent* misalignment is fine —
> calibration absorbs it. **Sloppy** misalignment is not, because it changes between
> dives, and nothing downstream can calibrate around that.

---

## 8. Milled housings 🔁

*Milled variant only.*

> **📝 TBD** — Delrin stock spec, workholding, toolpath notes, o-ring groove
> dimensions (reference an actual standard, don't eyeball it), bore finish, threading
> the endcaps, and which tolerances actually matter vs. which don't.
>
> Record the honest experience too: milling Delrin has been a real challenge on a
> belt-drive machine. That warning is worth more than the toolpaths.

---

## 9. Potting the flow meter

Its own procedure, not a footnote.

Potting is simpler than many manufacturers make it sound. "Fully potted" carries a
significantly higher price tag, but all they're really doing is covering the thing
with epoxy.

If you want to be fancy you can get into molded pottings and expensive epoxies
designed for exotic purposes. In reality, what you need is an epoxy that bonds
effectively to whatever your flow meter is made of. On the reference build that was
ABS plastic, which makes bonding incredibly easy — wipe it down with acetone, then
just epoxy.

**The goal:** a solid, consistent, thick layer of epoxy over any part of the flow
meter housing that could admit water. Depending on the model you get, that's
primarily where the wires enter the housing, plus the wiring path itself.

**Technique.** Mix a series of small batches rather than trying to do it all at
once. Put a little epoxy in the bottom of the box, let it harden, then press the
flow meter into it. Clamp it in place, then slowly, over several batches, fill the
space around the housing.

**How to know you're done.** Imagine you're water, trying to find a path to the
electricity. You're creative, patient and strong. If you can figure out a way to get
to the electricity, water will manage it too. Give up and re-pot until you're sure
it'll hold.

> **📝 TBD** — numbers and failure modes: mold prep and release agent, resin choice
> and mix ratio, degassing, pour technique to avoid voids, cure time and
> temperature, what a void does to you at 150 ft, how to inspect for one, what a
> failed pot looks like, and when to scrap it.

> **✅ Checkpoint** — all parts printed and/or milled, test-fit dry, potting cured
> and inspected. Nothing electronic has been touched yet.
>
> **→ Continue to the [Assembly Guide](assembly-guide.md).**

---

## Appendices

- **A. Print settings summary** — 📝 TBD, one-page reference
- **B. O-ring and fastener spec** — 📝 TBD, sizes, torque, thread locker
- **C. Hardware revision applicability** — 📝 TBD, what this doc covers and what
  changed between revisions
