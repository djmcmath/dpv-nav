# Calibration screen mockups

Pixel mockups of the unified calibration screens: one frame (header, work area,
status line, softkeys) shared by Baseline (1), Fill gaps (1a), Mounted (2) and
Heading (3), plus the shared upload and result screens. Designed 2026-11-09;
the review page is published at https://claude.ai/artifact/UNkMxYdHoWi5kpYHdDxcpX.

This is a JavaScript re-drawing of the screens, not the firmware. It uses the
same 320x240 panel, RGB565 colours and Adafruit GFX 5x7 font, so text sizes and
layout match what `display.cpp` will draw, but nothing here checks that the
firmware actually does. When the firmware lands, update `screens.js` to match it
before using the images anywhere.

```bash
node tools/cal_screen_mock/render.js
```

writes one PNG per screen (2x, 640x480) to `png/` and the review page to
`cal-screens.html`. The font comes from the GFX library under `.pio/`, so run
`pio run -e display` once on a fresh checkout.

- `screens.js`: the drawing primitives, the frame, and every screen.
- `page.template.html`: the review page; `render.js` inlines `screens.js` into it.

The user guide's calibration walkthrough is meant to use these images; see
`docs/backlog.md`.
