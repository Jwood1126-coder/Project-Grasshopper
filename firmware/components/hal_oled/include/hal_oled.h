#pragma once

// SSD1306 128x64 OLED driver + screen scheduler.
//
// Shares the I2C bus + wire-mutex with hal_lepton's CCI (board has one
// I2C bus serving Lepton @ 0x2A and OLED @ 0x3C). hal_lepton must be
// initialized first so the bus exists.

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bring up the panel and start the background screen-scheduler task.
// Returns ESP_FAIL if hal_lepton hasn't initialized I2C yet.
esp_err_t hal_oled_start(void);

// Live status the scheduler should display.
typedef struct {
    const char *fw_version;
    const char *wifi_ssid;
    const char *wifi_ip;
    int         wifi_rssi;       // dBm; 0 means "not connected"
    bool        relay_connected;
    uint32_t    therm_fps;
    uint32_t    cam_fps;
    uint32_t    free_heap_kb;
    bool        sd_present;
} hal_oled_status_t;

// Push a status snapshot. Cheap; just copies into an internal struct
// the scheduler reads.
void hal_oled_set_status(const hal_oled_status_t *st);

// Force the scheduler to draw the boot screen / error screen.
void hal_oled_show_boot(const char *line);
void hal_oled_show_error(const char *code, const char *msg);

#ifdef __cplusplus
}
#endif
