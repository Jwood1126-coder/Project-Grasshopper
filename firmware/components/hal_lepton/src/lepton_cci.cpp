// Lepton 3.1R CCI (Camera Control Interface) over I2C.
//
// Direct port of Scout Fox's lepton_cci.h (which streams at 8.7 fps in
// the field). Uses Arduino's Wire library on top of arduino-esp32,
// which is already linked in for the SPI HAL. Wire is built on the
// IDF v5.3 LEGACY i2c driver — same driver esp32-camera's SCCB uses,
// just on a different I2C port — so there's no driver_ng conflict.
//
// Why this instead of IDF-native i2c_master_*: in field testing the
// IDF wrappers (both new and legacy) failed to bring the Lepton out
// of "discard packet" mode. Same wires, same Lepton, same Arduino SPI
// HAL — only the I2C plumbing differs. Wire's clock-stretching
// timeout, transaction timing, or both, match what the Lepton expects
// in a way the IDF driver doesn't on this hardware. Architecture
// doc §5 explicitly calls out Fox's CCI as "lift wholesale".
//
// Thread safety: every CCI op takes lep_wire_mutex.

#include "lepton_internal.h"
#include "board_pins.h"
#include "hal_lepton.h"

#include <Arduino.h>
#include <Wire.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "lep_cci";

// Wire instance — we use the global Wire (TwoWire on I2C_NUM_0).
// esp32-camera's SCCB defaults to I2C_NUM_1 (CONFIG_SCCB_HARDWARE_I2C_PORT1),
// so the two coexist on different ports under the same legacy driver.
extern "C" {

i2c_port_t lep_i2c_port = I2C_NUM_0;
SemaphoreHandle_t lep_wire_mutex = NULL;

}

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
#define CCI_CMD_OEM_REBOOT           0x4842   // soft-reboot the Lepton
#define CCI_CMD_RAD_ENABLE           0x4E10
#define CCI_CMD_RAD_TLINEAR_ENABLE   0x4EC0

// ---- Persistent state ----
static bool s_agc = false;
static int  s_gain = LEP_GAIN_AUTO;

// ---- Low-level: register r/w with mutex ----
//
// Mirrors Fox's cci_write_reg / cci_read_reg exactly. Notable details
// preserved from Fox:
//   - 50 µs gap between the register-address write (with explicit STOP)
//     and the data read (Lepton FW needs this gap to latch).
//   - Wire.endTransmission(true) for an explicit STOP between phases.
//   - Wire.setTimeOut(3000) configured in init() — Lepton clock-
//     stretches up to ~3 s on cold boot during writes.

static bool cci_write_reg(uint16_t reg, uint16_t value) {
    xSemaphoreTake(lep_wire_mutex, portMAX_DELAY);
    Wire.beginTransmission(LEP_I2C_ADDR);
    Wire.write((reg >> 8) & 0xFF);
    Wire.write(reg & 0xFF);
    Wire.write((value >> 8) & 0xFF);
    Wire.write(value & 0xFF);
    uint8_t err = Wire.endTransmission();
    xSemaphoreGive(lep_wire_mutex);
    if (err != 0) {
        ESP_LOGW(TAG, "write_reg 0x%04x: err=%d", reg, err);
        return false;
    }
    return true;
}

static bool cci_read_reg(uint16_t reg, uint16_t *value) {
    xSemaphoreTake(lep_wire_mutex, portMAX_DELAY);

    Wire.beginTransmission(LEP_I2C_ADDR);
    Wire.write((reg >> 8) & 0xFF);
    Wire.write(reg & 0xFF);
    uint8_t err = Wire.endTransmission(true);   // full STOP
    if (err != 0) {
        xSemaphoreGive(lep_wire_mutex);
        ESP_LOGW(TAG, "read_reg 0x%04x: write err=%d", reg, err);
        return false;
    }

    delayMicroseconds(50);

    uint8_t n = Wire.requestFrom((uint8_t)LEP_I2C_ADDR, (uint8_t)2);
    if (n != 2) {
        xSemaphoreGive(lep_wire_mutex);
        ESP_LOGW(TAG, "read_reg 0x%04x: got %d bytes", reg, n);
        return false;
    }
    uint8_t hi = Wire.read();
    uint8_t lo = Wire.read();
    *value = ((uint16_t)hi << 8) | lo;

    xSemaphoreGive(lep_wire_mutex);
    return true;
}

// ---- Wait for STATUS busy bit to clear ----

static bool cci_wait_idle(int timeout_ms) {
    uint16_t status = 0;
    while (timeout_ms > 0) {
        if (!cci_read_reg(CCI_REG_STATUS, &status)) {
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

static bool cci_run_command(uint16_t cmd) {
    if (!cci_wait_idle(1000)) return false;
    if (!cci_write_reg(CCI_REG_COMMAND, cmd)) return false;
    return cci_wait_idle(5000);
}

static bool cci_set_attribute(uint16_t cmd, uint32_t value) {
    if (!cci_wait_idle(1000)) return false;
    if (!cci_write_reg(CCI_REG_DATA_0, value & 0xFFFF)) return false;
    if (!cci_write_reg(CCI_REG_DATA_1, (value >> 16) & 0xFFFF)) return false;
    if (!cci_write_reg(CCI_REG_DATA_LEN, 2)) return false;
    if (!cci_write_reg(CCI_REG_COMMAND, cmd | 0x0001)) return false;
    return cci_wait_idle(5000);
}

// ---- Single-attribute helpers (Fox's lep_enable_*) ----

static bool lep_enable_radiometry(void) {
    ESP_LOGI(TAG, "enable radiometry");
    return cci_set_attribute(CCI_CMD_RAD_ENABLE, 1);
}

static bool lep_enable_tlinear(void) {
    ESP_LOGI(TAG, "enable TLinear");
    return cci_set_attribute(CCI_CMD_RAD_TLINEAR_ENABLE, 1);
}

static bool lep_enable_agc_calc(void) {
    ESP_LOGI(TAG, "enable AGC calc");
    return cci_set_attribute(CCI_CMD_AGC_CALC_ENABLE, 1);
}

static bool lep_enable_vsync(void) {
    ESP_LOGI(TAG, "enable VSYNC GPIO");
    return cci_set_attribute(CCI_CMD_OEM_GPIO_MODE, 5);
}

// ---- C-callable public API ----

extern "C" {

esp_err_t lepton_cci_init(void) {
    if (lep_wire_mutex) return ESP_OK;

    lep_wire_mutex = xSemaphoreCreateMutex();
    if (!lep_wire_mutex) return ESP_ERR_NO_MEM;

    Wire.begin(LEP_I2C_SDA, LEP_I2C_SCL);
    Wire.setClock(100000);          // Lepton's safe range
    Wire.setTimeOut(3000);          // Lepton clock-stretches up to ~3 s

    ESP_LOGI(TAG, "CCI initialized via Arduino Wire (sda=%d scl=%d, 0x%02x @100kHz)",
             LEP_I2C_SDA, LEP_I2C_SCL, LEP_I2C_ADDR);

    return ESP_OK;
}

esp_err_t lepton_cci_oem_reboot(void) {
    ESP_LOGW(TAG, "issuing Lepton OEM_REBOOT (soft reset via CCI)");
    if (!cci_run_command(CCI_CMD_OEM_REBOOT)) {
        ESP_LOGW(TAG, "OEM_REBOOT cmd failed");
        return ESP_FAIL;
    }
    // Lepton needs ~5 s to come back from a soft reboot.
    vTaskDelay(pdMS_TO_TICKS(5000));
    ESP_LOGI(TAG, "OEM_REBOOT done");
    return ESP_OK;
}

esp_err_t lepton_cci_configure(void) {
    ESP_LOGI(TAG, "running full CCI configuration...");

    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "configure retry %d", attempt);
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        bool ok = lep_enable_radiometry()
               && lep_enable_tlinear()
               && lep_enable_agc_calc()
               && lep_enable_vsync();

        if (!ok) continue;

        // Apply persisted settings if any.
        bool a = false;
        int  g = LEP_GAIN_AUTO;
        if (lepton_cci_load_settings(&a, &g)) {
            if (a) {
                lepton_cci_set_agc(true);
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            if (g != LEP_GAIN_AUTO) {
                lepton_cci_set_gain(g);
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            s_agc = a;
            s_gain = g;
        }

        ESP_LOGI(TAG, "configuration complete");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "configuration FAILED after 3 attempts");
    return ESP_ERR_INVALID_RESPONSE;
}

esp_err_t lepton_cci_run_ffc(void) {
    ESP_LOGI(TAG, "run FFC");
    return cci_run_command(CCI_CMD_SYS_RUN_FFC) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t lepton_cci_set_agc(bool enable) {
    bool ok;
    if (enable) {
        ok = cci_set_attribute(CCI_CMD_RAD_TLINEAR_ENABLE, 0);
        vTaskDelay(pdMS_TO_TICKS(50));
        if (ok) ok = cci_set_attribute(CCI_CMD_AGC_ENABLE, 1);
    } else {
        ok = cci_set_attribute(CCI_CMD_AGC_ENABLE, 0);
        vTaskDelay(pdMS_TO_TICKS(50));
        if (ok) ok = cci_set_attribute(CCI_CMD_RAD_TLINEAR_ENABLE, 1);
    }
    if (!ok) return ESP_FAIL;
    s_agc = enable;
    ESP_LOGI(TAG, "AGC %s", enable ? "on" : "off");
    return ESP_OK;
}

esp_err_t lepton_cci_set_gain(int mode) {
    ESP_LOGI(TAG, "set gain mode %d", mode);
    if (!cci_set_attribute(CCI_CMD_SYS_GAIN_MODE, (uint32_t)mode)) return ESP_FAIL;
    s_gain = mode;
    return ESP_OK;
}

bool lepton_cci_agc_enabled(void) { return s_agc; }
int  lepton_cci_gain_mode(void)   { return s_gain; }

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

}  // extern "C"
