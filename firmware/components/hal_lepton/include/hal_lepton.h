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

// ---------- Composable boot phases ----------
//
// hal_lepton_boot() is the all-in-one entry point. The phase functions
// below give finer control — needed by deep-sleep wake cycles where we
// want to skip the diagnostic dumps, skip FFC every wake, use bounded
// waits, and measure each phase. Cold-start order is exact:
//
//   power_on → cci_bus_init → cci_apply_config → vospi_bring_up
//   → wait_first_frame → run_initial_ffc (optional)
//
// hal_lepton_boot() composes these in sequence with the historical
// defaults (8s power-up wait, 30s first-frame wait). Each phase is
// idempotent within its layer.

// Drive the MOSFET LOW for 1s then HIGH, and wait `boot_wait_ms` for
// the Lepton firmware to come up (Lepton 3.5 needs ~5–8s before it
// will respond to I2C). Pass 0 to skip the wait.
esp_err_t hal_lepton_power_on(uint32_t boot_wait_ms);

// Drive the MOSFET LOW. Caller is responsible for first stopping the
// VoSPI reader (otherwise it'll keep retrying on dead lines). Used by
// power_manager / deep-sleep prep.
esp_err_t hal_lepton_power_off(void);

// I2C + SPI bus init + I2C address probe. Safe to call multiple times.
esp_err_t hal_lepton_cci_bus_init(void);

// Apply the full CCI configuration (radiometry, TLinear, AGC, VSYNC).
// Must follow cci_bus_init. Does NOT run FFC. Logs CCI state before
// and after for diagnostics; deep-sleep wake paths can call the
// underlying lepton_cci_configure() directly to skip the dumps if
// they want a quieter boot.
esp_err_t hal_lepton_cci_apply_config(void);

// Init VoSPI driver + spawn the reader task pinned to core 1.
esp_err_t hal_lepton_vospi_bring_up(void);

// Block until the first committed frame appears, or `timeout_ms`
// elapses. Polls at 250ms.
esp_err_t hal_lepton_wait_first_frame(uint32_t timeout_ms);

// Run a single FFC. Updates the cached lep_last_ffc_ms timestamp.
// Per Fox: do NOT call this until after the first frame is committed.
esp_err_t hal_lepton_run_initial_ffc(void);

// ---------- Frame access ----------

// Copy the latest committed frame into `dst` (must be at least
// LEP_FRAME_BYTES). Returns false if no frame has been committed yet.
bool hal_lepton_get_frame(uint16_t *dst);

uint32_t hal_lepton_frame_count(void);

// True iff at least one frame has been committed AND it landed within
// `max_age_ms` (judged against esp_timer_get_time/1000). Intended for
// callers that need to know "the sensor is currently streaming" not
// just "had a frame at some point" — frame_count > 0 is sticky once
// set, freshness is what actually distinguishes a healthy stream from
// one that stalled mid-session. Use ~1500–2000 ms (covers 2-3 frame
// intervals at the Lepton's native ~5 fps after our discard ratio).
bool hal_lepton_frame_fresh(uint32_t max_age_ms);

// Cooperative state for callers that need to know whether hal_lepton_boot()
// is currently running on another task. Set true on entry to
// hal_lepton_boot(), cleared on exit. The deep-sleep cmd-handler-spawned
// worker uses this to AVOID racing app_main's Lepton boot when the
// dashboard fires timelapse.start in the ~10 s window between
// net_relay_start (WS up) and hal_lepton_boot finishing.
bool hal_lepton_boot_in_progress(void);

// Block until hal_lepton_boot() either finishes or `timeout_ms` elapses.
// Returns ESP_OK if boot is no longer in progress when this returns,
// ESP_ERR_TIMEOUT otherwise. Polls every 100 ms.
esp_err_t hal_lepton_wait_boot_complete(uint32_t timeout_ms);

// ---------- CCI controls (thread-safe) ----------

esp_err_t hal_lepton_run_ffc(void);
esp_err_t hal_lepton_set_agc(bool enable);
esp_err_t hal_lepton_set_gain_mode(int mode);  // LEP_GAIN_HIGH/LOW/AUTO

bool hal_lepton_agc_enabled(void);
int  hal_lepton_gain_mode(void);

// Persist current AGC + gain to NVS (namespace "lepcfg").
void hal_lepton_save_settings(void);

// Read back the TLinear (radiometric) state captured by the last
// CCI configure. `resolution` is 0 for 0.1 K/count, 1 for 0.01 K/count
// (Lepton's RAD_TLINEAR_RESOLUTION encoding). Returns true if TLinear
// is currently active (raw frame pixels = K * scale).
bool lepton_cci_get_tlinear_state(bool *active, bool *auto_res,
                                   uint16_t *resolution);

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
// hal_lepton owns the I2C port + a wire-mutex (so CCI ops don't race
// other devices on the same physical bus). Other components on the
// same bus (e.g. hal_oled) borrow these via the accessors below.
//
// Uses the legacy driver/i2c.h API for IDF v5.3 compatibility — the
// esp32-camera SCCB on v5.3 forces the legacy driver, and the IDF
// won't link the new and old drivers in the same binary.
//
// Returns -1 / NULL if hal_lepton_boot() hasn't run.

#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

int               hal_lepton_i2c_port(void);
SemaphoreHandle_t hal_lepton_wire_mutex(void);

#ifdef __cplusplus
}
#endif
