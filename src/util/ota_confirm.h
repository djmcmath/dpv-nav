#pragma once

// OTA rollback confirmation, shared by both boards.
//
// The bootloader is built with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, so an
// image written by an OTA update boots in the PENDING_VERIFY state. If it
// resets for ANY reason before it is marked valid, the bootloader reverts to
// the previous slot on the next boot.
//
// By default the Arduino core marks a pending image valid inside initArduino(),
// before setup() even runs -- which proves nothing. Overriding the weak
// verifyRollbackLater() to return true skips that, and each board calls
// ota_confirm::markValid() only once it has proved it actually works:
//   nav     -- self-test passed (READY) with the display link round-trip OK
//   display -- first valid NavPacket parsed
//
// Header-only on purpose: the display env's build_src_filter lives in the
// untracked platformio.ini, so a new .cpp would silently not be compiled there.
// Include this from exactly ONE translation unit per board (its main), since
// it defines verifyRollbackLater().
//
// A USB-flashed image is never PENDING_VERIFY, so markValid() is a no-op there.

#include <Arduino.h>
#include <esp_ota_ops.h>

bool verifyRollbackLater() { return true; }

namespace ota_confirm {

inline void markValid() {
    static bool done = false;
    if (done) return;
    done = true;

    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        Serial.printf("[OTA] new image on %s confirmed valid (err=%d)\n",
                      running->label, (int)err);
    }
}

}  // namespace ota_confirm
