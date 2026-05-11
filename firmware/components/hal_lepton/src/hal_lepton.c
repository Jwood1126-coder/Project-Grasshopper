// Public glue between hal_lepton.h and the cci+vospi internals.
// Single boot routine that powers the Lepton, runs CCI configuration,
// brings up the VoSPI reader, waits for the first frame, runs FFC.

#include "hal_lepton.h"
#include "lepton_internal.h"
#include "board_pins.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "hal_lepton";

static void power_cycle(void) {
    if (LEP_POWER_PIN < 0) return;
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << LEP_POWER_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    // Force a real off → on cycle. ESP32 resets don't clear the MOSFET
    // state, so the Lepton may be stuck in a bad mode (only emitting
    // 4-byte headers) from a previous boot. Drive LOW for 1 s so the
    // Lepton power supply fully drops.
    gpio_set_level(LEP_POWER_PIN, 0);
    ESP_LOGI(TAG, "MOSFET off (gpio %d) — power-cycle pause 1s", LEP_POWER_PIN);
    vTaskDelay(pdMS_TO_TICKS(1000));
    gpio_set_level(LEP_POWER_PIN, 1);
    ESP_LOGI(TAG, "MOSFET on");
}

esp_err_t hal_lepton_boot(void) {
    power_cycle();
    // Fox uses 5s here. Phase 3 sometimes converged at 2s + many
    // sync-fail retries, but post-cycle Leptons appear to need the
    // full ~5s for the OEM module to fully initialize — observed via
    // OEM_FW_VER GET returning DATA_LEN=0 (Lepton not ready to
    // respond) at 2s but populated values at 5s+.
    ESP_LOGI(TAG, "waiting 5s for Lepton boot (matches Fox)...");
    vTaskDelay(pdMS_TO_TICKS(5000));

    esp_err_t err = lepton_cci_init();
    if (err != ESP_OK) return err;
    // Diagnostic snapshot BEFORE configure — see what factory defaults
    // the Lepton booted into.
    lepton_cci_dump_state();
    err = lepton_cci_configure();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CCI configure failed");
        return err;
    }
    // Diagnostic snapshot AFTER configure — confirm our SETs took.
    lepton_cci_dump_state();

    // EXPERIMENT: issue FFC before waiting for first frame. Fox runs
    // FFC AFTER the first frame, but in our post-cycle-storm state the
    // Lepton never emits a first frame to trigger FFC. FFC sometimes
    // kicks a stuck pixel pipeline into life by forcing a calibration
    // refresh — worth trying as a one-shot.
    ESP_LOGW(TAG, "pre-emptive FFC (before first frame)...");
    if (lepton_cci_run_ffc() == ESP_OK) {
        ESP_LOGI(TAG, "pre-emptive FFC done");
        // Give Lepton 1 s after FFC to settle.
        vTaskDelay(pdMS_TO_TICKS(1000));
    } else {
        ESP_LOGW(TAG, "pre-emptive FFC failed");
    }

    err = lepton_vospi_init();
    if (err != ESP_OK) return err;

    lepton_vospi_start();

    // Wait up to 30s for first frame.
    int waited = 0;
    while (lepton_vospi_frame_count() == 0 && waited < 30000) {
        vTaskDelay(pdMS_TO_TICKS(250));
        waited += 250;
    }
    if (lepton_vospi_frame_count() == 0) {
        ESP_LOGE(TAG, "no frame after %d ms", waited);
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "first frame received after %d ms", waited);

    // Initial FFC — Fox does this once after first frame.
    if (lepton_cci_run_ffc() == ESP_OK) {
        lep_last_ffc_ms = (uint32_t)(esp_timer_get_time() / 1000);
        ESP_LOGI(TAG, "initial FFC done");
    } else {
        ESP_LOGW(TAG, "initial FFC failed");
    }

    return ESP_OK;
}

bool hal_lepton_get_frame(uint16_t *dst) {
    return lepton_vospi_get_frame(dst);
}

uint32_t hal_lepton_frame_count(void) {
    return lepton_vospi_frame_count();
}

esp_err_t hal_lepton_run_ffc(void) {
    esp_err_t e = lepton_cci_run_ffc();
    if (e == ESP_OK) lep_last_ffc_ms = (uint32_t)(esp_timer_get_time() / 1000);
    return e;
}

esp_err_t hal_lepton_set_agc(bool enable) { return lepton_cci_set_agc(enable); }
esp_err_t hal_lepton_set_gain_mode(int mode) { return lepton_cci_set_gain(mode); }
bool hal_lepton_agc_enabled(void) { return lepton_cci_agc_enabled(); }
int  hal_lepton_gain_mode(void)   { return lepton_cci_gain_mode(); }
void hal_lepton_save_settings(void) { lepton_cci_save_settings(); }

void hal_lepton_get_stats(hal_lepton_stats_t *out) {
    lepton_vospi_get_stats(out);
}

const char *hal_lepton_state_name(uint8_t state) {
    return lepton_vospi_state_name(state);
}

int               hal_lepton_i2c_port(void) { return lep_wire_mutex ? lep_i2c_port : -1; }
SemaphoreHandle_t hal_lepton_wire_mutex(void) { return lep_wire_mutex; }
