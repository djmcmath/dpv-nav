![Tern](images/logo-image-w-text.png)

# Tern DPV-Nav

**Built for Exploration**

Underwater navigation for technical divers who are tired of getting lost and
wondering where they went.

---

## What it is

A dead-reckoning navigation system that mounts on a DPV ("scooter"). It knows which
way you're pointed, how fast you're moving through the water, and where you started,
and from those three things it continuously estimates where you are — showing bearing
and range to a waypoint you chose.

The design goal is to make something that any technical diver can build, not to create a project that requires a bunch of expensive proprietary gear.  Therefore: The scope is deliberately small. The parts are deliberately ordinary. Some of the
build is genuinely technical: you'll manufacture and solder custom PCBs, print parts,
and flash firmware to a pair of ESP32s. But even if you can't solder or print, if
you're in the technical diving community the odds are excellent that you know
someone who can.

**This is not "underwater GPS."** GPS doesn't work underwater and no firmware
violates that. Precise underwater positioning is a delightful engineering problem
with several excellent solutions, and precision scales almost perfectly with money
spent. What this *is*: a tool that will tell you where you are far more accurately
than anything else readily available at this price point. If your current process is
a wrist compass plus elapsed time plus mental arithmetic, this is a large step up.

**For planning, assume error is about 10% of the distance since your last fix.** Run
500 m from your last known position and expect to be within 50 m of the target —
close enough to drop a marker and run a reel, not close enough to skip the search.
Careful calibration and straight runs have produced 2–3% in practice, but 10% is the
number to brief your buddy on.

> **⚠ Safety** — this is an *aid*, not an authority. It doesn't know your
> certification limits, your gas, your deco obligation, or your dive plan. It doesn't
> actually know where you are; it's estimating, and the estimate's errors compound.
> It also adds a cabled device to your DPV, which brings entanglement risk and task
> loading to challenges you already have. Dive within your limits and your training.
> **Read [SAFETY.md](SAFETY.md) before you build or dive.**

## Design goals

1. **Design for Manufacturability.** This isn't a $10,000 Italian-engineered underwater-equivalent-of-a-Ferrari.  It's a $500 DIY project.  It needs to balance effective functionality against cost and ability of a typical DIY-er to build.
2. **Design for Simple Usability.** Minimize complexity wherever possible.  Tech divers do mental math all the time, approximations are typically good enough, and getting lost in menus and button pushing at depth is NOT what anyone wants to do.  The operation should remind people more of an iPad Touch than a Sony Alpha.
3. **Fail loudly, never silently.** The failure mode that matters is bad data that
   looks fine and gets discovered a week later. The backlog is ordered by this, not
   by what's easiest.
4. **Readily available parts**, and a build that stays within reach of a determined
   hobbyist.
5. **Source-available and DIY-first.** Build one, modify it, share what you learn.

## Where this stands

Diveable and in regular use. Current release **0.7.25**; nav board **Rev D**, display
board **Rev B**.

**Working and proven in the water:** tilt-compensated heading (Mahony AHRS),
dead-reckoning position, flow-sensor speed with GPS override on the surface, up to
200 named waypoints with arrive-to-reset, the full calibration chain (baseline →
gap-fill → mounted → 12-point heading → speed) with cloud fitting and on-device
accept/reject, dive logging with automatic upload and post-dive track correction,
current measurement by holding station, depth-triggered dive/surface mode,
magnetometer thermal compensation, and OTA updates for both boards together.

**In flight:** the unified calibration screens (**0.7.26**) — one shared frame and
button model across all five cal steps — are built and in test. The menu labels and
screens described in the User Guide are the 0.7.25 ones until that ships.

**Known gaps worth knowing about before you build:**

- A new baseline or mounted mag cal **silently invalidates** the 12-point heading
  cal. The rule (re-collect from scratch, never gap-fill) currently lives only in the
  docs and in the builder's head. Nothing enforces it.
- Forgetting to stop logging silently defers the whole dive's upload.
- Magnetic declination is a hard-coded constant (currently set for the Seattle area), so a new dive site means editing that by hand in config.h.
- Thermal compensation has never been tested on the **cold** side; every measurement
  warmed from room temperature.
- Polar cells in the gap-fill grid can't realistically be aimed at, because heading
  gain goes as `tan(pitch)`.
- Volumes 1 and 2 of the documentation are **drafts** with visible gaps.  To be frank, until I can retire and do this full time, everything will be in some state of draft.

**→ [docs/backlog.md](docs/backlog.md)** is the full list, ordered by what bites a
diver first rather than by what's easiest to fix. It also records decisions already
settled, so they stop costing time. Read it before proposing work.

## Documentation

Everything lives in **[docs/](docs/)** — start at **[docs/README.md](docs/README.md)**
for the index. Four volumes, in the order you'd use them:

| | | |
|---|---|---|
| **0** | [System Overview](docs/overview.md) | What it is, how dead reckoning works, the four modules, why it's split that way. **Read this first.** |
| **1** | [Manufacturing Guide](docs/manufacturing-guide.md) | Variant choices, BOM, PCBs, 3D printing, milling, potting. *Draft.* |
| **2** | [Assembly Guide](docs/assembly-guide.md) | Soldering, wiring, sealing, leak testing, firmware, first boot. *Draft.* |
| **3** | [User Guide](docs/user-guide.md) | First-time setup, calibration, the dive routine, menus, troubleshooting. |

Deeper engineering reference — the calibration system, the AHRS, thermal
compensation, the logging formats — is indexed in
[docs/README.md](docs/README.md#engineering-reference).

Hardware sources (KiCad schematics, PCB layouts, 3D models) are under
[hardware/](hardware/).

## Building the firmware

```bash
pio run -e nav        # nav board
pio run -e display    # display board
```

Full toolchain and flashing instructions:
[Assembly Guide §12](docs/assembly-guide.md#12-firmware).

## About the name

Named after the Arctic Tern, which is a fantastic bird. They navigate from the north
pole to the south every year — mating in the Antarctic, raising their young in
transit, growing to maturity in the Arctic — and then migrate back to do it all
again. Insane.

If a tiny bird can navigate thousands of kilometres a year, perhaps we can build a
DIY-capable device that lets divers cover much smaller distances with confidence.

## On how it was built

Yes, it's vibe coded. I used Claude Code to great effect, covering a ton of ground in
a matter of months that I hadn't been able to get through on my own. Say what you
will about AI-supported coding: this project exists, and it wouldn't without AI
support.

## License

Tern DPV-Nav is source-available and free for noncommercial use.

- **Firmware and software** — [PolyForm Noncommercial 1.0.0](LICENSES/PolyForm-Noncommercial-1.0.0.txt)
- **Hardware designs and documentation** — [CC BY-NC-SA 4.0](LICENSES/CC-BY-NC-SA-4.0.txt)

Build one, modify it, share your improvements. Commercial use — selling units or
kits, build services, or use in a commercial product — requires a separate license:
contact `contact@terndiving.com`.

See [LICENSE.md](LICENSE.md) for which license covers what, [SAFETY.md](SAFETY.md)
before you build or dive, and [CONTRIBUTING.md](CONTRIBUTING.md) if you'd like to
contribute. Build reports and troubleshooting entries are the most useful thing you
can send.
