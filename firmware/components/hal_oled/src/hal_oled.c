// SSD1306 driver + screen scheduler. See hal_oled.h.
//
// Layout: 128x64, horizontal addressing mode, 8 pages of 128 bytes.
// We render to a 1024-byte framebuffer in DRAM and flush via a single
// I2C write per page (under wire_mutex shared with Lepton CCI).

#include "hal_oled.h"
#include "board_pins.h"
#include "hal_lepton.h"
#include "oled_font.h"

#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "hal_oled";

#define FB_BYTES (OLED_W * OLED_H / 8)

static i2c_master_dev_handle_t s_dev = NULL;
static SemaphoreHandle_t       s_wire = NULL;
static uint8_t                 s_fb[FB_BYTES];

typedef enum {
    SCR_BOOT = 0,
    SCR_STATUS,
    SCR_ERROR,
} screen_t;

static screen_t s_screen = SCR_BOOT;

static char s_boot_line[32]   = "starting...";
static char s_err_code[16]    = "";
static char s_err_msg[40]     = "";

static hal_oled_status_t s_status = {0};
static SemaphoreHandle_t s_status_mutex = NULL;

// ---- SSD1306 init sequence ----
//
// Co=0, D/C#=0 → command-byte stream (0x00 prefix).
// One-shot setup is fine; the chip remembers all registers across writes.
static const uint8_t ssd1306_init[] = {
    0x00,                       // Co/DC: command
    0xAE,                       // display off
    0xD5, 0x80,                 // clock divide
    0xA8, 0x3F,                 // multiplex 64
    0xD3, 0x00,                 // display offset 0
    0x40,                       // start line 0
    0x8D, 0x14,                 // charge pump on
    0x20, 0x00,                 // memory mode horizontal
    0xA1,                       // segment remap (mirror columns)
    0xC8,                       // COM scan dec (mirror rows)
    0xDA, 0x12,                 // COM pins
    0x81, 0x7F,                 // contrast
    0xD9, 0xF1,                 // pre-charge
    0xDB, 0x40,                 // VCOMH deselect
    0xA4,                       // resume RAM contents
    0xA6,                       // normal (not inverse)
    0x2E,                       // deactivate scroll
    0xAF,                       // display on
};

static esp_err_t oled_send_cmd(const uint8_t *cmd_buf, size_t len) {
    if (xSemaphoreTake(s_wire, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = i2c_master_transmit(s_dev, cmd_buf, len,
                                         pdMS_TO_TICKS(200));
    xSemaphoreGive(s_wire);
    return err;
}

// Flush the framebuffer in 8 page writes. Each page = 1 control byte
// (0x40 = data) + 128 data bytes. We'd love one giant write, but the
// SSD1306 expects the 0x40 control byte at the start of each I2C
// transmission, so 8 small writes is the canonical approach.
static esp_err_t oled_flush(void) {
    if (!s_dev) return ESP_FAIL;
    if (xSemaphoreTake(s_wire, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    // Set column range 0..127, page range 0..7.
    static const uint8_t addr_setup[] = {
        0x00,
        0x21, 0x00, 0x7F,        // column 0..127
        0x22, 0x00, 0x07,        // page 0..7
    };
    esp_err_t err = i2c_master_transmit(s_dev, addr_setup, sizeof(addr_setup),
                                         pdMS_TO_TICKS(200));
    if (err != ESP_OK) goto out;

    uint8_t chunk[1 + 128];
    chunk[0] = 0x40;
    for (int p = 0; p < 8; p++) {
        memcpy(&chunk[1], &s_fb[p * 128], 128);
        err = i2c_master_transmit(s_dev, chunk, sizeof(chunk),
                                   pdMS_TO_TICKS(200));
        if (err != ESP_OK) break;
    }

out:
    xSemaphoreGive(s_wire);
    return err;
}

static void fb_clear(void) {
    memset(s_fb, 0, sizeof(s_fb));
}

// Pixel at (x,y), x∈[0,127] y∈[0,63].
static inline void fb_pixel(int x, int y, bool on) {
    if (x < 0 || x >= OLED_W || y < 0 || y >= OLED_H) return;
    int idx = (y / 8) * OLED_W + x;
    uint8_t mask = 1 << (y & 7);
    if (on) s_fb[idx] |= mask; else s_fb[idx] &= (uint8_t)~mask;
}

// Draw one 5x7 glyph at (x,y); returns advance.
static int fb_glyph(int x, int y, char c) {
    if (c < 0x20 || c > 0x7E) c = '?';
    const uint8_t *g = oled_font_5x7[c - 0x20];
    for (int col = 0; col < 5; col++) {
        for (int row = 0; row < 7; row++) {
            if (g[col] & (1 << row)) fb_pixel(x + col, y + row, true);
        }
    }
    return 6;
}

static void fb_text(int x, int y, const char *s) {
    while (*s) {
        x += fb_glyph(x, y, *s++);
        if (x > OLED_W - 6) break;
    }
}

static void fb_hline(int y) {
    for (int x = 0; x < OLED_W; x++) fb_pixel(x, y, true);
}

// ---- Screen rendering ----

static void render_boot(void) {
    fb_clear();
    fb_text(28, 8,  "GRASSHOPPER");
    fb_hline(20);
    fb_text(0, 28, s_boot_line);
}

static void render_status(void) {
    hal_oled_status_t st;
    if (s_status_mutex && xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        st = s_status;
        xSemaphoreGive(s_status_mutex);
    } else {
        memset(&st, 0, sizeof(st));
    }

    char line[24];

    fb_clear();

    // Row 0: device label + relay dot
    fb_text(0, 0, "GRASSHOPPER");
    if (st.relay_connected) fb_text(122, 0, ".");

    fb_hline(9);

    // Row 1: WiFi
    if (st.wifi_rssi == 0 || !st.wifi_ssid || !*st.wifi_ssid) {
        fb_text(0, 14, "WiFi: searching");
    } else {
        snprintf(line, sizeof(line), "%.10s %ddBm", st.wifi_ssid, st.wifi_rssi);
        fb_text(0, 14, line);
    }

    // Row 2: IP
    if (st.wifi_ip && *st.wifi_ip) {
        snprintf(line, sizeof(line), "IP %s", st.wifi_ip);
        fb_text(0, 24, line);
    }

    // Row 3: thermal + cam fps
    snprintf(line, sizeof(line), "T %lu  V %lu fps",
             (unsigned long)st.therm_fps, (unsigned long)st.cam_fps);
    fb_text(0, 36, line);

    // Row 4: heap + sd
    snprintf(line, sizeof(line), "heap %luK %s",
             (unsigned long)st.free_heap_kb,
             st.sd_present ? "SD" : "no-SD");
    fb_text(0, 46, line);

    // Row 5: fw version
    if (st.fw_version) {
        fb_text(0, 56, st.fw_version);
    }
}

static void render_error(void) {
    fb_clear();
    fb_text(0, 0, "ERROR");
    fb_hline(9);
    fb_text(0, 16, s_err_code);
    fb_text(0, 32, s_err_msg);
}

static void scheduler_task(void *arg) {
    (void)arg;
    while (1) {
        switch (s_screen) {
        case SCR_BOOT:   render_boot();   break;
        case SCR_STATUS: render_status(); break;
        case SCR_ERROR:  render_error();  break;
        }
        oled_flush();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

esp_err_t hal_oled_start(void) {
    if (s_dev) return ESP_OK;

    i2c_master_bus_handle_t bus = hal_lepton_i2c_bus();
    s_wire = hal_lepton_wire_mutex();
    if (!bus || !s_wire) {
        ESP_LOGE(TAG, "I2C bus not ready (call hal_lepton_boot first)");
        return ESP_FAIL;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = OLED_I2C_ADDR,
        .scl_speed_hz    = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device: %s", esp_err_to_name(err));
        return err;
    }

    s_status_mutex = xSemaphoreCreateMutex();

    err = oled_send_cmd(ssd1306_init, sizeof(ssd1306_init));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ssd1306 init: %s — display absent?", esp_err_to_name(err));
        // Keep s_dev around so future writes are no-ops via the same path.
        return err;
    }
    ESP_LOGI(TAG, "ssd1306 ready @ 0x%02x, %dx%d", OLED_I2C_ADDR, OLED_W, OLED_H);

    // Render boot screen synchronously so the user sees something
    // before the scheduler kicks in.
    render_boot();
    oled_flush();

    BaseType_t ok = xTaskCreate(scheduler_task, "oled", 3072, NULL, 4, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "scheduler task create failed");
        return ESP_FAIL;
    }
    s_screen = SCR_STATUS;
    return ESP_OK;
}

void hal_oled_set_status(const hal_oled_status_t *st) {
    if (!st || !s_status_mutex) return;
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_status = *st;
        xSemaphoreGive(s_status_mutex);
    }
}

void hal_oled_show_boot(const char *line) {
    if (line) {
        strncpy(s_boot_line, line, sizeof(s_boot_line) - 1);
        s_boot_line[sizeof(s_boot_line) - 1] = 0;
    }
    s_screen = SCR_BOOT;
}

void hal_oled_show_error(const char *code, const char *msg) {
    if (code) {
        strncpy(s_err_code, code, sizeof(s_err_code) - 1);
        s_err_code[sizeof(s_err_code) - 1] = 0;
    }
    if (msg) {
        strncpy(s_err_msg, msg, sizeof(s_err_msg) - 1);
        s_err_msg[sizeof(s_err_msg) - 1] = 0;
    }
    s_screen = SCR_ERROR;
}
