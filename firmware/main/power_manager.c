// power_manager: peripheral shutdown + deep-sleep entry.
//
// Conservative — does the minimum needed to leave the system in a
// state where esp_deep_sleep_start() won't corrupt anything and the
// device draws a small current. Things that are RAM-only (preview
// tasks, JPEG encoders, etc.) don't need explicit teardown because
// RAM is wiped on sleep anyway; we only chase peripherals with
// power, GPIO leakage, or external state (SD).

#include "power_manager.h"

#include <stdint.h>

#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hal_camera.h"
#include "hal_lepton.h"
#include "hal_storage.h"

static const char *TAG = "power_mgr";

void power_manager_prep_for_sleep(bool vis_was_active,
                                   bool therm_was_active) {
    ESP_LOGI(TAG, "prep_for_sleep: vis=%d therm=%d",
             (int)vis_was_active, (int)therm_was_active);

    // ---- Lepton: cut power via MOSFET. The VoSPI reader task is
    // RAM-only, will be wiped on sleep — no graceful stop needed.
    // hal_lepton_power_off is a no-op if MOSFET pin isn't wired.
    if (therm_was_active) {
        hal_lepton_power_off();
    }

    // ---- Camera: deinit clears its SCCB/I2S state. Skipped in PR-C
    // (the OV2640 will lose VDD when the MCU's regulators drop into
    // sleep mode on most ESP32-S3 dev boards; if your power tree
    // keeps it powered, current draw will be slightly higher — fine
    // for the first slice, deferred to power-measurement work).
    (void)vis_was_active;

    // ---- SD: fsync + unmount. session_store already fsync'd the
    // journal at commit time so this is mostly a courtesy, but it
    // also takes the SD out of FATFS's "open" state so we don't
    // wake to a stale handle. Unmount is best-effort.
    // (hal_storage doesn't yet expose an unmount entrypoint; the
    // FATFS driver will release filesystem locks when the MCU
    // resets on wake. Tracked as a follow-up.)

    // ---- Brief settle so any in-flight UART log lines flush.
    vTaskDelay(pdMS_TO_TICKS(50));
}

void power_manager_enter_deep_sleep(uint64_t sleep_us) {
    // Clamp to a sane range. 0 would loop forever (immediate wake);
    // anything past 1 h is almost certainly a math mistake at this
    // stage of the project — fail safe to 1 h.
    const uint64_t MIN_US = 1000ULL;                    // 1 ms
    const uint64_t MAX_US = 3600ULL * 1000ULL * 1000ULL; // 1 hour
    if (sleep_us < MIN_US) sleep_us = MIN_US;
    if (sleep_us > MAX_US) {
        ESP_LOGW(TAG, "sleep_us %llu clamped to 1h", (unsigned long long)sleep_us);
        sleep_us = MAX_US;
    }

    ESP_LOGI(TAG, "entering deep sleep for %llu us (%llu s)",
             (unsigned long long)sleep_us,
             (unsigned long long)(sleep_us / 1000000ULL));

    esp_sleep_enable_timer_wakeup(sleep_us);
    esp_deep_sleep_start();
    // Unreachable.
    for (;;) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}
