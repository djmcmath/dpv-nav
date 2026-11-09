/* Pixel mock of the DPV-Nav display board: ST7789 320x240, RGB565 colours,
   Adafruit GFX classic 5x7 font (6x8 cell per text size). FONT_HEX is the
   library's glcdfont.c, inserted at build time. */

const FONT = (() => {
  const a = new Uint8Array(1280);
  for (let i = 0; i < 1280; i++) a[i] = parseInt(FONT_HEX.substr(i * 2, 2), 16);
  return a;
})();

const C = {
  BLACK: 0x0000, WHITE: 0xFFFF, RED: 0xF800, GREEN: 0x07E0, CYAN: 0x07FF,
  YELLOW: 0xFFE0, GRAY: 0x7BEF, DIM: 0x2104, OPEN: 0x1082, DIMTEXT: 0x4208,
  STEP: 0x52AA, SKY: 0x2AD4, GROUND: 0x7A83,
};

class TFT {
  constructor() { this.buf = new Uint16Array(76800); this.clip = null; }
  px(x, y, c) {
    if (x < 0 || y < 0 || x >= 320 || y >= 240) return;
    const k = this.clip;
    if (k && (x < k[0] || y < k[1] || x >= k[0] + k[2] || y >= k[1] + k[3])) return;
    this.buf[y * 320 + x] = c;
  }
  fillRect(x, y, w, h, c) { for (let j = y; j < y + h; j++) for (let i = x; i < x + w; i++) this.px(i, j, c); }
  hline(x, y, w, c) { this.fillRect(x, y, w, 1, c); }
  vline(x, y, h, c) { this.fillRect(x, y, 1, h, c); }
  rect(x, y, w, h, c) {
    this.hline(x, y, w, c); this.hline(x, y + h - 1, w, c);
    this.vline(x, y, h, c); this.vline(x + w - 1, y, h, c);
  }
  line(x0, y0, x1, y1, c) {
    const dx = Math.abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    const dy = -Math.abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    let err = dx + dy;
    for (;;) {
      this.px(x0, y0, c);
      if (x0 === x1 && y0 === y1) break;
      const e2 = 2 * err;
      if (e2 >= dy) { err += dy; x0 += sx; }
      if (e2 <= dx) { err += dx; y0 += sy; }
    }
  }
  fillCircle(cx, cy, r, c) {
    for (let dy = -r; dy <= r; dy++) {
      const dx = Math.round(Math.sqrt(r * r - dy * dy));
      this.hline(cx - dx, cy + dy, 2 * dx + 1, c);
    }
  }
  inRound(i, j, x, y, w, h, r) {
    if (i < x || j < y || i >= x + w || j >= y + h) return false;
    const ccx = i < x + r ? x + r : i > x + w - 1 - r ? x + w - 1 - r : i;
    const ccy = j < y + r ? y + r : j > y + h - 1 - r ? y + h - 1 - r : j;
    const dx = i - ccx, dy = j - ccy;
    return dx * dx + dy * dy <= r * r;
  }
  fillRound(x, y, w, h, r, c, clipW = w) {
    for (let j = y; j < y + h; j++)
      for (let i = x; i < x + clipW; i++)
        if (this.inRound(i, j, x, y, w, h, r)) this.px(i, j, c);
  }
  round(x, y, w, h, r, c) {
    for (let j = y; j < y + h; j++)
      for (let i = x; i < x + w; i++)
        if (this.inRound(i, j, x, y, w, h, r) && !this.inRound(i, j, x + 1, y + 1, w - 2, h - 2, r - 1))
          this.px(i, j, c);
  }
  fillTri(x0, y0, x1, y1, x2, y2, c) {
    const E = [[x0, y0, x1, y1], [x1, y1, x2, y2], [x2, y2, x0, y0]];
    const lo = Math.min(y0, y1, y2), hi = Math.max(y0, y1, y2);
    for (let y = lo; y <= hi; y++) {
      const xs = [];
      for (const [ax, ay, bx, by] of E) {
        if (ay === by) { if (y === ay) xs.push(ax, bx); }
        else if (y >= Math.min(ay, by) && y <= Math.max(ay, by)) xs.push(ax + (y - ay) * (bx - ax) / (by - ay));
      }
      if (xs.length) {
        const a = Math.round(Math.min(...xs)), b = Math.round(Math.max(...xs));
        this.hline(a, y, b - a + 1, c);
      }
    }
  }
  // Transparent background, as the firmware's labels on a black screen.
  text(x, y, str, s, fg) {
    for (const ch of str) {
      const code = ch === "°" ? 248 : ch === "±" ? 241 : ch.charCodeAt(0);
      for (let i = 0; i < 5; i++) {
        const bits = FONT[code * 5 + i];
        for (let j = 0; j < 8; j++) if ((bits >> j) & 1) this.fillRect(x + i * s, y + j * s, s, s, fg);
      }
      x += 6 * s;
    }
  }
}

const tw = (s, z) => [...s].length * 6 * z;

// ---------------------------------------------------------------------------
// The frame: header 0-23, work area 24-183, status 186-209, softkeys 212-239.
// ---------------------------------------------------------------------------
const STAGES = [["1", "BASELINE"], ["1a", "FILL GAPS"], ["2", "MOUNTED"], ["3", "HEADING"]];

function header(t, stage, count) {
  let x = 2;
  for (const [id] of STAGES) {
    const w = tw(id, 2) + 6;
    if (id === stage) { t.fillRound(x, 1, w, 20, 3, C.CYAN); t.text(x + 3, 3, id, 2, C.BLACK); }
    else t.text(x + 3, 3, id, 2, C.STEP);
    x += w + 5;
  }
  t.text(x + 1, 3, STAGES.find(s => s[0] === stage)[1], 2, C.CYAN);
  if (count) t.text(318 - tw(count, 2), 3, count, 2, C.WHITE);
  t.hline(0, 23, 320, C.CYAN);
}

function key(t, side, k, fill) {
  const x = side ? 162 : 2, y = 212, w = 156, h = 26, r = 5;
  let ink;
  switch (k.style) {
    case "blank": t.round(x, y, w, h, r, C.DIM); return;
    case "dim": t.round(x, y, w, h, r, C.DIM); ink = C.DIMTEXT; break;
    case "normal": t.round(x, y, w, h, r, C.GRAY); ink = C.WHITE; break;
    case "go": t.fillRound(x, y, w, h, r, C.GREEN); ink = C.BLACK; break;
    case "hold":
      t.round(x, y, w, h, r, C.GRAY); ink = C.GRAY;
      if (fill > 0) { t.fillRound(x, y, w, h, r, C.RED, Math.round(w * Math.min(1, fill))); ink = C.WHITE; }
      break;
  }
  t.text(x + ((w - tw(k.label, 2)) >> 1), y + 6, k.label, 2, ink);
}

function frameEnd(t, statusParts, left, right, fill = 0) {
  t.hline(0, 184, 320, C.DIM);
  let x = 4;
  for (const [s, c] of statusParts) { t.text(x, 190, s, 2, c); x += tw(s, 2); }
  key(t, 0, left, fill);
  key(t, 1, right, 0);
}

const QUIT = { label: "hold: QUIT", style: "hold" };
const REJECT = { label: "hold: REJECT", style: "hold" };
const FINISH = { label: "FINISH", style: "normal" };
const ACCEPT = { label: "ACCEPT", style: "normal" };
const OK = { label: "OK", style: "normal" };
const BLANK = { label: "", style: "blank" };

// ---------------------------------------------------------------------------
// Shared widgets
// ---------------------------------------------------------------------------
const CELL = { G: C.GREEN, Y: C.YELLOW, R: C.RED, ".": C.OPEN };
const GX = 26, GY = 36, CW = 24;

function ticks(t, rowLabels, rh) {
  ["N", "", "", "E", "", "", "S", "", "", "W", "", ""].forEach((l, c) => { if (l) t.text(GX + c * CW + 10, 27, l, 1, C.WHITE); });
  rowLabels.forEach((l, r) => t.text(2, GY + r * rh + (rh >> 1) - 4, l, 1, C.GRAY));
}

function cells(t, rows, rh, nums) {
  let n = 0;
  rows.forEach((row, r) => [...row].forEach((ch, c) => {
    const cx = GX + c * CW, cy = GY + r * rh;
    t.fillRect(cx + 1, cy + 1, CW - 2, rh - 2, CELL[ch]);
    if (ch === "Y" && nums) t.text(cx + 4, cy + (rh >> 1) - 4, String(nums[n++ % nums.length]), 1, C.BLACK);
  }));
}

function highlight(t, r, c, rh, col) {
  const cx = GX + c * CW, cy = GY + r * rh;
  t.rect(cx, cy, CW, rh, col); t.rect(cx + 1, cy + 1, CW - 2, rh - 2, col);
}

function dot(t, x, y) { t.fillCircle(x, y, 4, C.BLACK); t.fillCircle(x, y, 3, C.WHITE); }

const EDGES = [90, 60, 30, -30, -60, -90];   // elevation bands, as mag_orient::elevBand()
function grid5(t, rows, cur, hdg, pitch, nums) {
  const RH = 24;
  ticks(t, ["+60", "+30", "  0", "-30", "-60"], RH);
  cells(t, rows, RH, nums);
  highlight(t, cur[0], cur[1], RH, C.WHITE);
  let dr = 0;
  while (dr < 4 && pitch < EDGES[dr + 1]) dr++;
  const frac = (EDGES[dr] - pitch) / (EDGES[dr] - EDGES[dr + 1]);
  dot(t, Math.round(GX + hdg / 360 * 12 * CW), Math.round(GY + (dr + frac) * RH));
}

function grid3(t, rows, cur, inWin, hdg, pitch) {
  const RH = 20;
  ticks(t, ["+30", "  0", "-30"], RH);
  cells(t, rows, RH, null);
  highlight(t, cur[0], cur[1], RH, inWin ? C.WHITE : C.YELLOW);
  dot(t, Math.round(GX + hdg / 360 * 12 * CW), Math.round(GY + (45 - pitch) / 90 * 3 * RH));
}

function rollWidget(t, x, y, S, cols, cur) {
  const e = S - 1, cx = x + (e >> 1), cy = y + (e >> 1);
  const T = [[x, y, x + e, y], [x + e, y, x + e, y + e], [x + e, y + e, x, y + e], [x, y + e, x, y]];
  T.forEach(([ax, ay, bx, by], k) => t.fillTri(ax, ay, bx, by, cx, cy, cols[k]));
  if (cur >= 0) {
    const [ax, ay, bx, by] = T[cur];
    t.line(ax, ay, bx, by, C.WHITE); t.line(ax, ay, cx, cy, C.WHITE); t.line(bx, by, cx, cy, C.WHITE);
  }
}

// drawHorizon() from display.cpp, at a parameterised size: error from the
// target pitch, exaggerated by gain, brackets at +-tol.
function horizon(t, x0, y0, S, pErr, roll, tol, gain, inWin) {
  t.clip = [x0, y0, S, S];
  const mid = S >> 1, off = pErr * gain;
  const dr = Math.max(-60, Math.min(60, roll * gain));
  const slope = Math.tan(dr * Math.PI / 180);
  for (let x = 0; x < S; x++) {
    const yh = Math.max(0, Math.min(S, mid + off + (x - mid) * slope));
    const y = Math.floor(yh);
    if (y > 0) t.vline(x0 + x, y0, y, C.SKY);
    if (y < S) t.vline(x0 + x, y0 + y, S - y, C.GROUND);
  }
  const yl = Math.floor(mid + off - mid * slope), yr = Math.floor(mid + off + (S - 1 - mid) * slope);
  for (let k = -2; k <= 2; k++) t.line(x0, y0 + yl + k, x0 + S - 1, y0 + yr + k, C.WHITE);
  const mark = inWin ? C.GREEN : C.YELLOW, tp = Math.round(tol * gain);
  for (let side = 0; side < 2; side++) {
    const xo = side ? S - 4 : 3, xa = side ? S - 14 : 3;
    for (let k = 0; k < 3; k++) {
      t.vline(x0 + xo + (side ? -k : k), y0 + mid - tp, 2 * tp + 1, mark);
      t.hline(x0 + xa, y0 + mid - tp + k, 11, mark);
      t.hline(x0 + xa, y0 + mid + tp - k, 11, mark);
    }
  }
  t.hline(x0 + mid - 12, y0 + mid, 8, C.BLACK);
  t.hline(x0 + mid + 5, y0 + mid, 8, C.BLACK);
  t.fillCircle(x0 + mid, y0 + mid, 2, C.BLACK);
  t.clip = null;
  t.rect(x0, y0, S, S, C.GRAY);
}

function bar(t, x, y, w, h, frac, col, label, labelCol) {
  t.rect(x, y, w, h, C.GRAY);
  const fw = Math.round((w - 2) * frac);
  if (fw > 0) t.fillRect(x + 1, y + 1, fw, h - 2, col);
  if (label) t.text(x + 6, y + 2, label, 2, labelCol);
}

function hdgCal(t, f, ind, pitch, roll, level, hold, att, statusParts, markStyle) {
  header(t, "3", "4/12");
  t.text(4, 28, "Align to external compass", 2, C.GRAY);
  t.text(4, 55, "TARGET", 2, C.GRAY);
  t.text(220, 48, "120°", 4, C.WHITE);
  t.text(4, 86, "INDICATED HDG", 2, C.GRAY);
  t.text(244, 82, ind, 3, C.GRAY);
  horizon(t, 4, 104, 80, pitch, roll, 3, 6, level);
  t.text(92, 107, "HOLD", 2, C.GRAY);
  bar(t, 170, 106, 146, 18, hold, C.GREEN);
  t.text(92, 133, att, 2, C.GRAY);
  t.text(92, 159, "Avg err +2°", 2, C.YELLOW);
  frameEnd(t, statusParts, QUIT, { label: "MARK", style: markStyle }, f);
}

function result(t, f, stage, num, word, col, sub, extra, next) {
  header(t, stage, "RESULT");
  t.text(8, 36, num, 6, col);
  t.text(316 - tw(word, 4), 44, word, 4, col);
  t.text(8, 96, sub, 2, C.GRAY);
  if (extra) t.text(8, 120, extra, 2, C.GRAY);
  frameEnd(t, next, REJECT, ACCEPT, f);
}

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------
const SCREENS = {
  bars: { stage: "1", draw(t, f = 0) {
    header(t, "1", "212/2160");
    [["X", 92], ["Y", 64], ["Z", 31]].forEach(([a, v], i) => {
      const y = 34 + i * 28;
      t.text(4, y + 4, a, 2, C.GRAY);
      t.rect(24, y, 292, 22, C.GRAY);
      t.fillRect(25, y + 1, Math.round(290 * v / 100), 20, v >= 80 ? C.GREEN : v >= 40 ? C.YELLOW : C.RED);
    });
    t.text(4, 126, "Finding north...", 2, C.WHITE);
    t.text(4, 150, "The grid appears", 2, C.GRAY);
    t.text(4, 166, "once it locks.", 2, C.GRAY);
    frameEnd(t, [["Tumble the unit randomly", C.YELLOW]], QUIT, FINISH, f);
  } },

  baseGrid: { stage: "1", draw(t, f = 0) {
    header(t, "1", "1980/2160");
    grid5(t, ["RYRYGYRRYRYR", "GGYGGGGYGYGG", "GGGGGGGGGGGG", "GGGGYGGGGGGY", "YGYGGYRGGRGY"],
          [1, 9], 284, 41, [7, 12, 3, 9, 14, 5, 11, 8, 16, 2]);
    t.text(4, 162, "38/60 cells", 2, C.WHITE);
    t.text(150, 162, "284° +41°", 2, C.YELLOW);
    rollWidget(t, 287, 157, 26, [C.GREEN, C.YELLOW, C.RED, C.GREEN], 1);
    frameEnd(t, [["Auto-finish at 2160", C.YELLOW]], QUIT, FINISH, f);
  } },

  gapGrid: { stage: "1a", draw(t, f = 0) {
    header(t, "1a", "9/14");
    grid5(t, ["..G..Y...R..", ".G..G...G...", "....G...Y...", "..G...G..G..", ".R.Y..G....."],
          [2, 8], 255, -8, [11, 6, 4]);
    t.text(4, 162, "5 cells left", 2, C.WHITE);
    t.text(160, 162, "255° -8°", 2, C.YELLOW);
    rollWidget(t, 287, 157, 26, [C.GREEN, C.RED, C.RED, C.YELLOW], 0);
    frameEnd(t, [["Fill red & yellow cells", C.YELLOW]], QUIT, FINISH, f);
  } },

  mounted: { stage: "2", hold: "QUIT", draw(t, f = 0) {
    header(t, "2", "16/36");
    grid3(t, ["GGRGRRGRGRRR", "GGGGRGGRGGGR", "RRGRRRGRRRRR"], [1, 4], true, 130, 1);
    horizon(t, 4, 100, 80, 1, -2, 10, 2, true);
    t.text(92, 102, "MOTION", 2, C.GRAY);
    bar(t, 170, 101, 146, 18, 0, C.RED, "still", C.GREEN);
    t.text(92, 126, "HOLD", 2, C.GRAY);
    bar(t, 170, 125, 146, 18, 0.6, C.CYAN);
    t.text(92, 152, "130° P+01 R-02", 2, C.WHITE);
    frameEnd(t, [["Holding...", C.GREEN]], QUIT, FINISH, f);
  } },

  mountedOut: { stage: "2", draw(t, f = 0) {
    header(t, "2", "16/36");
    grid3(t, ["GGRGRRGRGRRR", "GGGGRGGRGGGR", "RRGRRRGRRRRR"], [0, 6], false, 200, 18);
    horizon(t, 4, 100, 80, -12, 3, 10, 2, false);
    t.text(92, 102, "MOTION", 2, C.GRAY);
    bar(t, 170, 101, 146, 18, 0.4, C.RED, "turning", C.WHITE);
    t.text(92, 126, "HOLD", 2, C.GRAY);
    bar(t, 170, 125, 146, 18, 0, C.CYAN);
    t.text(92, 152, "200° P+18 R+03", 2, C.WHITE);
    frameEnd(t, [["Nose up to 30", C.YELLOW]], QUIT, FINISH, f);
  } },

  hdgReady: { stage: "3", draw(t, f = 0) {
    hdgCal(t, f, "123°", 0.8, -0.4, true, 1, "P+0.8 R-0.4", [["Ready - press MARK", C.GREEN]], "go");
  } },

  hdgNotLevel: { stage: "3", draw(t, f = 0) {
    hdgCal(t, f, "126°", 4.1, -1.2, false, 0, "P+4.1 R-1.2", [["Not level", C.RED]], "dim");
  } },

  marked: { stage: "3", draw(t) {
    header(t, "3", "5/12");
    t.text(70, 44, "MARKED", 5, C.GREEN);
    t.text(52, 108, "TGT 120°  IND 123°", 2, C.WHITE);
    t.text(64, 134, "err +3°  avg +2°", 2, C.YELLOW);
    frameEnd(t, [["Next: ", C.GRAY], ["150°", C.WHITE]],
             { label: "hold: QUIT", style: "dim" }, { label: "MARK", style: "dim" });
  } },

  upload: { stage: "2", draw(t) {
    header(t, "2", "UPLOAD");
    t.text(8, 44, "Uploading", 3, C.WHITE);
    t.text(8, 80, "14 s", 3, C.YELLOW);
    t.text(8, 128, "Takes up to 30 s.", 2, C.GRAY);
    t.text(8, 150, "The unit is busy.", 2, C.GRAY);
    frameEnd(t, [["Please wait", C.GRAY]], BLANK, BLANK);
  } },

  resBase: { stage: "1", draw(t, f = 0) {
    result(t, f, "1", "9%", "OK", C.YELLOW, "RMS error", "4 orientation gaps",
           [["Press ACCEPT to install", C.WHITE]]);
  } },

  resMount: { stage: "2", hold: "REJECT", draw(t, f = 0) {
    result(t, f, "2", "14%", "POOR", C.RED, "RMS error", null,
           [["Next: ", C.GRAY], ["run 2 again", C.RED]]);
  } },

  resHdg: { stage: "3", draw(t, f = 0) {
    result(t, f, "3", "2.1°", "GOOD", C.GREEN, "likely worst error", null,
           [["Press ACCEPT to install", C.WHITE]]);
  } },

  saving: { stage: "1", draw(t) {
    header(t, "1", "SAVING");
    t.text(8, 36, "9%", 6, C.YELLOW);
    t.text(316 - tw("OK", 4), 44, "OK", 4, C.YELLOW);
    t.text(8, 96, "RMS error", 2, C.GRAY);
    t.text(8, 120, "4 orientation gaps", 2, C.GRAY);
    frameEnd(t, [["Saving...", C.GREEN]], BLANK, BLANK);
  } },

  installedBase: { stage: "1", draw(t) {
    header(t, "1", "SAVED");
    t.text(8, 44, "Installed", 3, C.GREEN);
    t.text(8, 84, "Gap-fill map: 9 cells", 2, C.WHITE);
    t.text(8, 104, "ready on the unit.", 2, C.WHITE);
    frameEnd(t, [["Next: ", C.GRAY], ["1a FILL GAPS", C.WHITE]], BLANK, OK);
  } },

  installedNoMap: { stage: "1", draw(t) {
    header(t, "1", "SAVED");
    t.text(8, 44, "Installed", 3, C.GREEN);
    t.text(8, 84, "Gap-fill map not", 2, C.YELLOW);
    t.text(8, 104, "updated: timeout", 2, C.YELLOW);
    frameEnd(t, [["Next: ", C.GRAY], ["sync at tern.local", C.WHITE]], BLANK, OK);
  } },

  installedHdg: { stage: "3", draw(t) {
    header(t, "3", "SAVED");
    t.text(8, 44, "Installed", 3, C.GREEN);
    t.text(8, 84, "Heading cal is on.", 2, C.WHITE);
    frameEnd(t, [["All 3 steps done", C.GREEN]], BLANK, OK);
  } },

  gapSent: { stage: "1a", draw(t) {
    header(t, "1a", "SENT");
    t.text(8, 44, "Uploaded", 3, C.GREEN);
    t.text(8, 84, "Patch RMS 8%", 2, C.WHITE);
    t.text(8, 108, "3 cells still thin", 2, C.GRAY);
    frameEnd(t, [["Next: ", C.GRAY], ["merge on website", C.WHITE]], BLANK, OK);
  } },

  offline: { stage: "2", draw(t) {
    header(t, "2", "OFFLINE");
    t.text(8, 44, "No WiFi", 3, C.YELLOW);
    t.text(8, 84, "Samples are saved", 2, C.WHITE);
    t.text(8, 104, "on the unit.", 2, C.WHITE);
    frameEnd(t, [["Next: ", C.GRAY], ["upload at tern.local", C.WHITE]], BLANK, OK);
  } },

  failed: { stage: "2", draw(t) {
    header(t, "2", "FAILED");
    t.text(8, 44, "Upload failed", 3, C.RED);
    t.text(8, 84, "HTTP -3: connection", 2, C.GRAY);
    t.text(8, 104, "reset", 2, C.GRAY);
    frameEnd(t, [["Next: ", C.GRAY], ["retry at tern.local", C.WHITE]], BLANK, OK);
  } },
};

// What a completed hold leads to.
const AFTER = {
  QUIT(t, stage) {
    header(t, stage, "");
    t.text(8, 52, "Quit", 4, C.WHITE);
    t.text(8, 104, "Nothing was uploaded.", 2, C.GRAY);
    frameEnd(t, [["Back to the nav screen", C.GRAY]], BLANK, BLANK);
  },
  REJECT(t, stage) {
    header(t, stage, "");
    t.text(8, 52, "Rejected", 4, C.WHITE);
    t.text(8, 104, "Your old cal stays on.", 2, C.GRAY);
    frameEnd(t, [["Back to the nav screen", C.GRAY]], BLANK, BLANK);
  },
};
