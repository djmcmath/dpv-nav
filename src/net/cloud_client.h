#pragma once

#include <Arduino.h>

// Shared outbound HTTPS client for divemap.diverdaniel.com: device-auth
// bootstrap (RFC 8628) + generic upload/fit helpers for magnetometer
// calibration. Not calibration-specific by design -- dive-log upload needs
// the identical device-auth + upload plumbing later.
//
// See docs/cloud-calibration-plan.md and
// divemap/docs/architecture/device-calibration-plan.md.
//
// This is a blocking client (HTTPClient's request calls block until they
// return or time out). That is acceptable for the one-shot calls below
// (upload/fit/respond) because every call here is user-initiated from the
// CAL menu, not something that runs during underwater navigation -- but a
// call in progress does stall the nav device's main loop for up to
// CLOUD_HTTP_TIMEOUT_MS. This proved disruptive for the up-to-10-minute
// account-link poll specifically (no way to cancel while blocked), so that
// one is a non-blocking state machine instead -- see startAuthorizePoll()
// below, modeled on wifi::update()'s reconnect logic.
namespace cloud {

// Has this unit completed the device-auth flow and stored a token in NVS?
bool isAuthorized();

// One-time device-auth bootstrap (device-code grant, loosely inspired by
// RFC 8628 but without a verification URI -- the diver never reads or types
// a URL. Linking is two independent, order-agnostic halves: this device
// flow, and the diver separately entering the same code under "My Devices"
// in their Dive Map account settings). Split into a quick blocking call
// followed by a non-blocking poll so the caller can show the diver their
// code and stay responsive (cancel button) while waiting:
//
//   1. beginAuthorize() -- single HTTP round trip against
//      POST /api/device/authorize, fills in deviceCodeOut (needed for step
//      2) and userCodeOut (for display -- this is the only thing shown to
//      the diver).
//   2. startAuthorizePoll(deviceCode) -- begins polling in the background.
//   3. updateAuthorizePoll() -- call once per main-loop tick. No-ops
//      between poll intervals; every CLOUD_AUTH_POLL_INTERVAL_MS it makes
//      one bounded (CLOUD_HTTP_TIMEOUT_MS) POST /api/device/token call and
//      updates the status returned by getAuthorizePollStatus(). On success
//      it persists the bearer token to NVS itself.
//   4. cancelAuthorizePoll() -- call if the diver backs out. Takes effect
//      immediately (nav_main's loop is single-threaded/single-core, so this
//      can only run between updateAuthorizePoll() ticks, never mid-request).
bool beginAuthorize(String& deviceCodeOut, String& userCodeOut, String& errorOut);

enum class AuthPollStatus : uint8_t {
    IDLE,      // no poll in progress (not started, cancelled, or already consumed)
    POLLING,   // waiting for the diver to approve
    APPROVED,  // token obtained and saved to NVS
    DENIED,    // diver rejected it
    EXPIRED,   // user_code TTL elapsed, or CLOUD_AUTH_POLL_TIMEOUT_MS elapsed client-side
    ERROR,     // network/server error; see lastAuthorizeError()
};

void           startAuthorizePoll(const String& deviceCode);
void           updateAuthorizePoll();
void           cancelAuthorizePoll();
AuthPollStatus getAuthorizePollStatus();
// Valid after getAuthorizePollStatus() returns ERROR; empty otherwise.
String         lastAuthorizeError();

// Result of a calibration upload + fit round trip.
struct CalibrationResult {
    bool   ok = false;          // false if any network/auth/fit step failed
    String errorMessage;        // set when !ok; safe to show the diver as-is
    String calibrationId;
    String qualityBand;         // "good" | "warn" | "bad"
    float  rmsPct = 0.0f;
    String recommendation;      // shown to the diver as-is, e.g. "Bad cal; 47% RMS. ..."
    int    coverageGaps = -1;   // baseline only; empty coverage-grid cells. -1 = not
                                // applicable (mounted fit, or a baseline CSV collected
                                // before the 9-axis firmware update). See divemap's
                                // baseline-cal-coverage-feedback-plan.md, step 5.
};

// Upload a raw calibration CSV and run the fit, end to end:
//   1. POST the CSV to /api/device/uploads (kind=calibration_raw)
//   2. POST /api/device/uploads/{id}/calibrate {mode}
//   3. write the returned cal JSON to outputPath (MagCalib shape, ready for
//      storage::loadMagCalibration)
//
// Caller is responsible for checking wifi::isStaConnected() first and
// showing the offline warning -- this function does not wait for
// connectivity, it fails fast if there is none.
//
// mode is "baseline" or "mounted". For "mounted", the backend resolves the
// device's own most recent accepted baseline itself; nothing extra is sent.
CalibrationResult runCalibrationUpload(const char* mode, const char* csvPath,
                                        const char* outputPath);

// Records the diver's accept/reject decision for a completed calibration
// run. Returns false on any network/auth failure; the caller keeps showing
// the result screen and can retry.
bool respondToCalibration(const String& calibrationId, bool accepted);

}  // namespace cloud
