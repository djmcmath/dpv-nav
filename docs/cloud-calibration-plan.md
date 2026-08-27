# Cloud Calibration Plan (Firmware Side)

Companion to Dive Map's [device-calibration-plan.md](../../divemap/docs/architecture/device-calibration-plan.md),
which designs the backend half (`calibration-processor`, the `calibrations` table, the
`POST /api/device/uploads/{id}/calibrate` endpoint). This note designs the dpv-nav side:
what has to change on the unit to make magnetometer calibration a WiFi round-trip
instead of a laptop errand.

## Goal

Today: **CAL > Baseline** (or **Mounted**) fills a bin-coverage grid, saves a raw CSV to
LittleFS, and the diver has to carry that CSV to a laptop, run
`tools/mag_calibration.py`, and upload the resulting `mag_base.json`/`mag_mount.json`
back through the on-device web file manager (see
[calibration-guide.md](./calibration-guide.md)).

Target: the unit uploads the CSV itself once the bin grid completes, gets a fitted
result and an RMS-based verdict back over the same connection, and shows the diver
either "done, reload applied" or a rejectable warning — no laptop step.

The bin-coverage collection UI, the CSV format, and the two-stage baseline/mounted
design are unchanged. Only what happens *after* the CSV is written changes.

---

## What already exists and needs no change

- **Two-stage collection.** `CAL > Baseline` / `CAL > Mounted` (menu.cpp CAL submenu)
  already produce `/mag_baseline_samples.csv` and `/mag_mounted_samples.csv` on LittleFS
  via the bin-aware collector, with `showMagCalProgress(...)` driving the on-screen grid.
- **The hot-reload path.** `web_server`'s `/api/reload-cal` handler and
  `isReloadCalRequested()`/`clearReloadCalRequest()` ([web_server.h](../src/net/web_server.h))
  already exist to apply a newly-written cal file without a reboot. Today the only
  caller is "diver clicked Reload Cal on the file-manager web page" — this feature adds
  a second caller (the cloud-response handler), not new apply logic.
- **The JSON formats.** `storage::loadMagCalibration`/`saveMagCalibration`
  ([storage.h](../src/util/storage.h)) already read/write the exact `MagCalib` shape
  (`bias` + 3×3 `softIron`) that `calibration-processor` will hand back.
- **WiFi connectivity state.** `wifi::isStaConnected()`, `wifi::staSSID()`
  ([wifi_manager.h](../src/net/wifi_manager.h)) already track whether the unit has a
  usable network — the offline-warning check is a read of existing state, not new
  connectivity logic.
- **The accept/reject UI pattern.** Speed cal's RESULT screen already does exactly this
  interaction — BTN1 cycles **RESET+ACCEPT / ACCEPT / REJECT**, BTN2 confirms (see
  `SpeedCalPhase` in `display_main.cpp`, documented in CLAUDE.md's Menu System section).
  The calibration accept/reject screen should reuse this pattern rather than invent a
  new one — it's the closest sibling to "show a computed result and let the diver keep
  or discard it" already in the codebase.

## What's genuinely new

**dpv-nav has no outbound network client today.** `wifi_manager` only manages
connectivity; `web_server` only serves inbound requests. Neither dive-log upload nor
calibration upload has ever made an HTTP request *from* the device. This is the real
scope of this feature, and it's shared: whatever gets built here is the same client
dive-log upload will need later, so it should not be written calibration-specific.

### 1. Shared cloud client module (new: `src/net/cloud_client.h/.cpp`, namespace `cloud::`)

- HTTPS client using the ESP32 Arduino core's `HTTPClient` + `WiFiClientSecure`. Needs a
  real CA validation story for divemap.diverdaniel.com (a pinned cert or CA bundle) —
  **not** `WiFiClientSecure::setInsecure()`. This carries a device-auth bearer token;
  sending it over an unverified TLS connection isn't acceptable.
- Device-auth token bootstrap (RFC 8628 device grant against
  `POST /api/device/authorize` / `POST /api/device/token`, per the backend's Phase 1
  plan) and persistence — likely NVS alongside the existing `nvs_nav`/`nvs_disp`
  namespaces, given the token needs to survive reboots and this project already has a
  working NVS persistence pattern to copy.
- A generic `cloud::uploadFile(kind, path, ...)` / `cloud::postJson(...)` pair, so it's
  not hardcoded to calibration's specific endpoints.
- **Not scoped here:** wiring this client into the dive-log upload flow itself. That's
  a separate follow-up once this module exists — this plan only needs enough of it to
  drive calibration.

### 2. Calibration-specific flow (new: hooks into `menu`/`nav_main`/`display`)

On bin-grid completion (`CAL > Baseline`/`Mounted` finishing, i.e. right after the
existing "DONE" screen and CSV save):

1. **Connectivity check.** If `!wifi::isStaConnected()`: show a new
   `showCalUploadWarning("Connect to WiFi to finish calibration")` screen (or similar,
   modeled on the existing full-screen cal prompt functions in `display.h`) and leave
   the CSV in place — untouched, retryable later. Do not block the diver from using the
   unit locally in the meantime; this is additive to the existing offline-capable
   design, not a new requirement to be online.
2. **Upload.** `cloud::uploadFile("calibration_raw", "/mag_baseline_samples.csv", ...)`
   → `POST /api/device/uploads`. For `Mounted`, the backend resolves the device's prior
   accepted baseline itself (per the backend plan) — the firmware does not need to
   re-send it.
3. **Trigger + await result.** `POST /api/device/uploads/{id}/calibrate` with
   `{ mode: "baseline" | "mounted" }`, synchronous. Backend plan's open question about
   whether this stays synchronous (vs. needing an async/poll fallback if the fit turns
   out slow) applies here too — the firmware should tolerate a timeout and show a
   retry/offline-style message rather than hang indefinitely.
4. **Show the verdict.** New display function, e.g.
   `showCalResult(rms, qualityBand, recommendation)`, reusing the speed-cal
   accept/reject interaction: BTN1 cycles ACCEPT/REJECT, BTN2 confirms. Message text
   comes from the backend's `recommendation` field (quality thresholds are a backend
   decision — see the backend plan's open question on RMS bands).
5. **On accept:** write the returned `cal_json` to the correct path
   (`/mag_base.json` or `/mag_mount.json`) via the existing `storage::saveMagCalibration`,
   then set the reload-cal request flag so the existing hot-reload path
   (`web::isReloadCalRequested()`) picks it up — reusing the exact mechanism built for
   the manual-upload case, just with a new caller. Tell the diver the next step
   (Mounted, if Baseline just completed; otherwise done).
6. **On reject:** discard the result, leave the currently-loaded calibration active,
   tell the diver to re-collect (return to `CAL > Baseline`/`Mounted`).
7. **On network failure mid-flow:** never touch the currently-active cal file until a
   result is both received and accepted — write-new-then-swap, not overwrite-in-place.
   Show a failure message distinct from the offline-warning message (e.g. "upload
   failed — check connection and retry" vs. "not connected").

---

## Risks & Open Decisions

- **CA/TLS story for the ESP32 client is undecided.** Needs a concrete plan (embedded
  root CA, or a smaller pinned cert) before Phase 1 (shared client) can be built —
  flagging rather than picking one here. **Resolved by picking a pinned cert
  (see below), which turned out to be the wrong call on its own.**
- **Pinned root CA baked into firmware means every CA rotation needs a USB reflash
  of every unit in the field — this bit us for real on 2026-08-22.** `cloud_client.cpp`
  hardcodes `CLOUD_ROOT_CA_PEM` as a compile-time constant, and there's no OTA update
  path (`ArduinoOTA`/`Update.begin()`) anywhere in this firmware — only LittleFS
  *data* files (`menu.json`, `mag_base.json`, `hdg_fourier.json`, ...) can be pushed
  via the existing web upload UI ([net/web_server.cpp](../src/net/web_server.cpp)),
  not the firmware binary itself. When `divemap.diverdaniel.com`'s cert chain moved
  to Let's Encrypt's new "Generation Y" root hierarchy after their 2026-05-13 default-profile
  cutover, the pinned ISRG Root X1-only cert stopped validating and account-linking
  silently hung on "Requesting code...". Fixed for now by pinning all three currently
  live self-signed roots (YE, X2, X1) instead of just one, but that's a delay, not a
  fix: the next Let's Encrypt root rotation hits every already-deployed unit the same
  way, and the *only* remedy is finding it, plugging in a USB cable, and reflashing —
  exactly the failure mode this project's own goal statement (units that "log in to
  Dive Map on its own... as soon as it finds a connection") was trying to avoid.
  ~~**Real fix, not yet built:** move `CLOUD_ROOT_CA_PEM` out of compiled firmware into
  a LittleFS file (e.g. `/cloud_ca.pem`), loaded at runtime the same way
  `hdg_cal::load()` reads `/hdg_fourier.json`, with the compiled-in bundle kept only
  as a first-boot default. A stale/soon-to-expire root could then be refreshed the
  same way `mag_base.json` already is — upload a new file over the existing web UI,
  no reflash, no USB — and would also let a still-linked unit self-update its own CA
  file autonomously via the very cloud connection this plan is building, the first
  time it successfully reaches the server after a new root ships server-side.~~
  **Resolved differently (2026-08-27): CA pinning removed outright, not made
  refreshable.** `CLOUD_ROOT_CA_PEM` and `rootCaConfigured()` are gone; every
  `WiFiClientSecure` in `cloud_client.cpp` now calls `setInsecure()` instead of
  `setCACert(...)` — TLS still encrypts the connection, but the server's certificate
  chain is no longer validated against any trust anchor, so there is no CA left to
  rotate or refresh. This trades away server authentication rather than building
  machinery to keep it working: the realistic threat (someone actively MITM-ing a
  hobbyist's dive-computer WiFi session to corrupt calibration data or lift the
  bearer token) was judged not worth defending against for what this payload is.
  The actual security boundary is unchanged and unaffected by this — it's the bearer
  token from the device-auth flow below, checked server-side against a DB-stored
  hash (`DeviceAuth` in dive-map's `auth.rs`), independent of how or whether the TLS
  layer validates the server's identity.
- **Where the current bin-aware collector's source lives wasn't pinned down precisely**
  during this planning pass (CLAUDE.md and calibration-guide.md describe its behavior,
  but the source file wasn't conclusively located — likely `menu.cpp`/`display_main.cpp`/
  `nav_main.cpp`). Needs a quick grep for `mag_baseline_samples`/`mag_mounted_samples`
  before touching the completion hook in step 1 above.
- **Device-auth token storage** needs to decide whether it lives in the same
  `Preferences`/NVS namespaces as `nvs_nav`/`nvs_disp`, or its own — likely its own,
  since it's a security credential rather than a UI toggle, but worth a deliberate
  choice rather than defaulting into an existing namespace.
- **Synchronous `POST .../calibrate` blocking the UI thread.** `nav_main`/`display_main`
  are real-time loops (100 Hz sensor loop, 10 Hz serial link); an HTTP round-trip needs
  to not stall either device's main loop for multiple seconds. Needs a non-blocking
  request pattern (state machine, like `wifi_manager`'s reconnect logic or speed cal's
  phase machine) rather than a blocking `HTTPClient` call in the loop.
- **This plan covered magnetometer calibration only; the Fourier heading cal has
  since gotten its own cloud round trip** — see divemap's
  [heading-cal-cloud-plan.md](../../divemap/docs/architecture/heading-cal-cloud-plan.md),
  which reuses this plan's shared cloud client and the same
  `gCloudCalPhase` wait/result UI. That plan originally deferred a real
  fit-adequacy check beyond a single RMS number — today's fixed 12-point
  collection can still leave one sector thin (real case, 2026-07-24: only 4 of
  the 12 points fell in a 330°-060° span with a large swing, giving a
  mediocre fit there until 315/345/015/045 were added by hand). **Solved
  2026-07-28**, not on-device: divemap's
  [calibration-session-merge-plan.md](../../dive-map/docs/architecture/calibration-session-merge-plan.md)
  added server-side `sector_adequacy` reporting plus a website manual-entry
  form (Phase A) that lets a diver type in `(actual, indicated)` readings for
  the suggested headings and re-fit, without any firmware change or
  re-collection. A firmware-driven targeted-resample UI (Phase B) remains
  unbuilt and is intentionally sequenced after real-world use of Phase A.
- **Dropping TLS entirely (plain HTTP to divemap.diverdaniel.com) was considered and
  rejected for now — flagged as a possible future step, not a plan.** The payloads
  themselves (cal CSVs, eventually track logs) aren't sensitive, but the RFC 8628
  bearer token persisted by `cloud::isAuthorized()`/`loadToken()`/`saveToken()` is a
  long-lived credential with no visible expiry/rotation, and it would ride in
  cleartext on every request over plain HTTP, readable by anyone with passive path
  visibility (shared WiFi at a marina/dive shop, any hop to the backend) — not just
  an active attacker. The ~150KB flash saved (mbedcrypto + mbedtls + mbedx509 +
  WiFiClientSecure; WiFi/lwIP stay regardless, since the local file-browser AP needs
  them) is real but was not, by itself, judged worth that tradeoff once
  `partitions_nav.csv` (see below) recovered flash headroom a different way. This
  stands as-is — TLS itself is still in place; nothing below removed it.
  **What *was* removed (2026-08-27, see the entry above): server authentication.**
  `setInsecure()` means an active MITM can now impersonate the backend (feed back a
  forged calibration fit) or intercept the bearer token in a way passive sniffing
  never could over an unauthenticated-but-encrypted channel — the same two risks
  this paragraph originally raised, minus the passive-sniffing case that dropping to
  plain HTTP would have added on top. That residual active-MITM risk was weighed and
  accepted directly, not overlooked: judged impractical enough for this project's
  real threat model (a hobbyist device uploading calibration data) to not be worth
  the CA-maintenance burden that avoiding it requires.

---

## Implementation order

1. ✅ Locate/confirm the bin-aware CSV collector's completion hook — found at
   `nav_main.cpp`'s `imu::magBinCalIsComplete()` block.
2. **Not resolved** — TLS/CA approach for `cloud::` client (see Status below).
3. ✅ `src/net/cloud_client.h/.cpp`: device-auth bootstrap + NVS persistence, upload/fit/
   respond helpers.
4. ✅ Wired the calibration completion flow (connectivity check → upload → trigger →
   result) into `nav_main.cpp`.
5. ✅ New display functions and full Serial1 protocol (see Status below).
6. ✅ Wired accept → copy pending file over active + `loadCalibration()`; reject →
   discard pending file. Both call `cloud::respondToCalibration()`.
7. Update [calibration-guide.md](./calibration-guide.md) once this ships to real
   hardware — done in doc form already (pointer added), full rewrite pending a real
   field test.

## Status

Steps 1, 3, 4, 5, 6 are built and compile clean on both `nav` and `display`
PlatformIO environments (`pio run -e nav` / `-e display`, verified in this session).
Step 2 (TLS root CA) is the one explicit gap — see below.

### What shipped, and where it deviated from the sketch above

- **No separate "uploading" packet from the nav device.** The original sketch
  implied the display would need nav to announce "upload started." In practice the
  nav device is *blocked* on the network call the instant upload starts (see the
  blocking-loop risk below), so it can't send anything until the call returns anyway.
  Instead, the display enters its waiting screen **locally**, the moment its own
  bin-coverage "DONE" hold (`CAL_COMPLETE_HOLD_MS`) ends — no data from nav needed for
  that transition. The nav device sends exactly one new packet type
  (`CalCloudResultPacket`), after the blocking call finishes (or immediately, if
  skipped for no WiFi).
- **The link-timeout ("NO LINK") screen had to be deliberately overridden.** Because
  the nav device blocks for up to ~30s uploading, no `NavPacket`s arrive during that
  window, and the display's existing 5-second `NAV_TIMEOUT_MS` would otherwise show
  "NO LINK" mid-upload — which looks like a crash, not a wait. `gCloudCalPhase` now
  forces `linkAlive = true` for the duration of the cloud-cal UI. A 40-second
  display-side timeout (longer than the nav device's own worst case) still catches a
  genuinely wedged nav device and falls back to a dismissible "no response" screen.
- **Command buffers grew from 64 to 96 bytes** (`cmdBuf` in `nav_main.cpp`, `txBuf` in
  `display_main.cpp`) — the calibration id is a 36-character UUID, and the old 64-byte
  buffers were sized for short menu commands only.
- **Resolved: TLS root CA (2026-07-23).** `cloud_client.cpp`'s `CLOUD_ROOT_CA_PEM` is
  now populated with ISRG Root X1 (Let's Encrypt), fetched directly from
  letsencrypt.org rather than off the live chain — divemap.diverdaniel.com's cert
  terminates there. `rootCaConfigured()` still fails closed if it's ever emptied out.
- **Cloud account link (2026-07-23).** Device-auth bootstrap (RFC 8628) is now wired
  end to end, not just present in `cloud_client.h`/`.cpp`: a "Link acct" item in the
  Config menu (next to WiFi — GPS Pos/GPS Spd were also merged into a single GPS
  toggle in the same pass, the split having only ever existed for testing) sends
  `DisplayCmd::LINK_ACCOUNT`. The nav device calls the now-two-phase
  `cloud::beginAuthorize()` / `cloud::pollAuthorize()` (split so the user/device code
  can reach the display before the up-to-10-minute poll blocks) and reports back via
  a new `CloudLinkResultPacket`, mirroring the existing `CalCloudResultPacket`
  pattern. Same blocking tradeoff as the calibration upload, just a much longer
  window — accepted as a one-time surface-side setup action, no cancel path.
- **Flash budget was tight, now resolved via partition resize (2026-07-23).** Adding
  the actual call site (not just the module) pushed the `nav` environment from ~81% to
  ~93% flash usage (1.22 MB / 1.31 MB default app partition) — the linker had been
  discarding the unreferenced `cloud_client.cpp` object entirely until something called
  into it. Rather than trim features, `partitions_nav.csv` (referenced via
  `board_build.partitions` in the `[env:nav]` section of `platformio.ini`) reallocates
  640KB from the oversized LittleFS/spiffs data partition (1408KB → 768KB — still well
  above the ~500KB worst-case single dive log plus config files; see
  [data-logging-guide.md](./data-logging-guide.md)) into `app0`/`app1` (1280KB → 1600KB
  each), preserving dual-OTA capability. Current usage is now ~74.6% (1.22 MB / 1.56
  MB), i.e. ~400KB of headroom for dive-log upload and whatever comes next.
- **Not yet verified against real hardware or a real backend.** Both environments
  compile clean, but nothing here has been exercised against actual WiFi, an actual
  divemap deployment, or real ESP32 boards — same caveat every other phase of this
  project has flagged for its own first pass.
- **Cloud account link revised: non-blocking poll + shorter code (2026-07-23).** The
  first real link attempt exposed two problems with the design above: the "no cancel
  path" tradeoff was worse in practice than expected (the diver had no way out of the
  wait screen at all, since `pollAuthorize()` blocked the nav core and the display core
  deliberately disabled BTN2 during the wait), and `beginAuthorize()` was displaying
  `verification_uri_complete` (the code baked into a query string) instead of the short
  `verification_uri`, making the on-screen URL needlessly long to type. Fixed:
  `cloud::pollAuthorize()` is gone, replaced by a non-blocking state machine
  (`startAuthorizePoll` / `updateAuthorizePoll` / `cancelAuthorizePoll` /
  `getAuthorizePollStatus`) ticked from `nav_main.cpp`'s main loop alongside
  `wifi::update()`; a new `DisplayCmd::CANCEL_LINK` lets BTN2 back out at any point.
  The backend's `user_code` also shrank from an 8-char `XKCD-7291`-style code to 4
  numeric digits (shorter TTL, attempt-capped on the activate endpoint) — see
  divemap's `docs/architecture/device-uploads-plan.md` for the server-side rationale.
- **Cloud account link revised again: the URL is gone entirely (2026-07-24).** The
  diver still had to read a short URL off the device screen and type it into a phone
  browser. That's removed: `POST /api/device/authorize` no longer returns
  `verification_uri`/`verification_uri_complete` at all, `CloudLinkResultPacket` no
  longer carries a `verification_uri` field, and `cloud::beginAuthorize()` dropped
  that output parameter entirely. The device screen (`showCloudLinkWaiting()` in
  `drivers/display.cpp`) now shows only the 4-digit code, large, with instructional
  text pointing at "My Devices" in account settings instead of a URL. The
  divemap frontend already had a "Link a device" link there
  ([UserSettingsModal.svelte](/Users/djmcmath/Documents/dive-map/frontend/src/components/UserSettingsModal.svelte)),
  so no new entry point was needed on that side — see divemap's
  `docs/architecture/device-uploads-plan.md` ("Revision 2") for the full rationale,
  including why this stayed device-initiated rather than flipping to
  website-initiated.
- **Upload crash on large collections, found and fixed (2026-07-26).** Live testing
  for divemap's `baseline-cal-coverage-feedback-plan.md` (which pushed baseline
  collections to ~1000+ samples, well past what earlier testing had exercised) hit a
  real crash: `runCalibrationUpload()`'s `readFileToBuffer()` read the whole samples
  CSV into one heap allocation (`new uint8_t[len]`) before POSTing it. At ~1000+
  samples (~85KB CSV) that single contiguous allocation reliably failed against the
  WiFi/TLS-constrained heap, and the uncaught `bad_alloc` took the whole device down
  (panic -> `rst:0xc SW_CPU_RESET`) mid-upload rather than failing gracefully. This is
  almost certainly what earlier "no response" reports in that testing round actually
  were — nav had died and was rebooting, not hung, so the display's 40s no-response
  timeout fired. Root-caused via `xtensa-esp32-elf-addr2line` against the built
  `firmware.elf` (backtrace led straight to `readFileToBuffer`/`operator new[]`
  inside `runCalibrationUpload`). Fixed by streaming the CSV directly from LittleFS
  via `HTTPClient::sendRequest("POST", &file, size)` instead of buffering it at all —
  `readFileToBuffer()` is gone. Confirmed: reproduced the crash at 1030/1073 samples
  pre-fix, then re-ran an identical 1073-sample collection post-fix cleanly (no
  crash). No size ceiling on baseline collections now, by construction rather than by
  a raised-but-still-finite buffer size.
