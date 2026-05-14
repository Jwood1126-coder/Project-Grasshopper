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

// MOSFET pin config — applied once on the first power_on/power_off call.
static bool s_mosfet_configured = false;

static void mosfet_configure_once(void) {
    if (s_mosfet_configured || LEP_POWER_PIN < 0) return;
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << LEP_POWER_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    s_mosfet_configured = true;
}

esp_err_t hal_lepton_power_on(uint32_t boot_wait_ms) {
    if (LEP_POWER_PIN < 0) {
        // No MOSFET wired — board is hard-powered, just honour the wait.
        if (boot_wait_ms) vTaskDelay(pdMS_TO_TICKS(boot_wait_ms));
        return ESP_OK;
    }
    mosfet_configure_once();
    // Force a real off → on cycle. ESP32 resets don't clear the MOSFET
    // state, so the Lepton may be stuck in a bad mode (only emitting
    // 4-byte headers) from a previous boot. Drive LOW for 1 s so the
    // Lepton power supply fully drops.
    gpio_set_level(LEP_POWER_PIN, 0);
    ESP_LOGI(TAG, "MOSFET off (gpio %d) — power-cycle pause 1s", LEP_POWER_PIN);
    vTaskDelay(pdMS_TO_TICKS(1000));
    gpio_set_level(LEP_POWER_PIN, 1);
    ESP_LOGI(TAG, "MOSFET on");
    if (boot_wait_ms) {
        ESP_LOGI(TAG, "waiting %lums for Lepton boot...", (unsigned long)boot_wait_ms);
        vTaskDelay(pdMS_TO_TICKS(boot_wait_ms));
    }
    return ESP_OK;
}

esp_err_t hal_lepton_power_off(void) {
    if (LEP_POWER_PIN < 0) return ESP_OK;
    mosfet_configure_once();
    gpio_set_level(LEP_POWER_PIN, 0);
    ESP_LOGI(TAG, "MOSFET off (gpio %d)", LEP_POWER_PIN);
    return ESP_OK;
}

esp_err_t hal_lepton_cci_bus_init(void) {
    esp_err_t err = lepton_cci_init();
    if (err != ESP_OK) return err;
    // I2C address probe: confirm Lepton is on the bus before asking it
    // anything. If this NACKs, the bus is silent — no point running
    // CCI ops, the issue is physical (seating, wiring, power).
    if (!lepton_cci_probe_i2c()) {
        ESP_LOGE(TAG, "Lepton I2C probe FAILED — check seating, SDA/SCL/VIN, MOSFET");
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t hal_lepton_cci_apply_config(void) {
    lepton_cci_dump_state();
    esp_err_t err = lepton_cci_configure();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CCI configure failed");
        return err;
    }
    lepton_cci_dump_state();
    return ESP_OK;
}

esp_err_t hal_lepton_vospi_bring_up(void) {
    esp_err_t err = lepton_vospi_init();
    if (err != ESP_OK) return err;
    lepton_vospi_start();
    return ESP_OK;
}

esp_err_t hal_lepton_wait_first_frame(uint32_t timeout_ms) {
    uint32_t waited = 0;
    while (lepton_vospi_frame_count() == 0 && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(250));
        waited += 250;
    }
    if (lepton_vospi_frame_count() == 0) {
        ESP_LOGE(TAG, "no frame after %lu ms", (unsigned long)waited);
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "first frame received after %lu ms", (unsigned long)waited);
    return ESP_OK;
}

esp_err_t hal_lepton_run_initial_ffc(void) {
    esp_err_t err = lepton_cci_run_ffc();
    if (err == ESP_OK) {
        lep_last_ffc_ms = (uint32_t)(esp_timer_get_time() / 1000);
        ESP_LOGI(TAG, "initial FFC done");
    } else {
        ESP_LOGW(TAG, "initial FFC failed");
    }
    return err;
}

// Cooperative flag. The deep-sleep worker checks this to avoid racing
// app_main's hal_lepton_boot when a cmd arrives in the ~10 s window
// between net_relay_start and hal_lepton_boot finishing.
static volatile bool s_boot_in_progress = false;

bool hal_lepton_boot_in_progress(void) {
    return s_boot_in_progress;
}

esp_err_t hal_lepton_wait_boot_complete(uint32_t timeout_ms) {
    uint32_t waited = 0;
    while (s_boot_in_progress && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }
    return s_boot_in_progress ? ESP_ERR_TIMEOUT : ESP_OK;
}

esp_err_t hal_lepton_boot(void) {
    s_boot_in_progress = true;
    esp_err_t rc = ESP_FAIL;

    // Fox uses 5s here. New Lepton 3.5 units sometimes need longer
    // before responding to I2C; bump to 8s for safety.
    esp_err_t err = hal_lepton_power_on(8000);
    if (err != ESP_OK) { rc = err; goto out; }

    err = hal_lepton_cci_bus_init();
    if (err != ESP_OK) { rc = err; goto out; }

    err = hal_lepton_cci_apply_config();
    if (err != ESP_OK) { rc = err; goto out; }

    // NOTE: FFC at boot was an experiment that turned out to be
    // counterproductive. After FFC, the Lepton emits duplicate frames
    // (seg=0 on line 20) for several seconds while recalibrating,
    // which our VoSPI assembler treats as a sync failure and bails.
    // Fox runs FFC AFTER the first frame, and that's what we do too.

    err = hal_lepton_vospi_bring_up();
    if (err != ESP_OK) { rc = err; goto out; }

    err = hal_lepton_wait_first_frame(30000);
    if (err != ESP_OK) { rc = err; goto out; }

    hal_lepton_run_initial_ffc();   // non-fatal
    rc = ESP_OK;
out:
    s_boot_in_progress = false;
    return rc;
}

bool hal_lepton_get_frame(uint16_t *dst) {
    return lepton_vospi_get_frame(dst);
}

uint32_t hal_lepton_frame_count(void) {
    return lepton_vospi_frame_count();
}

bool hal_lepton_frame_fresh(uint32_t max_age_ms) {
    if (lepton_vospi_frame_count() == 0) return false;
    hal_lepton_stats_t st = {0};
    lepton_vospi_get_stats(&st);
    if (st.lastFrameMs == 0) return false;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    return (now - st.lastFrameMs) <= max_age_ms;
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
