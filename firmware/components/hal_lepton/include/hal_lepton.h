#pragma once

// Public API for the Lepton 3.1R driver — VoSPI reader + CCI control.
// Recycled from Scout Fox's lepton_vospi.h / lepton_cci.h.

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// CCI SYS Gain modes (passed to hal_lepton_set_gain_mode).
#define LEP_GAIN_HIGH 0
#define LEP_GAIN_LOW  1
#define LEP_GAIN_AUTO 2

// ---------- Boot sequence ----------
//
// Power on the Lepton MOSFET, init I2C+SPI, run CCI configuration with
// retries, start the VoSPI reader task on Core 1, wait for the first
// frame, run the initial FFC. Blocks for up to ~15s. Safe to call once
// from app_main.
//
// Returns:
//   ESP_OK                    — Lepton ready, frames flowing
//   ESP_ERR_TIMEOUT           — first frame did not arrive within 30s
//   ESP_ERR_INVALID_RESPONSE  — CCI configure failed after retries
//   ESP_FAIL                  — buffer allocation or driver init failed
esp_err_t hal_lepton_boot(void);

// ---------- Frame access ----------

// Copy the latest committed frame into `dst` (must be at least
// LEP_FRAME_BYTES). Returns false if no frame has been committed yet.
bool hal_lepton_get_frame(uint16_t *dst);

uint32_t hal_lepton_frame_count(void);

// ---------- CCI controls (thread-safe) ----------

esp_err_t hal_lepton_run_ffc(void);
esp_err_t hal_lepton_set_agc(bool enable);
esp_err_t hal_lepton_set_gain_mode(int mode);  // LEP_GAIN_HIGH/LOW/AUTO

bool hal_lepton_agc_enabled(void);
int  hal_lepton_gain_mode(void);

// Persist current AGC + gain to NVS (namespace "lepcfg").
void hal_lepton_save_settings(void);

// ---------- Diagnostics (for status / Tick) ----------

typedef struct {
    uint32_t frames;
    uint32_t totalPackets;
    uint32_t validPackets;
    uint32_t discardPackets;
    uint32_t syncEntries;
    uint32_t lineMismatch;
    uint32_t segNot1;
    uint32_t segMismatch;
    uint32_t frameTimeout;
    uint32_t segZero;
    uint32_t hardwareResets;
    uint32_t spliceDetected;
    uint32_t lastFrameMs;
    uint32_t lastFFCMs;
    uint8_t  state;            // 0=waiting 1=sync 2=reading 3=paused
    uint8_t  lastSegIds[4];
    bool     started;
    bool     ready;            // true after first frame committed
} hal_lepton_stats_t;

void hal_lepton_get_stats(hal_lepton_stats_t *out);
const char *hal_lepton_state_name(uint8_t state);

// ---------- Shared I2C bus access ----------
//
// hal_lepton owns the I2C master bus + a wire-mutex (so CCI ops don't
// race a future OLED driver on the same bus). Other components on the
// same bus (e.g. hal_oled) borrow these via the accessors below.
//
// Returns NULL if hal_lepton_boot() hasn't run or I2C init failed.

#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

i2c_master_bus_handle_t hal_lepton_i2c_bus(void);
SemaphoreHandle_t       hal_lepton_wire_mutex(void);

#ifdef __cplusplus
}
#endif
