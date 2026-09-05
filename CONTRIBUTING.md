# Contributing to Tern DPV-Nav

Thanks for your interest in contributing. This project is source-available under the [Tern DPV-Nav Source-Available License, Version 1.0](LICENSE.md). Before contributing, please read this document — it explains what you're agreeing to when you submit work.

---

## What kinds of contributions are welcome

- Bug reports and field observations (including dive test data, calibration results, error behavior)
- Corrections to documentation or BOM
- Firmware improvements, fixes, and new features
- Hardware design improvements — schematic corrections, layout refinements, mechanical improvements
- Build notes, assembly tips, and lessons learned from your own build

If you're planning a significant change — a new feature, a major refactor, or a hardware revision — please open an issue to discuss it first. That way you're not investing time in work that may not fit the project's direction.

---

## Licensing of your contributions

**By submitting a contribution to this project — whether as a pull request, issue attachment, email, forum post, or any other means — you agree to the following:**

You grant Daniel McMath / Tern Diving a perpetual, irrevocable, worldwide, royalty-free, non-exclusive license to use, reproduce, modify, distribute, sublicense, and relicense your contribution under any license terms, including commercial or proprietary terms.

This grant is stated in full in the [Contributions section of the license](LICENSE.md#contributions). Please read it before submitting.

**Why this matters:** This project uses a source-available license that reserves commercial rights. For that model to work — including any future commercial licensing, acquisition, or sale of the project — the Licensor needs to be able to relicense the full codebase and design files, including contributions from others. If contributors retained veto rights over that, the project could become unlicensable and unsellable, which would harm its long-term sustainability.

You are not giving up your own right to use your contribution. You are not assigning copyright. You are granting a license that allows the project to continue operating under its current model.

**You represent that:**
- You wrote the contribution yourself, or have the legal right to submit it under these terms
- Your contribution does not include material subject to third-party rights (patents, copyrights, trade secrets) that would limit the rights granted above
- If you are contributing on behalf of an employer, you have your employer's permission to do so

---

## What you get in return

Your contributions remain subject to the same source-available license as the rest of the project. Anyone building on this project — including you — has the same rights as any other community member under the [license](LICENSE.md).

Attribution for significant contributions will be maintained in the repository's commit history and, where appropriate, in project documentation.

---

## Safety

This project describes equipment for use in underwater environments. If you identify a safety-relevant issue — a design flaw, a build risk, incorrect depth ratings, or anything that could lead to equipment failure underwater — please flag it clearly as safety-related when you open the issue. Safety issues will be prioritized.

Do not assume that designs in this repository have been tested or validated for your specific conditions, depth, water temperature, or build. You are responsible for the safety of equipment you build and operate. See the Safety Disclaimer in the [license](LICENSE.md#safety-disclaimer).

---

## Code style and documentation

There's no formal style guide yet. Match the conventions of the surrounding code. Comment your reasoning, not just what the code does — especially in filtering logic, calibration math, and sensor fusion, where the *why* tends to get lost quickly.

For hardware changes, include the rationale in your PR description. A schematic diff without context is hard to review safely.

---

*Questions about contributing or about the license? Open an issue or reach out via [TernDiving.com](https://terndiving.com).*
