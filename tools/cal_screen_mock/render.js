// Renders the calibration screen mockups (screens.js) to one PNG per screen
// and to cal-screens.html, the review page.
//
//   node tools/cal_screen_mock/render.js
//
// The font is read from the Adafruit GFX library PlatformIO installs for the
// display build, so run `pio run -e display` once first on a fresh checkout.
const fs = require("fs"), vm = require("vm"), zlib = require("zlib"), path = require("path");

const dir = __dirname;
const glcd = path.join(dir, "../../.pio/libdeps/display/Adafruit GFX Library/glcdfont.c");
if (!fs.existsSync(glcd)) {
  console.error(`missing ${glcd}\nrun \`pio run -e display\` once to install the GFX library`);
  process.exit(1);
}
const src = fs.readFileSync(glcd, "utf8");
const bytes = src.slice(src.indexOf("{") + 1, src.lastIndexOf("}")).match(/0x[0-9A-Fa-f]{2}/g);
if (!bytes || bytes.length !== 1280) throw new Error(`expected 1280 font bytes, got ${bytes && bytes.length}`);
const fontHex = bytes.map(b => b.slice(2).toLowerCase()).join("");
const screensJs = `const FONT_HEX = "${fontHex}";\n` + fs.readFileSync(path.join(dir, "screens.js"), "utf8");

const ctx = {};
vm.createContext(ctx);
vm.runInContext(screensJs + "\nthis.SCREENS = SCREENS; this.TFT = TFT; this.AFTER = AFTER;", ctx);

const crcT = new Uint32Array(256).map((_, n) => {
  let c = n;
  for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
  return c >>> 0;
});
const crc = b => { let c = 0xFFFFFFFF; for (const x of b) c = crcT[(c ^ x) & 255] ^ (c >>> 8); return (c ^ 0xFFFFFFFF) >>> 0; };
function chunk(type, data) {
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type), data]);
  const c = Buffer.alloc(4); c.writeUInt32BE(crc(td));
  return Buffer.concat([len, td, c]);
}
function png(t, S) {
  const W = 320 * S, H = 240 * S, raw = Buffer.alloc((W * 3 + 1) * H);
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    const c = t.buf[Math.floor(y / S) * 320 + Math.floor(x / S)];
    const o = y * (W * 3 + 1) + 1 + x * 3;
    raw[o] = ((c >> 11) & 31) * 255 / 31; raw[o + 1] = ((c >> 5) & 63) * 255 / 63; raw[o + 2] = (c & 31) * 255 / 31;
  }
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(W, 0); ihdr.writeUInt32BE(H, 4); ihdr[8] = 8; ihdr[9] = 2;
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", ihdr),
    chunk("IDAT", zlib.deflateSync(raw)), chunk("IEND", Buffer.alloc(0))]);
}

const out = path.join(dir, "png");
fs.mkdirSync(out, { recursive: true });
for (const id of Object.keys(ctx.SCREENS)) {
  const t = new ctx.TFT();
  ctx.SCREENS[id].draw(t, 0);
  fs.writeFileSync(path.join(out, `${id}.png`), png(t, 2));
}
for (const id of Object.keys(ctx.AFTER)) {
  const t = new ctx.TFT();
  ctx.AFTER[id](t, "2");
  fs.writeFileSync(path.join(out, `after-${id.toLowerCase()}.png`), png(t, 2));
}

const page = fs.readFileSync(path.join(dir, "page.template.html"), "utf8");
fs.writeFileSync(path.join(dir, "cal-screens.html"), page.replace("/*__SCREENS_JS__*/", screensJs));
console.log(`wrote ${Object.keys(ctx.SCREENS).length + Object.keys(ctx.AFTER).length} PNGs to ${out} and cal-screens.html`);
