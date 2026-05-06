#pragma once

// Internal shared state between lepton_cci.c, lepton_vospi.c, hal_lepton.c.

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "driver/i2c_master.h"

// Shared I2C bus + Lepton device handle (created in lepton_cci_init).
extern i2c_master_bus_handle_t lep_i2c_bus;
extern i2c_master_dev_handle_t lep_i2c_dev;

// Shared mutex for I2C bus. Same role as Fox's wire_mutex — once OLED
// lands in phase 4 it must take this same mutex to coexist on the bus.
extern SemaphoreHandle_t lep_wire_mutex;

// CCI subsystem.
esp_err_t lepton_cci_init(void);
esp_err_t lepton_cci_configure(void);
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
