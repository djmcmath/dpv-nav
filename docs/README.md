# DPV-Nav Documentation

Tern DPV-Nav is a dead-reckoning underwater navigation system for DPVs. If you've
just landed here, read the [System Overview](overview.md) first — it's short, and it
explains what this is and what it deliberately isn't.

## The guides

Three volumes, in the order you'd actually use them.

| | Volume | Read it when |
|---|---|---|
| **0** | **[System Overview](overview.md)** | Always first. What it is, how dead reckoning works, how accurate it honestly is, the four modules, the design goals. |
| **1** | **[Manufacturing Guide](manufacturing-guide.md)** | You've decided to build one. Variant choices, BOM, PCBs, 3D printing, milling, potting the flow meter. |
| **2** | **[Assembly Guide](assembly-guide.md)** | You have a box of parts. Soldering, wiring, mechanical assembly, leak testing, firmware, first boot. |
| **3** | **[User Guide](user-guide.md)** | You have a working device. First-time setup, calibration, the dive routine, menus, screens, troubleshooting. |

> **💡 Note** — volumes 1 and 2 are **drafts**. They carry real, hard-won content in
> some sections and a **📝 TBD** block in others, which marks a section that is
> outlined but not yet written. The gaps are deliberately visible rather than hidden:
> if you're building from these, you'll need to fill in around them, and a build
> report or a PR closing one is the single most useful contribution to this project.
> See [CONTRIBUTING.md](../CONTRIBUTING.md).

## Engineering reference

Deeper than the guides, aimed at someone changing the firmware or the algorithms
rather than building or diving a unit.

| Document | Covers |
|---|---|
| [calibration-guide.md](calibration-guide.md) | The full calibration system: file formats, the magnetometer two-stage fit, coverage gap-filling, gyro/accel, the Fourier heading fit, speed cal, serial diagnostics, the storage API. |
| [ahrs-orientation.md](ahrs-orientation.md) | The Mahony filter, Euler extraction, update rate, quaternion access. **Read before touching an axis map.** |
| [mag-temperature-compensation.md](mag-temperature-compensation.md) | Magnetometer thermal drift: what ST publishes, what was measured, why readings are corrected rather than headings, and how to refit the coefficients. |
| [data-logging-guide.md](data-logging-guide.md) | Log levels, the CSV and binary record formats, `pos_src`, file storage, upload, and how to analyze a dive. |
| [logging-integration-example.cpp](logging-integration-example.cpp) | Worked example of wiring logging into new code. |

## State of the work

| Document | Covers |
|---|---|
| [backlog.md](backlog.md) | **Everything outstanding**, ordered by what bites a diver rather than by what's easiest. Tier 1 is silent data loss. Also carries a "Settled — do not re-open" section recording decisions already made. |

Read the backlog before proposing work or re-raising something that looks deferred.
Dive Map (the companion web service) keeps its own list at
`dive-map/docs/architecture/backlog.md`.

## Design plans

Working documents written while a feature was being designed. They are **historical
records of a decision**, not current documentation — where a plan and a guide
disagree, the guide is right. They're kept because source comments cite them by path,
and because they record *why* something is shaped the way it is.

| Document | Feature |
|---|---|
| [cloud-calibration-plan.md](cloud-calibration-plan.md) | Moving calibration fitting from a laptop script to the Dive Map server. |
| [baseline-cal-two-pass.md](baseline-cal-two-pass.md) | The baseline + gap-fill two-pass design, including six failed attempts at live orientation feedback. |
| [baseline-live-grid-plan.md](baseline-live-grid-plan.md) | Bringing the live coverage grid back to baseline collection (0.7.25). |
| [cal-screens-unified-plan.md](cal-screens-unified-plan.md) | The single shared frame and softkey model used by every calibration screen (0.7.26). |

---

## Callout conventions

Used throughout the guides. Defined once here.

> **⚠ Safety** — can hurt you or drown you. Never optional.

> **⛔ Stop** — do not proceed past this point until the check passes.

> **✅ Checkpoint** — a verification gate at the end of a stage.

> **💡 Note** — context, rationale, "why it's like this."

> **🔁 Variant** — this step differs depending on your build variant.

> **📝 TBD** — outlined but not yet written. What's missing is described inside.

<!--
=============================================================
AUTHOR NOTES — not for readers. Carried over from the original
assembly-guide-outline.md when it was split into volumes 1 and 2
(2026-11-09).
=============================================================

DUAL-PUBLISHING
---------------
Recommendation: single canonical source here in dpv-nav/docs/; the blog renders
it rather than duplicating it.

Why: these documents change every time the hardware does, and a manual copy will
silently drift. A drifted assembly guide for dive equipment is a real problem,
not a cosmetic one.

Two workable mechanisms, both fitting the markdown-commit-push workflow:

  1. Astro content collection sourced from the repo via git submodule.
     Zero sync step, works offline, `git submodule update --remote` to bump.
     Downside: submodules are mildly annoying forever.

  2. GitHub Action in dpv-nav that copies docs/ + assets/ into the site repo's
     content dir on push to main (or on release tag), opening a PR.
     Downside: one more moving part; upside: the site pins to a release tag, so
     the published guide always matches a tagged firmware/hardware rev.

Option 2 is probably the better fit given the hardware-revision problem — you
want the blog showing the last known-good build, not main.

Split the audiences rather than the content:
  - Blog gets a short narrative post ("I built a dead-reckoning nav system for a
    DPV, here's what it took") that links into the guides. Different voice,
    different job, written once, doesn't need to stay in sync.
  - The guides themselves are reference material and read the same in both
    places.

Image paths are the sharp edge. Use relative paths from the markdown file and
copy assets/ alongside the markdown in whichever mechanism you pick. Don't use
absolute site paths in the repo copy.

PHOTO SHOT LIST — do this before the next build
-----------------------------------------------
The photos are the long pole and most of them can only be taken once. Walk the
guides, list every image needed, and shoot them during the build rather than
trying to reconstruct them afterward. The ones you cannot retake:

  - every board mid-solder and at "done"                    -> assembly §8.1
  - each junction at each stage: stripped, mechanically
    joined, soldered, shrunk (this sequence IS §9.3-9.4)    -> assembly §9.3, §9.4
  - internals on the sled before it goes into the tube      -> assembly §10.1
  - o-ring seated, endcap open, immediately before closing  -> assembly §10.2
  - the harness laid out flat with color coding visible —
    one good overhead shot replaces a lot of §9.1           -> assembly §9.1
  - potting: mold, pour, cured, sectioned scrap piece
    showing no voids                                        -> manufacturing §9
  - button stack aligned vs. misaligned                     -> manufacturing §7.4
  - completed system: bench, mounted on the DPV, display
    in-hand, in-water                                       -> manufacturing §2
  - KiCad layout renders (no build needed)                  -> manufacturing §6.1

Shoot more than you need, on a plain background, with a scale reference.

ALSO STILL OWED
---------------
  - docs/bom/bom.csv — the canonical variant-tagged BOM (manufacturing §5)
  - Screen-by-screen calibration walkthrough in the user guide, using the
    renders from tools/cal_screen_mock/ (see backlog.md Tier 3). Unblocked once
    0.7.26 ships.
-->
