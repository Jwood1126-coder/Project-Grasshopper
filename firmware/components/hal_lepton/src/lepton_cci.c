// Lepton 3.1R CCI (Camera Control Interface) over I2C.
// IDF v5.x port of Fox's lepton_cci.h. Logic is unchanged; APIs swapped:
//   Wire / Preferences  →  i2c_master_* / nvs_*
//   Serial.printf       →  ESP_LOG*
//
// Thread safety: every CCI op takes lep_wire_mutex. When OLED arrives in
// phase 4 it must take the same mutex.

#include "lepton_internal.h"
#include "board_pins.h"
#include "hal_lepton.h"

#include <string.h>

#include "esp_log.h"
#include "esp_rom_sys.h"  // esp_rom_delay_us
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "lep_cci";

// ---- Globals (declared extern in lepton_internal.h) ----
i2c_master_bus_handle_t lep_i2c_bus = NULL;
i2c_master_dev_handle_t lep_i2c_dev = NULL;
SemaphoreHandle_t lep_wire_mutex = NULL;

// ---- CCI register addresses ----
#define CCI_REG_STATUS    0x0002
#define CCI_REG_COMMAND   0x0004
#define CCI_REG_DATA_LEN  0x0006
#define CCI_REG_DATA_0    0x0008
#define CCI_REG_DATA_1    0x000A

#define CCI_STATUS_BUSY   0x0001

// ---- Command base addresses (SET = base | 0x0001) ----
#define CCI_CMD_AGC_ENABLE           0x0100
#define CCI_CMD_AGC_CALC_ENABLE      0x0148
#define CCI_CMD_SYS_RUN_FFC          0x0242
#define CCI_CMD_SYS_GAIN_MODE        0x0248
#define CCI_CMD_SYS_TELEMETRY_ENABLE 0x0218
#define CCI_CMD_OEM_POWER_DOWN       0x4800
#define CCI_CMD_OEM_GPIO_MODE        0x4854
#define CCI_CMD_RAD_ENABLE           0x4E10
#define CCI_CMD_RAD_TLINEAR_ENABLE   0x4EC0

// ---- Persistent state ----
static bool s_agc = false;
static int  s_gain = LEP_GAIN_AUTO;

// I2C byte timeout — Lepton clock-stretches up to ~3s on cold boot.
#define CCI_I2C_TIMEOUT_MS 3000

// ---- Low-level: register r/w with mutex ----

static esp_err_t cci_write_reg(uint16_t reg, uint16_t value) {
    uint8_t buf[4] = {
        (uint8_t)(reg >> 8),   (uint8_t)(reg & 0xFF),
        (uint8_t)(value >> 8), (uint8_t)(value & 0xFF),
    };
    xSemaphoreTake(lep_wire_mutex, portMAX_DELAY);
    esp_err_t err = i2c_master_transmit(lep_i2c_dev, buf, 4, CCI_I2C_TIMEOUT_MS);
    xSemaphoreGive(lep_wire_mutex);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "write_reg 0x%04x: %s", reg, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t cci_read_reg(uint16_t reg, uint16_t *value) {
    uint8_t reg_buf[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    uint8_t out[2] = {0, 0};
    xSemaphoreTake(lep_wire_mutex, portMAX_DELAY);
    // Lepton needs a small intra-transfer gap; transmit_receive issues a
    // RESTART between phases which the device tolerates.
    esp_err_t err = i2c_master_transmit_receive(
        lep_i2c_dev, reg_buf, 2, out, 2, CCI_I2C_TIMEOUT_MS);
    xSemaphoreGive(lep_wire_mutex);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read_reg 0x%04x: %s", reg, esp_err_to_name(err));
        return err;
    }
    *value = ((uint16_t)out[0] << 8) | out[1];
    return ESP_OK;
}

// ---- Wait for STATUS busy bit to clear ----

static bool cci_wait_idle(int timeout_ms) {
    uint16_t status = 0;
    while (timeout_ms > 0) {
        if (cci_read_reg(CCI_REG_STATUS, &status) != ESP_OK) {
            ESP_LOGE(TAG, "STATUS read failed (NACK?)");
            return false;
        }
        if (!(status & CCI_STATUS_BUSY)) return true;
        vTaskDelay(pdMS_TO_TICKS(1));
        timeout_ms--;
    }
    ESP_LOGW(TAG, "wait_idle timeout (status=0x%04x)", status);
    return false;
}

// ---- Higher-level command/attribute ----

static esp_err_t cci_run_command(uint16_t cmd) {
    if (!cci_wait_idle(1000)) return ESP_ERR_TIMEOUT;
    esp_err_t e = cci_write_reg(CCI_REG_COMMAND, cmd);
    if (e != ESP_OK) return e;
    return cci_wait_idle(5000) ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t cci_set_attribute(uint16_t cmd, uint32_t value) {
    if (!cci_wait_idle(1000)) return ESP_ERR_TIMEOUT;
    esp_err_t e;
    if ((e = cci_write_reg(CCI_REG_DATA_0, value & 0xFFFF)) != ESP_OK) return e;
    if ((e = cci_write_reg(CCI_REG_DATA_1, (value >> 16) & 0xFFFF)) != ESP_OK) return e;
    if ((e = cci_write_reg(CCI_REG_DATA_LEN, 2)) != ESP_OK) return e;
    if ((e = cci_write_reg(CCI_REG_COMMAND, cmd | 0x0001)) != ESP_OK) return e;
    return cci_wait_idle(5000) ? ESP_OK : ESP_ERR_TIMEOUT;
}

// ---- Init ----

esp_err_t lepton_cci_init(void) {
    if (lep_wire_mutex) return ESP_OK;

    lep_wire_mutex = xSemaphoreCreateMutex();
    if (!lep_wire_mutex) return ESP_ERR_NO_MEM;

    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = LEP_I2C_SCL,
        .sda_io_num = LEP_I2C_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &lep_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = LEP_I2C_ADDR,
        .scl_speed_hz = 100000,   // 100 kHz — Lepton's safe range
    };
    err = i2c_master_bus_add_device(lep_i2c_bus, &dev_cfg, &lep_i2c_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "CCI initialized (I2C0 sda=%d scl=%d, 0x%02x @100kHz)",
             LEP_I2C_SDA, LEP_I2C_SCL, LEP_I2C_ADDR);
    return ESP_OK;
}

// ---- Public CCI ops ----

esp_err_t lepton_cci_run_ffc(void) {
    ESP_LOGI(TAG, "run FFC");
    return cci_run_command(CCI_CMD_SYS_RUN_FFC);
}

static esp_err_t lep_enable_radiometry(void) {
    ESP_LOGI(TAG, "enable radiometry");
    return cci_set_attribute(CCI_CMD_RAD_ENABLE, 1);
}

static esp_err_t lep_enable_tlinear(void) {
    ESP_LOGI(TAG, "enable TLinear");
    return cci_set_attribute(CCI_CMD_RAD_TLINEAR_ENABLE, 1);
}

static esp_err_t lep_enable_agc_calc(void) {
    ESP_LOGI(TAG, "enable AGC calc");
    return cci_set_attribute(CCI_CMD_AGC_CALC_ENABLE, 1);
}

static esp_err_t lep_enable_vsync(void) {
    ESP_LOGI(TAG, "enable VSYNC GPIO");
    return cci_set_attribute(CCI_CMD_OEM_GPIO_MODE, 5);
}

esp_err_t lepton_cci_set_agc(bool enable) {
    esp_err_t e;
    if (enable) {
        if ((e = cci_set_attribute(CCI_CMD_RAD_TLINEAR_ENABLE, 0)) != ESP_OK) return e;
        vTaskDelay(pdMS_TO_TICKS(50));
        if ((e = cci_set_attribute(CCI_CMD_AGC_ENABLE, 1)) != ESP_OK) return e;
    } else {
        if ((e = cci_set_attribute(CCI_CMD_AGC_ENABLE, 0)) != ESP_OK) return e;
        vTaskDelay(pdMS_TO_TICKS(50));
        if ((e = cci_set_attribute(CCI_CMD_RAD_TLINEAR_ENABLE, 1)) != ESP_OK) return e;
    }
    s_agc = enable;
    ESP_LOGI(TAG, "AGC %s", enable ? "on" : "off");
    return ESP_OK;
}

esp_err_t lepton_cci_set_gain(int mode) {
    ESP_LOGI(TAG, "set gain mode %d", mode);
    esp_err_t e = cci_set_attribute(CCI_CMD_SYS_GAIN_MODE, (uint32_t)mode);
    if (e == ESP_OK) s_gain = mode;
    return e;
}

bool lepton_cci_agc_enabled(void) { return s_agc; }
int  lepton_cci_gain_mode(void)   { return s_gain; }

// ---- NVS persistence ----

void lepton_cci_save_settings(void) {
    nvs_handle_t h;
    if (nvs_open("lepcfg", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "agc",  s_agc ? 1 : 0);
    nvs_set_u8(h, "gain", (uint8_t)s_gain);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "settings saved: agc=%d gain=%d", s_agc, s_gain);
}

bool lepton_cci_load_settings(bool *agc, int *gain) {
    nvs_handle_t h;
    if (nvs_open("lepcfg", NVS_READONLY, &h) != ESP_OK) return false;
    bool found = false;
    uint8_t v;
    if (nvs_get_u8(h, "agc", &v) == ESP_OK) { *agc = v != 0; found = true; }
    if (nvs_get_u8(h, "gain", &v) == ESP_OK) {
        *gain = (v <= 2) ? v : LEP_GAIN_AUTO;
        found = true;
    }
    nvs_close(h);
    if (found) ESP_LOGI(TAG, "settings loaded: agc=%d gain=%d", *agc, *gain);
    return found;
}

// ---- Combined boot configure ----

esp_err_t lepton_cci_configure(void) {
    ESP_LOGI(TAG, "running full CCI configuration...");

    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "retry %d/3", attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        if (lep_enable_radiometry() != ESP_OK) continue;
        vTaskDelay(pdMS_TO_TICKS(200));

        // TLinear is 3.5-only; non-fatal on 3.1R.
        if (lep_enable_tlinear() != ESP_OK) {
            ESP_LOGW(TAG, "TLinear unavailable (Lepton 3.1R?) — continuing");
        }
        vTaskDelay(pdMS_TO_TICKS(200));

        lep_enable_agc_calc();
        vTaskDelay(pdMS_TO_TICKS(200));

        lep_enable_vsync();
        vTaskDelay(pdMS_TO_TICKS(200));

        // Restore persisted AGC + gain.
        bool a = false; int g = LEP_GAIN_AUTO;
        if (lepton_cci_load_settings(&a, &g)) {
            if (a) { lepton_cci_set_agc(true); vTaskDelay(pdMS_TO_TICKS(200)); }
            if (g != LEP_GAIN_AUTO) { lepton_cci_set_gain(g); vTaskDelay(pdMS_TO_TICKS(200)); }
            s_agc = a; s_gain = g;
        }

        ESP_LOGI(TAG, "configuration complete");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "configuration FAILED after 3 attempts");
    return ESP_ERR_INVALID_RESPONSE;
}
