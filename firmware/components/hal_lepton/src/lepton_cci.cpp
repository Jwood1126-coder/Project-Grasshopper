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
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include <string.h>

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
// TLinear resolution control. Defined for future testing — NOT enabled
// in the configure path. tCam-Mini's reference always pairs TLinear=1
// with AUTO_RES=1 (memory:lepton_breakout_damaged.md). Until the VoSPI
// link is solid, we keep TLinear=0 and don't touch these.
#define CCI_CMD_RAD_TLINEAR_RESOLUTION       0x4EC4   // 0=0.1K, 1=0.01K
#define CCI_CMD_RAD_TLINEAR_AUTO_RESOLUTION  0x4EC8   // 0=fixed, 1=auto

// ---- Persistent state ----
static bool s_agc = false;
static int  s_gain = LEP_GAIN_AUTO;
// Intended TLinear state, set by lep_enable_tlinear() during configure.
// Used by lepton_cci_set_agc() so AGC toggles restore the configured
// TLinear state rather than unconditionally re-enabling it (which would
// break the current TLinear=0 workaround). See memory:lepton_breakout_damaged.md.
static bool s_tlinear_intended = false;

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

// ---- Wait for STATUS busy bit to clear, then surface response code ----
//
// STATUS register layout (FLIR IDD §4.2.2):
//   bit 0     : BUSY
//   bit 1     : BOOT_MODE
//   bit 2     : BOOT_STATUS
//   bits 8-15 : signed int8 response code from last command (0 = OK,
//               negative = LEP_RESULT error). Treating "BUSY cleared"
//               as success without checking this byte is exactly how
//               the cycle-storm-era FFCs reported "OK in 5 ms" — they
//               were rejected, not executed.
//
// Returns true iff BUSY cleared AND response code is 0. On error,
// `*resp_out` (if non-null) gets the signed response code so callers
// can log it. Timeouts are reported as resp = -127 (synthetic).
static bool cci_wait_idle_status(int timeout_ms, int8_t *resp_out) {
    uint16_t status = 0;
    while (timeout_ms > 0) {
        if (!cci_read_reg(CCI_REG_STATUS, &status)) {
            ESP_LOGE(TAG, "STATUS read failed (NACK?)");
            if (resp_out) *resp_out = -127;
            return false;
        }
        if (!(status & CCI_STATUS_BUSY)) {
            int8_t resp = (int8_t)((status >> 8) & 0xFF);
            if (resp_out) *resp_out = resp;
            if (resp != 0) {
                ESP_LOGW(TAG, "command failed: status=0x%04x resp=%d", status, resp);
                return false;
            }
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
        timeout_ms--;
    }
    ESP_LOGW(TAG, "wait_idle timeout (status=0x%04x)", status);
    if (resp_out) *resp_out = -127;
    return false;
}

static bool cci_wait_idle(int timeout_ms) {
    return cci_wait_idle_status(timeout_ms, NULL);
}

// ---- Higher-level command/attribute ----

static bool cci_run_command(uint16_t cmd) {
    if (!cci_wait_idle(1000)) return false;
    if (!cci_write_reg(CCI_REG_COMMAND, cmd)) return false;
    int8_t resp = 0;
    bool ok = cci_wait_idle_status(5000, &resp);
    ESP_LOGI(TAG, "RUN  0x%04x -> %s (resp=%d)", cmd, ok ? "OK" : "FAIL", resp);
    return ok;
}

static bool cci_set_attribute(uint16_t cmd, uint32_t value) {
    if (!cci_wait_idle(1000)) return false;
    if (!cci_write_reg(CCI_REG_DATA_0, value & 0xFFFF)) return false;
    if (!cci_write_reg(CCI_REG_DATA_1, (value >> 16) & 0xFFFF)) return false;
    if (!cci_write_reg(CCI_REG_DATA_LEN, 2)) return false;
    if (!cci_write_reg(CCI_REG_COMMAND, cmd | 0x0001)) return false;
    int8_t resp = 0;
    bool ok = cci_wait_idle_status(5000, &resp);
    ESP_LOGI(TAG, "SET  0x%04x = 0x%08lx -> %s (resp=%d)",
             cmd, (unsigned long)value, ok ? "OK" : "FAIL", resp);
    return ok;
}

// GET protocol — corrected per FLIR IDD and verified empirically on
// Lepton 3.1R:
//   1. wait idle
//   2. write DATA_LEN = expected_words (Lepton accepts this as a hint
//      for SET; harmless on GET)
//   3. write COMMAND with bottom 2 bits = 00 (GET form)
//   4. wait idle, READ AND CHECK the response code in STATUS
//   5. read DATA_0..DATA_(N-1) — N = expected_words
//
// IMPORTANT: we do NOT validate the post-GET DATA_LEN value. Empirically,
// this Lepton firmware (3.1R) reports DATA_LEN in BYTES on the GET
// response (so it's always 2× expected_words) while accepting DATA_LEN
// in WORDS on SET. Trying to validate it as words causes every GET to
// be wrongly flagged as failed — exactly the behavior that drove the
// "OEM ROM damaged" hypothesis. resp=0 from STATUS is the authoritative
// success signal.
//
// `words_out` must have capacity for `expected_words`. Returns the
// signed response code (0 = OK), or -127 on transport failure.
static int cci_get_attribute_n(uint16_t cmd, uint16_t expected_words,
                               uint16_t *words_out) {
    if (!cci_wait_idle(1000)) return -127;
    if (!cci_write_reg(CCI_REG_DATA_LEN, expected_words)) return -127;
    if (!cci_write_reg(CCI_REG_COMMAND, cmd & ~0x0003u)) return -127;

    int8_t resp = 0;
    bool ok = cci_wait_idle_status(5000, &resp);
    if (!ok) {
        ESP_LOGW(TAG, "GET  0x%04x (want %u w) -> resp=%d", cmd, expected_words, resp);
        return resp;
    }

    // Optional: read DATA_LEN for diagnostic logging only — not a gate.
    uint16_t reported_len = 0;
    cci_read_reg(CCI_REG_DATA_LEN, &reported_len);

    for (int i = 0; i < expected_words; i++) {
        uint16_t v = 0;
        if (!cci_read_reg(CCI_REG_DATA_0 + i * 2, &v)) return -127;
        words_out[i] = v;
    }
    ESP_LOGI(TAG, "GET  0x%04x (%u w, lepton-reported len=%u) -> OK",
             cmd, expected_words, reported_len);
    return 0;
}

// ---- Single-attribute helpers (Fox's lep_enable_*) ----

static bool lep_enable_radiometry(void) {
    ESP_LOGI(TAG, "enable radiometry");
    return cci_set_attribute(CCI_CMD_RAD_ENABLE, 1);
}

// Cached tlinear state (read back via GET right after enabling).
// Zero values mean "unknown / not radiometric" — used by capture.c
// to decide whether to publish temp readings.
static volatile bool     s_tlinear_active        = false;
static volatile bool     s_tlinear_auto_res      = false;
static volatile uint16_t s_tlinear_resolution    = 0;  // 0 = 0.1K, 1 = 0.01K

static bool lep_enable_tlinear(void) {
    // VoSPI link is now clean, so retire the old TLinear=0 workaround.
    // Codex-recommended order: AUTO_RESOLUTION first (so the Lepton
    // chooses scale per frame), then ENABLE TLinear. Verify both with
    // GET and capture the resulting resolution so the host knows the
    // raw→Kelvin scale factor (0.01K or 0.1K per count).
    s_tlinear_intended = true;
    bool ok_auto    = cci_set_attribute(CCI_CMD_RAD_TLINEAR_AUTO_RESOLUTION, 1);
    bool ok_enable  = cci_set_attribute(CCI_CMD_RAD_TLINEAR_ENABLE, 1);

    uint16_t v_auto = 0, v_enable = 0, v_res = 0;
    int ra = cci_get_attribute_n(CCI_CMD_RAD_TLINEAR_AUTO_RESOLUTION, 1, &v_auto);
    int re = cci_get_attribute_n(CCI_CMD_RAD_TLINEAR_ENABLE,          1, &v_enable);
    int rr = cci_get_attribute_n(CCI_CMD_RAD_TLINEAR_RESOLUTION,      1, &v_res);

    s_tlinear_active     = (re == 0 && v_enable == 1);
    s_tlinear_auto_res   = (ra == 0 && v_auto == 1);
    s_tlinear_resolution = (rr == 0) ? v_res : 0;

    ESP_LOGI(TAG, "TLinear set: enable=%d auto_res=%d res=%u (verified %d/%d/%d)",
             (int)s_tlinear_active, (int)s_tlinear_auto_res,
             (unsigned)s_tlinear_resolution, re, ra, rr);
    return ok_auto && ok_enable;
}

extern "C" bool lepton_cci_get_tlinear_state(bool *active,
                                              bool *auto_res,
                                              uint16_t *resolution) {
    if (active)     *active     = s_tlinear_active;
    if (auto_res)   *auto_res   = s_tlinear_auto_res;
    if (resolution) *resolution = s_tlinear_resolution;
    return s_tlinear_active;
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

// Probe the I2C bus for the Lepton at LEP_I2C_ADDR. Issues a zero-byte
// write transaction; success = ACK = Lepton present. Failure usually
// means physical issue (seating, wiring, power), not protocol.
bool lepton_cci_probe_i2c(void) {
    xSemaphoreTake(lep_wire_mutex, portMAX_DELAY);
    Wire.beginTransmission(LEP_I2C_ADDR);
    uint8_t err = Wire.endTransmission();
    xSemaphoreGive(lep_wire_mutex);
    if (err == 0) {
        ESP_LOGI(TAG, "I2C probe at 0x%02x: ACK (Lepton present)", LEP_I2C_ADDR);
        return true;
    }
    ESP_LOGW(TAG, "I2C probe at 0x%02x: err=%d (NACK — Lepton not responding)",
             LEP_I2C_ADDR, err);
    // Scan the rest of the bus to see if ANYTHING is there.
    int found = 0;
    for (uint8_t a = 1; a < 127; a++) {
        if (a == LEP_I2C_ADDR) continue;
        xSemaphoreTake(lep_wire_mutex, portMAX_DELAY);
        Wire.beginTransmission(a);
        uint8_t e = Wire.endTransmission();
        xSemaphoreGive(lep_wire_mutex);
        if (e == 0) {
            ESP_LOGW(TAG, "  bus scan: device at 0x%02x", a);
            found++;
        }
    }
    if (found == 0) {
        ESP_LOGW(TAG, "  bus scan: no devices found at all — bus is dead "
                      "(check pull-ups, SDA/SCL wiring)");
    }
    return false;
}

esp_err_t lepton_cci_oem_reboot(void) {
    ESP_LOGW(TAG, "issuing Lepton OEM_REBOOT (soft reset via CCI)");
    if (!cci_wait_idle(1000) || !cci_write_reg(CCI_REG_COMMAND, CCI_CMD_OEM_REBOOT)) {
        ESP_LOGW(TAG, "OEM_REBOOT command write failed; waiting anyway in case reboot started");
        vTaskDelay(pdMS_TO_TICKS(5000));
        return ESP_FAIL;
    }

    // OEM_REBOOT can make the Lepton disappear before STATUS can be read.
    // Treat a successful COMMAND write as the acknowledgement and always
    // allow the reboot window before issuing more CCI commands.
    vTaskDelay(pdMS_TO_TICKS(5000));
    ESP_LOGI(TAG, "OEM_REBOOT done");
    return ESP_OK;
}

// ---- Diagnostic command IDs and known data lengths ----
//
// Per FLIR Lepton Software IDD. Module bases: SYS=0x0200, AGC=0x0100,
// RAD=0x4E00, OEM=0x4800. Lengths are in 16-bit WORDS (= bytes/2).
//
// Critical correction from prior code: 0x480C is OEM_LOW_POWER_MODE_2,
// NOT firmware version. OEM_SOFTWARE_VERSION is 0x4820 (4 words / 8 B).
// The "OEM_FW_VER returns DATA_LEN=0" anomaly that drove the OEM-damage
// hypothesis was a wrong-command-ID + missing-DATA_LEN-preload bug.
#define CCI_CMD_SYS_STATUS           0x0204   // 4 words
#define CCI_CMD_SYS_FFC_STATUS       0x0244   // 2 words (uint32 enum)
#define CCI_CMD_OEM_PART_NUMBER      0x481C   // 16 words / 32 bytes
#define CCI_CMD_OEM_SOFTWARE_VERSION 0x4820   // 4 words / 8 bytes
#define CCI_CMD_OEM_VIDEO_OUTPUT_EN  0x4824   // 2 words (uint32 enum)
#define CCI_CMD_OEM_CAL_STATUS       0x4848   // 2 words

// Read-only diagnostic dump: print every CCI value we care about so
// we can compare "what the Lepton thinks its state is" against
// "what we tried to set". If our SET commands aren't actually taking
// effect, this surfaces the gap.
//
// The OEM-module probes (PART_NUMBER, SOFTWARE_VERSION, CAL_STATUS,
// VIDEO_OUTPUT_ENABLE) are the discriminator for OEM-damage vs
// software-recoverable. Damaged OEM ROM/RAM presents as either
// transport failure, non-zero response codes, or empty strings on
// the identity reads while VIDEO_OUTPUT_ENABLE (a settings register)
// still works.
void lepton_cci_dump_state(void) {
    uint16_t buf[16];
    int rc;

    ESP_LOGI(TAG, "=== Lepton CCI state dump ===");

    // STATUS register (direct read, not a GET).
    uint16_t status = 0;
    if (cci_read_reg(CCI_REG_STATUS, &status)) {
        int8_t resp = (int8_t)((status >> 8) & 0xFF);
        ESP_LOGI(TAG, "  STATUS              = 0x%04x  busy=%d boot_mode=%d boot_status=%d resp=%d",
                 status, status & 1, (status >> 1) & 1, (status >> 2) & 1, resp);
    } else {
        ESP_LOGW(TAG, "  STATUS              read failed");
    }

    // SYS_STATUS (4 words).
    rc = cci_get_attribute_n(CCI_CMD_SYS_STATUS, 4, buf);
    if (rc == 0) {
        ESP_LOGI(TAG, "  SYS_STATUS          camStatus=%u commandCount=%u  (raw: %04x %04x %04x %04x)",
                 buf[0], buf[1], buf[0], buf[1], buf[2], buf[3]);
    } else {
        ESP_LOGW(TAG, "  SYS_STATUS          GET FAILED (rc=%d)", rc);
    }

    // OEM_SOFTWARE_VERSION (4 words / 8 bytes: gpp_major, gpp_minor, gpp_build, dsp_major, dsp_minor, dsp_build, _, _).
    rc = cci_get_attribute_n(CCI_CMD_OEM_SOFTWARE_VERSION, 4, buf);
    if (rc == 0) {
        // Words are big-endian-packed pairs of bytes per IDD.
        uint8_t *b = (uint8_t *)buf;
        ESP_LOGI(TAG, "  OEM_SOFTWARE_VER    gpp=%u.%u.%u dsp=%u.%u.%u  (raw: %04x %04x %04x %04x)",
                 b[1], b[0], b[3], b[5], b[4], b[7],
                 buf[0], buf[1], buf[2], buf[3]);
    } else {
        ESP_LOGW(TAG, "  OEM_SOFTWARE_VER    GET FAILED (rc=%d)  ← OEM ROM may be damaged", rc);
    }

    // OEM_PART_NUMBER (16 words / 32 bytes ASCII).
    rc = cci_get_attribute_n(CCI_CMD_OEM_PART_NUMBER, 16, buf);
    if (rc == 0) {
        char part[33] = {0};
        memcpy(part, buf, 32);
        // Sanitize for log printing.
        for (int i = 0; i < 32; i++) {
            if (part[i] != 0 && (part[i] < 0x20 || part[i] > 0x7E)) part[i] = '?';
        }
        ESP_LOGI(TAG, "  OEM_PART_NUMBER     \"%s\"", part);
    } else {
        ESP_LOGW(TAG, "  OEM_PART_NUMBER     GET FAILED (rc=%d)  ← OEM ROM may be damaged", rc);
    }

    // OEM_VIDEO_OUTPUT_ENABLE (2 words). 0 = disabled (no pixels), 1 = enabled.
    rc = cci_get_attribute_n(CCI_CMD_OEM_VIDEO_OUTPUT_EN, 2, buf);
    if (rc == 0) {
        uint32_t v = (uint32_t)buf[0] | ((uint32_t)buf[1] << 16);
        ESP_LOGI(TAG, "  OEM_VIDEO_OUT_EN    = %lu  %s", (unsigned long)v,
                 v == 0 ? "← DISABLED, this would explain no pixels!" : "(enabled)");
    } else {
        ESP_LOGW(TAG, "  OEM_VIDEO_OUT_EN    GET FAILED (rc=%d)", rc);
    }

    // OEM_CAL_STATUS (2 words).
    rc = cci_get_attribute_n(CCI_CMD_OEM_CAL_STATUS, 2, buf);
    if (rc == 0) {
        uint32_t v = (uint32_t)buf[0] | ((uint32_t)buf[1] << 16);
        ESP_LOGI(TAG, "  OEM_CAL_STATUS      = 0x%08lx", (unsigned long)v);
    } else {
        ESP_LOGW(TAG, "  OEM_CAL_STATUS      GET FAILED (rc=%d)  ← OEM cal damaged?", rc);
    }

    // OEM_GPIO_MODE — known to work on this Lepton (it's in OEM module).
    // Useful as a control: if the others fail but this works, OEM is
    // selectively damaged (identity/cal area, but not all OEM regs).
    rc = cci_get_attribute_n(CCI_CMD_OEM_GPIO_MODE, 2, buf);
    if (rc == 0) {
        uint32_t v = (uint32_t)buf[0] | ((uint32_t)buf[1] << 16);
        ESP_LOGI(TAG, "  OEM_GPIO_MODE       = %lu  (control read — should be 5 after configure)",
                 (unsigned long)v);
    } else {
        ESP_LOGW(TAG, "  OEM_GPIO_MODE       GET FAILED (rc=%d)", rc);
    }

    // AGC + RAD + SYS settings — should reflect what we SET them to.
    struct { const char *name; uint16_t cmd; uint16_t words; } gets[] = {
        { "AGC_ENABLE         ", CCI_CMD_AGC_ENABLE,           2 },
        { "AGC_CALC_EN        ", CCI_CMD_AGC_CALC_ENABLE,      2 },
        { "RAD_ENABLE         ", CCI_CMD_RAD_ENABLE,           2 },
        { "RAD_TLINEAR_EN     ", CCI_CMD_RAD_TLINEAR_ENABLE,   2 },
        { "SYS_GAIN_MODE      ", CCI_CMD_SYS_GAIN_MODE,        2 },
        { "SYS_TELEMETRY      ", CCI_CMD_SYS_TELEMETRY_ENABLE, 2 },
    };
    for (size_t i = 0; i < sizeof(gets)/sizeof(gets[0]); i++) {
        rc = cci_get_attribute_n(gets[i].cmd, gets[i].words, buf);
        if (rc == 0) {
            uint32_t v = (uint32_t)buf[0] | ((uint32_t)buf[1] << 16);
            ESP_LOGI(TAG, "  %s= %lu  (raw: %04x %04x)",
                     gets[i].name, (unsigned long)v, buf[0], buf[1]);
        } else {
            ESP_LOGW(TAG, "  %s  GET FAILED (rc=%d)", gets[i].name, rc);
        }
    }

    ESP_LOGI(TAG, "=== end Lepton state dump ===");
}

// FFC diagnostic: trigger SYS_RUN_FFC and poll SYS_FFC_STATUS for 2 s.
// Healthy Lepton:
//   - status starts at 0 (idle/success)
//   - command transitions status to LEP_SYS_FFC_STATUS_BUSY (0xFFFFFFFC)
//     for ~300-500 ms while the shutter actuates and frames are captured
//   - returns to 0 (success) when calibration completes
// Damaged OEM FFC controller: status stays at 0 the whole time, OR
// transitions to a negative error code, OR oscillates without a clean
// busy→done transition. Combined with response-code logging on the
// RUN command itself, this is the cleanest software-only test for OEM
// pixel-pipeline health.
void lepton_cci_ffc_probe(void) {
    uint16_t buf[2];
    int rc;

    ESP_LOGI(TAG, "=== FFC probe start ===");

    rc = cci_get_attribute_n(CCI_CMD_SYS_FFC_STATUS, 2, buf);
    if (rc == 0) {
        uint32_t v = (uint32_t)buf[0] | ((uint32_t)buf[1] << 16);
        ESP_LOGI(TAG, "  pre-FFC status      = 0x%08lx", (unsigned long)v);
    } else {
        ESP_LOGW(TAG, "  pre-FFC status      GET FAILED (rc=%d)", rc);
    }

    ESP_LOGI(TAG, "  triggering SYS_RUN_FFC (0x%04x)...", CCI_CMD_SYS_RUN_FFC);
    uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
    bool ffc_ok = cci_run_command(CCI_CMD_SYS_RUN_FFC);
    uint32_t t_run = (uint32_t)(esp_timer_get_time() / 1000) - t0;
    ESP_LOGI(TAG, "  RUN_FFC returned %s in %u ms (healthy = 400-500 ms)",
             ffc_ok ? "OK" : "FAIL", (unsigned)t_run);

    // Poll FFC status every 50 ms for 2 s.
    for (int i = 0; i < 40; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
        rc = cci_get_attribute_n(CCI_CMD_SYS_FFC_STATUS, 2, buf);
        if (rc == 0) {
            uint32_t v = (uint32_t)buf[0] | ((uint32_t)buf[1] << 16);
            // Only log transitions or every 200 ms to keep output sane.
            static uint32_t s_last = 0xDEADBEEF;
            if (v != s_last || (i % 4) == 0) {
                ESP_LOGI(TAG, "  ffc t=%4d ms  status=0x%08lx %s",
                         (i + 1) * 50, (unsigned long)v,
                         v == 0xFFFFFFFC ? "(BUSY — shutter actuating)" :
                         v == 0          ? "(idle/success)" : "");
                s_last = v;
            }
        } else {
            ESP_LOGW(TAG, "  ffc t=%4d ms  status GET FAILED (rc=%d)", (i + 1) * 50, rc);
            break;
        }
    }

    ESP_LOGI(TAG, "=== FFC probe end ===");
}

esp_err_t lepton_cci_configure(void) {
    ESP_LOGI(TAG, "running full CCI configuration...");

    // OEM_REBOOT every configure call. Discovered on Fox session
    // 2026-05-12: forces Lepton to reset internal pipeline state and
    // unlocks the seg=1 emission that was otherwise silently missing.
    // Without this, frames=0; with it, frames flow at ~12-17 fps.
    // See memory:lepton_breakout_damaged.md.
    //
    // ALWAYS wait 5s after, regardless of cci_run_command return value:
    // Grasshopper's wait_idle_status checks the response code in STATUS,
    // which OEM_REBOOT often clears before we can read it (Lepton resets
    // mid-response). The reboot still happens — we just don't get a
    // clean ACK. Fox doesn't check response code so it returns true.
    (void)lepton_cci_oem_reboot();

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
    // Restore TLinear to the *intended* state set by lep_enable_tlinear(),
    // not unconditionally to 1. The previous version always re-enabled
    // TLinear when AGC was turned off, which silently broke streaming
    // while the TLinear=0 workaround is active.
    bool ok;
    if (enable) {
        // AGC mode requires TLinear off (8-bit display intensity, not
        // 14-bit radiometric counts).
        ok = cci_set_attribute(CCI_CMD_RAD_TLINEAR_ENABLE, 0);
        vTaskDelay(pdMS_TO_TICKS(50));
        if (ok) ok = cci_set_attribute(CCI_CMD_AGC_ENABLE, 1);
    } else {
        ok = cci_set_attribute(CCI_CMD_AGC_ENABLE, 0);
        vTaskDelay(pdMS_TO_TICKS(50));
        if (ok) ok = cci_set_attribute(CCI_CMD_RAD_TLINEAR_ENABLE,
                                       s_tlinear_intended ? 1 : 0);
    }
    if (!ok) return ESP_FAIL;
    s_agc = enable;
    ESP_LOGI(TAG, "AGC %s (TLinear restored to %d)",
             enable ? "on" : "off", s_tlinear_intended ? 1 : 0);
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
