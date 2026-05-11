#pragma once

// Internal shared state between lepton_cci.cpp, lepton_vospi.c, hal_lepton.c.
// CCI is C++ (uses Arduino Wire); VoSPI + glue are C. Public symbols are
// declared `extern "C"` so they're callable from both sides.

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "driver/i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

// I2C port the CCI bus runs on. CCI uses Arduino Wire (which sits on
// the legacy IDF driver under the hood); hal_oled also installs on
// this same port via the legacy API. esp32-camera's SCCB lives on
// I2C_NUM_1 per CONFIG_SCCB_HARDWARE_I2C_PORT1.
extern i2c_port_t lep_i2c_port;

// Shared mutex for the I2C bus. CCI takes it for every Wire op;
// hal_oled also takes it for its writes.
extern SemaphoreHandle_t lep_wire_mutex;

// CCI subsystem.
esp_err_t lepton_cci_init(void);
esp_err_t lepton_cci_oem_reboot(void);
esp_err_t lepton_cci_configure(void);
void      lepton_cci_dump_state(void);
esp_err_t lepton_cci_run_ffc(void);
esp_err_t lepton_cci_set_agc(bool enable);
esp_err_t lepton_cci_set_gain(int mode);
bool      lepton_cci_agc_enabled(void);
int       lepton_cci_gain_mode(void);
void      lepton_cci_save_settings(void);
bool      lepton_cci_load_settings(bool *agc, int *gain);

// VoSPI subsystem.
esp_err_t lepton_vospi_init(void);
void      lepton_vospi_start(void);
bool      lepton_vospi_get_frame(uint16_t *dst);
uint32_t  lepton_vospi_frame_count(void);

#include "hal_lepton.h"
void      lepton_vospi_get_stats(hal_lepton_stats_t *out);
const char *lepton_vospi_state_name(uint8_t state);

// Set by hal_lepton.c when FFC runs so stats can compute lastFFCMs.
extern volatile uint32_t lep_last_ffc_ms;

#ifdef __cplusplus
}
#endif
