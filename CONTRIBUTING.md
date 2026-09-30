# Contributing to Tern DPV-Nav

Thanks for your interest in contributing. This project is source-available under noncommercial licenses (see [LICENSE.md](LICENSE.md)). Before contributing, please read this document. It explains what you're agreeing to when you submit work.

---

## What kinds of contributions are welcome

- Bug reports and field observations (including dive test data, calibration results, error behavior)
- Corrections to documentation or BOM
- Firmware improvements, fixes, and new features
- Hardware design improvements: schematic corrections, layout refinements, mechanical improvements
- Build notes, assembly tips, and lessons learned from your own build

If you're planning a significant change (a new feature, a major refactor, or a hardware revision), please open an issue to discuss it first. That way you're not investing time in work that may not fit the project's direction.

---

## Contributor License Agreement

Before your first pull request can be merged, you'll be asked to accept the project's [Contributor License Agreement](CLA.md). This is a one-time step: a bot will comment on your pull request with a link, and you accept by clicking through. The CLA is based on the standard Harmony contributor agreement template.

**What it does:** You keep the copyright to your contribution. You grant Daniel McMath / Tern Diving a license to use your contribution, including the right to offer it under other license terms, commercial ones included.

**Why this matters:** This project reserves commercial rights. For that model to work, including any future commercial licensing, acquisition, or sale of the project, the project needs to be able to license the full codebase and design files, including contributions from others. If contributors retained veto rights over that, the project could become unlicensable and unsellable, which would harm its long-term sustainability.

You are not giving up your own right to use your contribution, and you are not assigning copyright.

---

## What you get in return

Your contributions are released under the same licenses as the rest of the project. Anyone building on this project, including you, has the same rights as any other community member.

Attribution for significant contributions will be maintained in the repository's commit history and, where appropriate, in project documentation.

---

## Safety

This project describes equipment for use in underwater environments. If you identify a safety-relevant issue (a design flaw, a build risk, incorrect depth ratings, or anything that could lead to equipment failure underwater), please flag it clearly as safety-related when you open the issue. Safety issues will be prioritized.

See [SAFETY.md](SAFETY.md).

---

## Code style and documentation

There's no formal style guide yet. Match the conventions of the surrounding code. Comment your reasoning, not just what the code does, especially in filtering logic, calibration math, and sensor fusion, where the *why* tends to get lost quickly.

For hardware changes, include the rationale in your PR description. A schematic diff without context is hard to review safely.

`hardware/digikey-kicad-library` is a git submodule pulling in Digi-Key's own KiCad library, which is under its own license (see [LICENSE.md](LICENSE.md)), not the project's. After cloning, run `git submodule update --init` or those footprints/symbols will be missing.

---

*Questions about contributing or about licensing? Open an issue or reach out via [TernDiving.com](https://terndiving.com).*
