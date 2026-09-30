# Licensing

Copyright (c) 2026 Daniel McMath / Tern Diving (TernDiving.com)

Tern DPV-Nav is **source-available**, not open source. Different parts of this repository are covered by different standard licenses. Both are noncommercial: you may use, copy, modify, and share this work for noncommercial purposes. Commercial use requires a separate license from Tern Diving (see below).

| What | License | Full text |
|---|---|---|
| Firmware, scripts, and all other software (including code samples in documentation) | PolyForm Noncommercial License 1.0.0 | [LICENSES/PolyForm-Noncommercial-1.0.0.txt](LICENSES/PolyForm-Noncommercial-1.0.0.txt) |
| Hardware design files (schematics, PCB layouts, CAD/STL/DXF, drawings, BOM) and documentation | Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International | [LICENSES/CC-BY-NC-SA-4.0.txt](LICENSES/CC-BY-NC-SA-4.0.txt) |

Where a file carries its own license header or SPDX identifier, that governs. Otherwise:

- **PolyForm Noncommercial 1.0.0:** source code and build configuration: `src/`, `lib/`, `firmware/`, `build/`, the scripts and test harnesses in `tools/`, `platformio.ini`, `partitions_nav.csv`, and code samples in `docs/`.
- **CC BY-NC-SA 4.0:** `hardware/`, `docs/`, sample data and calibration files (the CSV and JSON files in `tools/` and `baseline cal jsons/`), and everything else not listed here.

**Not covered by either license:**

- `images/`: the Tern logos are trademarks, all rights reserved. See [TRADEMARKS.md](TRADEMARKS.md).
- `hardware/digikey-kicad-library/`: third-party library, distributed under its own license.

## Required notices (software)

Under the Notices section of the PolyForm Noncommercial License, anyone who distributes the software must pass along the following lines unchanged:

```
Required Notice: Copyright (c) 2026 Daniel McMath / Tern Diving (https://terndiving.com)
Required Notice: This software is part of equipment used in underwater environments. Improper construction, calibration, or use may result in serious injury or death. See SAFETY.md.
```

## Attribution (hardware and documentation)

When sharing hardware files or documentation under CC BY-NC-SA 4.0, please attribute as:

> Tern DPV-Nav by Daniel McMath / Tern Diving (https://terndiving.com), licensed under CC BY-NC-SA 4.0. Original: https://github.com/djmcmath/dpv-nav

## Commercial licensing

Tern Diving reserves the right to offer this work under commercial terms. If you want to sell units or kits, offer build or manufacturing services, or incorporate any part of this project into a commercial product or service, contact contact@terndiving.com or see TernDiving.com.

## See also

- [SAFETY.md](SAFETY.md): dive-safety notice. Read it before you build or dive with this equipment.
- [TRADEMARKS.md](TRADEMARKS.md): the licenses above do not grant rights to the Tern Diving or DPV-Nav names.
- [CONTRIBUTING.md](CONTRIBUTING.md) and [CLA.md](CLA.md): how to contribute, and the terms for contributions.
