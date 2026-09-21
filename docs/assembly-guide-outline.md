---
title: "Build Your Own DPV-Nav"
description: "Complete assembly guide for the Tern Diving DPV-Nav dead-reckoning navigation system."
hardwareRev: "v0.x"          # doc applies to this hardware revision only
firmwareMin: "0.x.y"
lastUpdated: 2026-09-13
draft: true
---

<!--
FRAMEWORK NOTE — this file is the skeleton, not the content.
Bracketed [[ ]] blocks describe what goes in each section.
Delete them as you fill in.

Proposed file split (repo canonical):

  dpv-nav/docs/build/
    README.md              -> 0. Hub / table of contents (short)
    01-safety-and-scope.md
    02-what-youre-building.md
    03-before-you-start.md
    04-choose-a-variant.md
    05-architecture.md
    06-fabrication.md
    07-electronics.md
    08-wiring.md
    09-mechanical-assembly.md
    10-leak-testing.md
    11-firmware.md
    12-calibration-and-first-light.md
    13-troubleshooting.md
    14-maintenance.md
    99-appendices.md
    bom/bom.csv            -> single machine-readable BOM, variant-tagged
    assets/                -> photos, diagrams (referenced by relative path)

Blog: one long page that concatenates these at build time, OR a narrative
"here's what I built" post that links into the repo hub. See the note at the
bottom of this file.
-->

<!--
CALLOUT CONVENTIONS — define once, use everywhere, style in both renderers.

  > **⚠ Safety** — can hurt you or drown you. Never optional.
  > **⛔ Stop** — do not proceed past this point until the check passes.
  > **✅ Checkpoint** — verification gate at the end of a stage.
  > **💡 Note** — context, rationale, "why it's like this."
  > **🔁 Variant** — this step differs by build variant.
-->

# Build Your Own DPV-Nav

---

## 1. Read this first

[[ Short. Four things: ]]

### 1.1 Safety
[[ This is a dead reckoning tool, not a precision navigator.  Do not assume that it's correct in lieu of other better information.  It's an *aid*, not a substitute for a dive plan.  Remember: Tern DPV Nav has no idea what your dive plan is, how much gas you have, whether or not you're on the guide line, etc.  If it floods, the screen goes dark, and you're on your own for the trip back to the beach.  Don't blindly follow DPV-Nav *anywhere*, let alone someplace your better instincts as a technical diver tell you not to go.  Always dive within your limits and your training, and don't let some bolt-on hardware tell you do do dumb things.

Explicitly: the user assumes all risk. ]]

### 1.2 What this guide is and isn't
[[ This is the "how to build your own" guide.  It comes with absolutely no guarantee.  This works for me, and I hope it works for you, but I make no guarantee of any kind that it does.  Expect at least some level of discovery and debugging in this process.  It's a DIY hobbyist item, not a polished commercial delivery. ]]

### 1.3 Who this is for
[[ If you're building it from scratch, you should be a reasonably accomplished builder with a broad swath of manufacturing capability.  If you're outsourcing parts of the process to friends with more skills, that's fine -- you may not own a soldering iron, but if you have a friend who's savvy, that's a great answer.  Not everyone has a 3D printer, but almost everyone knows someone who does. ]]

### 1.4 License and reuse
[[ One paragraph + link to LICENSE.md and CONTRIBUTING.md. ]]

---

## 2. What you're building

[[ HERO SECTION. Lead with photos, minimal text. ]]

- [[ 3–5 photos of the completed system: on the bench, mounted to the DPV,
     display in-hand, in-water shot if you have one. ]]
- [[ Optional: short video of it running. ]]
- [[ Capability summary in plain language — 4 or 5 bullets. ]]
- [[ Spec table: depth rating, runtime, position accuracy (state the test
     conditions honestly), update rate, dimensions, mass in air / in water. ]]
- [[ Cost and build-time ranges, by variant, as a teaser for §4. ]]

**→ [Bill of Materials](./bom/bom.csv)** — [[ link out early; people price the
build before they read anything else. ]]

---

## 3. Before you start

### 3.1 Skills you'll need
[[ 
   - All of the soldering is through-hole or wiring junctions; there's no SMD soldering.  The smallest components are the picoblade connectors, which require some finesse to get right, but do not require any special equipment.
   - In most cases, the best way to junction wires involves heat shrink tubing; you'll need to be comfortable with applying and using heat shrink tubing.
   - 3D printing and the requisite follow-on work to make 3D printed parts useful from your setup.  In my own setup, for example, parts often require a little trimming with a utility knife, or sanding surfaces that ended up not quite to spec.  
   - If you go with the BlueRobotics and GoPro housings, you'll need to drill holes in plastic (not metal).  Holes do need to be precisely placed and sized in order to work correctly (the guide includes templates), and they'll need to be clean seating surfaces with no burrs.
   - If you go with the milled housings, you'll need to have access to a CNC mill (they're surprisingly common).  If you're fantastically bold, you may be able to mill components by hand with a vice and a router, but getting the o-ring grooves just right will be challenging.
   - You'll need to be able to install PlatformIO on a computer with USB ports, and you'll need to be comfortable flashing firmware to embedded computers (e.g. the pair of ESP32s).
   - Installing cables in glands is a significant part of this process. Stripping cables to appropriate lengths, ensuring seating surfaces are clean, and applying appropriate torque are basic mechanical skills.

   - We'll walk you through all of it in this guide, so if you're unfamiliar with any of it, that isn't necessarily a show-stopper, but you should admit that you're learning while you build, so expect several attempts on any part of the process that you're new at.]]

### 3.2 Tools
[[ Table: tool | required or optional | notes/substitutes.
   Split into: electronics bench, fab (printer / mill), assembly hand tools,
   test gear (multimeter; scope is optional-but-you'll-want-one),
   vacuum pump + test plug for §10. ]]

### 3.3 Consumables
[[ Solder, heat shrink (various sizes), potting compound (epoxy), o-ring grease,
   thread locker, desiccant. ]]

### 3.4 Time and cost
[[ Honest ranges per variant. Separate "hands-on hours" from "elapsed time"
   — PCB fab lead time, print time, potting cure, shipping. ]]

---

## 4. Choose your build variant

[[ This section belongs BEFORE the BOM detail and before fabrication, because
   the choice cascades into which BOM lines, which printed parts, and which
   assembly steps apply. Tag everything downstream with 🔁. ]]

### 4.1 Housings — commercial vs. milled

| | Commercial (BlueRobotics + GoPro) | Milled Delrin |
|---|---|---|
| Cost | [[ ]] | [[ ]] |
| Tooling required | None | CNC mill (or buy a kit) |
| Time | [[ ]] | [[ ]] |
| Risk | Known-good, tested to depth | You own the pressure analysis |
| Button options | GoPro mechanical | Piezo (Perdix-style) |

[[ BlueRobotics builds high quality gear for serious underwater applications.  Their DIY / Hobbyist unmanned submersible is competent equipment, designed for significantly deeper applications than what most technical divers are capable of.  As such, they're over-engineered for this application.  They're also fantastically convenient -- the glands and housings are well-built, easy to work with, and require basically no special skills.  That convenience and engineering comes at a cost, however -- the BlueRobotics housing is something like $200.  Similarly, the GoPro housing is straightforward: it's already a housing that works with commercially available o-rings in a known size.  Drill one hole for the gland and you're done.  But it costs $60, and you're locked into the mechanical solution for buttons that force mechanical alignment issues into the design.

If you mill your own, you're resonsible for precision, obviously -- I'm providing a model that works for me, and it's not my fault if you build it wrong.  I went with about a 4:1 safety factor -- more to allow low precision milling than because I think it's important to over-design -- so the most likely failure mode is a leaky o-ring or gland, rather than a collapsed unit. ]]

### 4.2 Penetrators vs. cable glands
[[ BlueRobotics makes genuinely high quality penetrators.  They're great, and I love them.  But I do not love paying for them. At $13 a pop, times 5 glands, it adds up fast. If you're cheap but demanding, you might try the penetrators from https://rovmaker.org -- they look identical to the BlueRobotics units for <$5 a pop.  If you're really cheap, just add some chromed-brass glands to your Digikey order; they're typically in the $3-5 range, and work well if installed correctly.  Either way: plan on getting a vacuum testing kit to make sure that whatever path you went down actually holds pressure. ]]

### 4.3 Buttons
[[ This is one of the biggest reasons to go with the milled housing, in my opinion.  The printed GoPro housing frame is pretty good, but if it slips out of alignment during a dive, you don't get to push any buttons until you can open the housing again later.  With the milled housing, there's room for proper piezo buttons (like the familiar Shearwater-style buttons), which are delightfully reliable.  On the other hand, the piezo buttons aren't technically rated for pressure exposure -- we're hacking a component from the automotive world into something for the underwater world. v]]

### 4.4 Optional modules
[[ There are no optional subsystems.  Without GPS, you never get time or initial position, so you can't navigate.  If you don't have the flow meter, you never get in-water speed, so you can't navigate.  If you don't have the IMU, you can't get heading, so you can't navigate.  If you don't have the display module, you can't control the device or see the outputs. ]]

> **🔁** Once you've picked, note your variant. Every step tagged 🔁 below
> branches on these four choices.

---

## 5. Bill of materials

[[ Keep the canonical BOM as a variant-tagged CSV in the repo — one file, one
   source of truth, columns for qty/variant/supplier/PN/link/unit cost.
   This section is a short reader's guide to that file, not a duplicate table:
   - what's variant-specific vs. universal
   - long-lead items to order first
   - acceptable substitutions and what NOT to substitute (regulators, o-rings,
     the transducer)
   - subtotal by subsystem ]]

---

## 6. System architecture

[[ There are 4 separate components: Nav, Display, Flow, and GPS, each with its own housing and wiring.  Flow is fully potted and bolts on to the side of the Nav housing.  GPS and Display are both either milled housings or GoPro housings.  (You don't need to make the same decision for both -- you can use a GoPro housing for GPS and mill a housing for the Display.)  The Nav housing is the core, where all the wires end up.  It holds the central processor, the IMU, and the battery.

TODO: block diagram
TODO: photos of each component

]]

### 6.1 Block diagram
[[ Power domains, data buses (I²C / RS485 / whatever), pressure boundaries
   drawn explicitly as dashed lines — makes §8.2 (junctions outside pressure
   vessels) obvious rather than arbitrary. ]]

### 6.2 Module: nav / main
[[ The main module is where most of the math happens, where the tern.local website is served from, and where the IMU and battery live.  The processing all happens on an ESP32 wired to a Pololu AltIMU 9-axis measurement unit.  A depth sensor is installed in the housing to enable dive/surface mode and tracking water temperature and depth.  The battery is a small flat-pack with a non-metallic housing to reduce magnetic signature.  Everything is arranged so that the IMU -- where we're measuring magnetic fields -- is as far from wiring as possible.  Especially important is the power wiring.  If you run power from the battery to the ESP32 past the IMU, you'll absolutely getting heading deflection based on current draw. 

Note the installation orientation of the IMU.  All of the math assumes that you install your sensor the same way I did.  You can argue that I did it wrong, or that it would be better if it was done some other way.  But if you want it to work with this codebase, just install it the same way I did.
]]

### 6.3 Module: display
[[ For initial prototyping, I used GoPro housings.  The GoPro solution requires zero engineering to know that it's rated for depth, they come from the factory with two buttons and a flat rear display, and there's a whole suite of options for mounting the housing on random objects.  However, I was frustrated by the expense ($60 a pop) and the fact that the buttons have to be aligned to mechanical buttons inside the housing.  After losing a number of dives to inability to push buttons, I re-engineered the entire interior framing solution, and that's worked well, but be aware that it can be extremely finicky.

The display module has it's own ESP32.  Initially, it was just an RS485 receiver that drove the EyeSPI board and TFT display.  Over time, numerous functions ended up on the display board, caching data or functionality on the board to eventually send over to Nav for processing or transmission. ]]

### 6.4 Module: GPS
[[ Obviously, the GPS doesn't work underwater, so you may be wondering why it's here, and why it's outside the main Nav housing.  Fundamentally, dead reckoning estimates future position from a known starting point, so fixing a good initial position is key to navigation.
   
Initial prototypes had the GPS module just mounted directly to the Nav board.  This works great, as long as you're on the beach.  It **does not** work when you're at the descent buoy trying to hold the scooter high enough out of the water to clear surface-reflection interference to get a pre-dive fix.  I've played with a couple of different methods of putting GPS in a place where I can unclip it and raise it out of the water, while leaving the scooter-proper fully immersed.  The best fixes came from a powered external antenna connected to the u.fl port on the GPS board, but the connection was sufficiently unstable to make it a non-viable solution for production.  You can cheap out on the GPS board if you'd like -- getting a board that **just** has GPS (no battery) will still give you fixes.  However, I went with the Adafruit Ultimate GPS because the battery and memory setup allows the unit to store ephemeris data, which allows for a much faster fix when it gets signal again. ]]

### 6.5 Module: flow meter
[[ It doesn't seem to matter what flow meter you get, as long as it's roughly in the right range of flow sensitivity, size, and depth rating.  There are a number of options here, and they all look like basically the same thing -- an impeller driving a Hall effect sensor that needs to be calibrated before it's useful.  Whatever flow meter you end up with, you'll need to put it in a box full of epoxy, with the wires coming out of the potting to supply signal to the Nav unit.

It mounts on the side of the Nav unit, with piping on both ends to improve repeatability of flow.  Alignment with the direction of scooter travel is key, of course. ]]

### 6.6 Power
[[ I've opted for a 3mAh flat-pack from Amazon -- they're cheap, I can get them in multi-packs, and they last more than long enough to be workable for this purpose.  The one gigantic caveat is that the polarity on the connection is almost certainly wrong.  **Before you plug in a non-Adafruit battery to an Adafruit ESP32, check and correct the polarity!** 

Note that the battery wires should be as short as possible. They carry enough current to generate a sufficient magnetic field that they'll definitely drive heading impact if they're anywhere near the IMU.  If you keep power at the back of the board, you'll be ok.
]]

### 6.7 Why it's split this way
[[ I wanted to set things up with as few pressure boundaries as possible.  The initial concept design had everything in one housing.  But that just didn't work.  The IMU needs to be aligned to the scooter tube, which means it has to be mounted **on the tube**, or you're doing some wonky alignment process on the handle.  The display needs to be someplace you can see it, which means either a giant thing on the tube, or a sane-sized display on the handle.  If the flowmeter is inside the housing, you end up with an unnaturally large housing with plumbing flowing through it -- which is actually a more complex pressure boundary than just potting it and putting it outside.  GPS needs to be well above the water line to get a fix, and I'm not strong enough to hoist my 52lb Cuda 650 far enough out of the water to get signal, so it ends up on a cable. ]]

---

## 7. Fabrication

### 7.1 3D printed parts
- [[ Material: PETG vs ASA vs nylon — pick and justify (water, UV, creep under
     clamp load, temperature in a black housing in the sun). ]]
- [[ Print settings table: layer height, walls, infill, supports, orientation.
     Orientation matters most for the structural mounts — call it out per part. ]]
- [[ Part list: filename | qty | material | structural or cosmetic | notes. ]]
- [[ Group by: DPV mounts, housing internals (sleds/trays/endcap carriers),
     flow meter potting mold/housing. ]]
- [[ Post-processing: reaming holes, heat-set inserts, test-fit before you
     commit. ]]

### 7.2 Milled housings — 🔁 milled variant only
[[ Delrin stock spec, workholding, toolpath notes, o-ring groove dimensions
   (reference an actual standard — don't eyeball it), bore finish, threading
   the endcaps, tolerances that actually matter vs. ones that don't.
   Note your own experience honestly: milling Delrin has been a real challenge
   on a belt-drive machine. That warning is worth more than the toolpaths. ]]

### 7.3 Potting the flow meter
[[ Its own procedure, not a footnote. Mold prep and release agent, resin
   choice and mix ratio, degassing, pour technique to avoid voids, cure time
   and temperature, what a void does to you at 150 ft, how to inspect,
   what a failed pot looks like and when to scrap it. ]]

> **✅ Checkpoint** — all parts printed/milled, test-fit dry, potting cured and
> inspected. Nothing electronic has been touched yet.

---

## 8. Electronics

### 8.1 Boards
[[ Which boards, how to order them (gerbers/fab files in repo, house rules
   for the fab), and incoming inspection. ]]

[[ Pictures from the kicad layout]]

### 8.2 Soldering
[[ Per-board build order. General rules: lowest-profile parts first, SMD
   before through-hole, connectors last. Call out anything orientation- or
   polarity-critical, anything heat-sensitive, anything you should socket
   rather than solder down (the ESP32 module, sensor breakouts) so it can be
   swapped without a rework station. ]]
[[ Photos per board at the "done" state. ]]
[[ Special case: soldering wire junctions. ]]

### 8.3 Bench smoke test — before anything goes near a housing
[[ Power up on the bench with a current-limited supply. Expected rail voltages
   and expected idle current draw (give numbers). Check before/after each
   board is added. Confirm the device enumerates and boots. ]]

> **⛔ Stop** — do not install a board into a housing that hasn't passed §8.3.
> Getting it back out is the expensive part.

---

## 9. Wiring

[[ Likely the highest-value section in the document. It's also where the
   guide should be most prescriptive — colors and junction technique are the
   things people get wrong and then can't debug. ]]

### 9.1 Wire color and signal map
[[ THE master reference table. Signal | color | gauge | from | to | notes.
   One row per conductor. Group by harness. Make this linkable/anchored —
   people will bookmark it and come back mid-build.
   Also state the convention itself (e.g. red = switched V+, black = ground,
   and what each signal color means) so a builder extending the system stays
   consistent. ]]

### 9.2 Harness overview
[[ Which harnesses exist, what crosses a pressure boundary, cut lengths with
   slack allowances. A diagram here saves a thousand words. ]]

### 9.3 Junctions outside pressure vessels
[[ The dedicated how-to you called out. Why junctions live outside: you want
   the pressure boundary to be a single continuous conductor with no splice
   inside a place you can't inspect. Cover: strip lengths, mechanical joint
   (lineman's/Western Union or crimp barrel — pick one and standardize),
   solder technique, wicking control, strain relief, and the fact that the
   joint carries load, not the solder. ]]

### 9.4 Heat shrink
[[ Step-by-step with photos. Adhesive-lined vs plain and when each. Sizing
   (shrink ratio math), pre-loading shrink onto the wire BEFORE you solder
   (the classic mistake — give it its own callout), staggering joints in a
   bundle so the diameter doesn't stack, double-shrink over the whole joint,
   heat gun technique and temperature, how to tell when it's actually sealed
   vs. just shrunk. ]]

### 9.5 Glands and penetrators — 🔁
[[ Sealing procedure for both paths. For the cheap gland path this is the
   critical technique section: prep, potting/epoxy inside the gland if
   required, torque, and the fact that §10's test is what qualifies it. ]]

### 9.6 Power wiring notes
[[ The most important power note: The referenced battery pack from Amazon has the JST plug polarity reversed from what the ESP32 expects.  Historically, when I've made this mistake, it hasn't fried the board, but it seems like it ought to.  You will probably need to swap the polarity on the JST connector (switch black for red wires) before plugging it in.  Double check with the enclosed diagram and a voltmeter to make sure you've gotten it right. ]]

> **✅ Checkpoint** — full harness built, continuity-checked end to end, no
> shorts to ground or between rails. Power-on test with the harness attached,
> outside the housings.

---

## 10. Mechanical assembly

### 10.1 Internals into housings
[[ Order of operations per module. Which connectors mate before the sled goes
   in. Cable routing and service loops — leave enough slack to pull the guts
   out without desoldering. Photos at each stage. ]]

### 10.2 O-rings and closing up
[[ O-ring sizes, inspection, correct grease and how much (thin film, not
   packed), seating, endcap alignment, fastener sequence and torque, desiccant
   pack and moisture indicator card. ]]

### 10.3 Mounting to the DPV
[[ Mount geometry, fastener spec, orientation constraints — call out the
   magnetic-separation minimum distance from motor and battery cables as a
   hard requirement, not a suggestion, and tie it back to §6.7.
   Alignment of the IMU/compass axes relative to the vehicle centerline: this
   is a calibration input, so it must be repeatable. ]]

---

## 11. Sealing and leak testing

[[ This is the gate between "assembled" and "wet," and it belongs before
   firmware. Do not let anyone skip it. ]]

- [[ Vacuum test procedure: pull to spec, hold time, acceptable leak-down.
     Give actual numbers. ]]
- [[ What to do if it doesn't hold — bisecting which seal is leaking. ]]
- [[ Hydrostatic / pressure test if you have the capability: test to depth ×
     safety factor, hydraulic not pneumatic. 🔁 Mandatory for the milled
     variant. ]]
- [[ Shallow wet test with the housing sealed and empty (or with a dummy
     mass) before it ever holds electronics in water. ]]
- [[ Post-test inspection: moisture card, o-ring witness marks. ]]

> **⛔ Stop** — the system does not go in water with electronics inside until
> every housing has passed §11.

---

## 12. Firmware

### 12.1 Toolchain
[[ What to install, repo clone, board definitions, which serial driver. ]]

### 12.2 Flashing
[[ Exact commands. Per-module (nav, display, GPS if separate). What a
   successful flash looks like. Bootloader/recovery if it bricks. ]]

### 12.3 Configuration
[[ Config file / on-device settings: units, WiFi credentials for calibration
   upload, declination for the dive site, device token if pairing with
   Divemap. ]]

### 12.4 First boot
[[ Expected serial output, annotated line by line. Sensor discovery — what it
   looks like when the IMU/depth/flow meter are found, and what a missing one
   looks like. Link straight to §13 for each failure signature. ]]

> **✅ Checkpoint** — all sensors enumerate, all rails stable, display lights
> up and shows live data on the bench.

---

## 13. Calibration and first light

[[ Ordered, because later steps depend on earlier ones. Each gets: procedure,
   expected duration, pass/fail criteria with actual numbers, and what to do
   on a fail. Assume the reader is a competent diver but not a sensor-fusion
   expert. ]]

### 13.1 Depth zero
[[ Surface zero procedure, atmospheric compensation. ]]

### 13.2 Magnetometer calibration
- [[ Physical setup: away from steel, rebar, benches, laptops. Motor connected
     and in its final mounted position — you're calibrating the installed
     system, not the sensor. ]]
- [[ The motion procedure. If you've standardized on the 36-point dwell sweep
     (CW and CCW), document it as the procedure, with dwell time per point. ]]
- [[ Running the ellipsoid fit — on-device, script, or WiFi upload path. ]]
- [[ Pass/fail criteria, stated properly: per-sector residuals and worst-case
     heading error, not just RMS. Explain in one sentence why RMS alone isn't
     a gate — a systematic sector error averages out. Include the field
     magnitude dispersion pre-check as a "your data is bad, re-run it" gate. ]]
- [[ Declination: set it once, in the right sign, for the dive site. Note the
     failure mode where a doubled declination shows up as a heading error
     roughly equal to the local declination value. ]]

### 13.3 Flow meter calibration
[[ k-factor derivation from known-distance runs. Where to do it, how many
   runs, how to average, how to enter it. ]]

### 13.4 IMU alignment / static bias
[[ Mounting-angle offsets, level reference, static bias capture. ]]

### 13.5 In-water shakedown
[[ First dive protocol. Shallow, short, buddy, redundant nav. What to watch.
   Where the data lands. ]]

### 13.6 Field validation
[[ The accuracy validation run: long straight out-and-back at shallow depth,
   SMB drops at each end for GPS back-calculation. The point is to separate
   static mounting bias from speed-dependent motor-induced heading error —
   say so, and say what each looks like in the data. Give a "this is what
   good looks like" target. ]]

---

## 14. Troubleshooting

[[ Symptom → likely cause → check → fix. Table or definition list.
   Seed it with what you've already burned time on:
   - 3.3V rail sags on WiFi connect / sawtooth on the scope → see §9.6
   - Heading error concentrated in one sector → declination or axis convention
   - Clean RMS but bad real-world heading → §13.2 pass criteria
   - Sensor not found on boot → I²C address / wiring / cold joint
   - Moisture card triggered after a dive → §10.2, §11
   - Flow meter reads zero / erratic → potting void, mounting orientation
   This section should grow every time you or a builder hits something. Keep
   it in the repo so it's PR-able. ]]

---

## 15. Maintenance and servicing

[[ O-ring inspection and replacement cadence. Post-dive rinse. Desiccant
   replacement. Buffer LiPo replacement interval and disposal. Recalibration
   triggers — new dive site (declination), anything remounted, motor or
   battery cable changes. Storage. Firmware update procedure. ]]

---

## 99. Appendices

- **A. Pinout tables** — [[ per board, per connector ]]
- **B. Wire color master table** — [[ or keep it in §9.1 and link here ]]
- **C. O-ring and fastener spec** — [[ sizes, torque, thread locker ]]
- **D. Print settings summary** — [[ one-page reference ]]
- **E. Serial command reference** — [[ every console command ]]
- **F. Hardware revision applicability** — [[ what this doc covers, what
  changed between revs ]]
- **G. Changelog** — [[ doc changelog, separate from firmware ]]

---

## Contributing and feedback

[[ Link CONTRIBUTING.md. Invite build reports and troubleshooting PRs
   explicitly — the troubleshooting section is the one that benefits most from
   other people's failures. ]]

---

<!--
=============================================================
DUAL-PUBLISHING NOTE (delete before publishing)
=============================================================

Recommendation: single canonical source in dpv-nav/docs/build/, blog renders
it rather than duplicating it.

Why: this document will change every time the hardware does, and a manual copy
will silently drift. A drifted assembly guide for dive equipment is a real
problem, not a cosmetic one.

Two workable mechanisms, both fitting the markdown-commit-push workflow:

  1. Astro content collection sourced from the repo via git submodule.
     Zero sync step, works offline, `git submodule update --remote` to bump.
     Downside: submodules are mildly annoying forever.

  2. GitHub Action in dpv-nav that copies docs/build/ + assets/ into the site
     repo's content dir on push to main (or on release tag), opening a PR.
     Downside: one more moving part; upside: the site pins to a release tag,
     so the published guide always matches a tagged firmware/hardware rev.

Option 2 is probably the better fit given the hardware-revision problem —
you want the blog showing the last known-good build, not main.

Split the audiences rather than the content:
  - Blog gets a short narrative post ("I built a dead-reckoning nav system for
    a DPV, here's what it took") that links into the guide. Different voice,
    different job, written once, doesn't need to stay in sync.
  - The guide itself is reference material and reads the same in both places.

Image paths are the sharp edge. Use relative paths from the markdown file and
copy assets/ alongside the markdown in whichever mechanism you pick. Don't use
absolute site paths in the repo copy.

=============================================================
PHOTO SHOT LIST — do this before you start the next build
=============================================================

The photos are the long pole and most of them can only be taken once. Walk the
outline, list every image you need, and shoot them during the build rather
than trying to reconstruct them afterward. The ones you cannot retake:

  - every board mid-solder and at "done"
  - each junction at each stage: stripped, mechanically joined, soldered,
    shrunk (this sequence is the whole point of §9.3–9.4)
  - internals on the sled before it goes into the tube
  - o-ring seated, endcap open, immediately before closing
  - the harness laid out flat with color coding visible — one good overhead
    shot here replaces a lot of §9.1
  - potting: mold, pour, cured, sectioned scrap piece showing no voids

Shoot more than you need, on a plain background, with a scale reference.
-->
