// Grasshopper — phase 3 entry point.
//
// Brings up Wi-Fi STA, opens an outbound WSS to the debug relay, and
// powers/configures the FLIR Lepton 3.1R via the hal_lepton component
// (recycled VoSPI reader + CCI from Fox). The tick task pushes Init +
// Tick (with live thermal stats) to the relay every 1.5 s.
//
// Phase 4 layers in OV2640/OV5640, OLED, SD, captive-portal AP fallback.

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_chip_info.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "hal_camera.h"
#include "hal_lepton.h"
#include "hal_oled.h"
#include "hal_storage.h"
#include "img_converters.h"   // fmt2jpg from esp32-camera
#include "net_relay.h"
#include "net_wifi.h"
#include "proto_gen.h"
#include "sdkconfig.h"

// Lepton frame geometry — 160x120 raw uint16.
#include "board_pins.h"

#ifndef GRASSHOPPER_FW_VERSION
#define GRASSHOPPER_FW_VERSION "0.1.0-dev"
#endif

static const char *TAG = "app";

static int64_t g_boot_ms = 0;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static uint64_t uptime_ms(void) { return (uint64_t)(now_ms() - g_boot_ms); }

static char s_json_buf[2048];

// Local rolling fps tracker — frame counter is monotonic from hal_lepton,
// fps is delta over the tick interval.
static uint32_t s_last_frame_count = 0;
static uint32_t s_last_fps = 0;

// Camera sensor name — populated once at init, then read-only.
static char s_sensor_name[32] = "";

// Live wifi link — refreshed each tick by fill_wifi().
static char s_wifi_ip[16]   = "";
static char s_wifi_ssid[33] = "";
static int  s_wifi_rssi     = 0;

static const char *gain_str(int mode) {
    switch (mode) {
    case LEP_GAIN_HIGH: return "high";
    case LEP_GAIN_LOW:  return "low";
    case LEP_GAIN_AUTO: return "auto";
    default: return "?";
    }
}

static void fill_visible(Visible_t *out) {
    hal_camera_stats_t st = {0};
    hal_camera_get_stats(&st);
    out->fps     = st.fps;
    out->w       = st.width;
    out->h       = st.height;
    out->quality = st.jpeg_quality;
    out->ready   = st.ready;
    out->sensor  = s_sensor_name;
}

static void fill_wifi(Wifi_t *out) {
    net_wifi_get_link(s_wifi_ip, sizeof(s_wifi_ip),
                      s_wifi_ssid, sizeof(s_wifi_ssid),
                      &s_wifi_rssi);
    out->mode = "STA";
    out->ssid = s_wifi_ssid[0] ? s_wifi_ssid : CONFIG_GRASSHOPPER_WIFI_SSID;
    out->rssi = s_wifi_rssi;
    out->ip   = s_wifi_ip;
}

static void fill_storage(Storage_t *out) {
    hal_storage_sd_stats_t  ss = {0};
    hal_storage_lfs_stats_t ls = {0};
    hal_storage_sd_stats(&ss);
    hal_storage_lfs_stats(&ls);

    out->sdMounted   = ss.mounted;
    out->sdTotalKB   = ss.total_bytes / 1024;
    out->sdUsedKB    = ss.used_bytes  / 1024;
    out->sdFreeKB    = ss.free_bytes  / 1024;
    out->lfsMounted  = ls.mounted;
    out->lfsTotalKB  = (uint32_t)(ls.total_bytes / 1024);
    out->lfsUsedKB   = (uint32_t)(ls.used_bytes  / 1024);
}

static void fill_thermal(Thermal_t *out) {
    hal_lepton_stats_t st = {0};
    hal_lepton_get_stats(&st);

    out->fps = s_last_fps;
    out->gain = gain_str(hal_lepton_gain_mode());
    out->agc = hal_lepton_agc_enabled();
    out->frames = st.frames;
    out->totalPackets = st.totalPackets;
    out->validPackets = st.validPackets;
    out->discardPackets = st.discardPackets;
    out->syncEntries = st.syncEntries;
    out->lineMismatch = st.lineMismatch;
    out->segNot1 = st.segNot1;
    out->segMismatch = st.segMismatch;
    out->segZero = st.segZero;
    out->frameTimeout = st.frameTimeout;
    out->spliceDetected = st.spliceDetected;
    out->hwResets = st.hardwareResets;
    out->lastFFCMs = st.lastFFCMs;
    out->state = hal_lepton_state_name(st.state);
}

static void send_init(void) {
    Init_t init = {
        .type = "init",
        .fwVersion = GRASSHOPPER_FW_VERSION,
        .gitSha = "dev",
        .deviceId = CONFIG_GRASSHOPPER_DEVICE_ID,
        .state = DEVICESTATE_IDLE,
        .uptimeMs = uptime_ms(),
        .freeHeap = (uint32_t)esp_get_free_heap_size(),
        .freePsram = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        .ntpSynced = false,
        .epoch = 0,
    };
    fill_wifi(&init.wifi);
    fill_visible(&init.visible);
    fill_thermal(&init.thermal);
    fill_storage(&init.storage);

    size_t n = Init_to_json(s_json_buf, sizeof(s_json_buf), &init);
    if (n > 0 && n < sizeof(s_json_buf)) {
        net_relay_send(s_json_buf, n);
        ESP_LOGI(TAG, "init sent (%u B)", (unsigned)n);
    } else {
        ESP_LOGE(TAG, "init buffer too small (n=%u)", (unsigned)n);
    }
}

static void tick_task(void *arg) {
    Tick_t tick = { .type = "tick" };

    bool sent_init = false;
    int64_t last_fps_ms = now_ms();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1500));

        // Refresh fps regardless of relay state so it stays accurate.
        int64_t t = now_ms();
        uint32_t frames = hal_lepton_frame_count();
        int64_t dt = t - last_fps_ms;
        if (dt > 0) {
            uint32_t df = frames - s_last_frame_count;
            s_last_fps = (uint32_t)((df * 1000ULL) / (uint64_t)dt);
        }
        s_last_frame_count = frames;
        last_fps_ms = t;
        hal_camera_tick_fps();

        // Refresh wifi link strings (also feeds the OLED status below).
        net_wifi_get_link(s_wifi_ip, sizeof(s_wifi_ip),
                          s_wifi_ssid, sizeof(s_wifi_ssid),
                          &s_wifi_rssi);
        {
            hal_camera_stats_t cs = {0};
            hal_camera_get_stats(&cs);
            hal_storage_sd_stats_t ss = {0};
            hal_storage_sd_stats(&ss);

            hal_oled_status_t os = {
                .fw_version      = GRASSHOPPER_FW_VERSION,
                .wifi_ssid       = s_wifi_ssid,
                .wifi_ip         = s_wifi_ip,
                .wifi_rssi       = s_wifi_rssi,
                .relay_connected = net_relay_is_connected(),
                .therm_fps       = s_last_fps,
                .cam_fps         = cs.fps,
                .free_heap_kb    = (uint32_t)(esp_get_free_heap_size() / 1024),
                .sd_present      = ss.mounted,
            };
            hal_oled_set_status(&os);
        }

        if (!net_relay_is_connected()) {
            sent_init = false;
            continue;
        }
        if (!sent_init) {
            send_init();
            sent_init = true;
            continue;
        }

        tick.uptimeMs = uptime_ms();
        tick.freeHeap = (uint32_t)esp_get_free_heap_size();
        tick.freePsram = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        tick.epoch = (uint64_t)time(NULL);
        tick.state = DEVICESTATE_STREAMING;
        fill_wifi(&tick.wifi);
        fill_visible(&tick.visible);
        fill_thermal(&tick.thermal);
        fill_storage(&tick.storage);

        size_t n = Tick_to_json(s_json_buf, sizeof(s_json_buf), &tick);
        if (n > 0 && n < sizeof(s_json_buf)) {
            net_relay_send(s_json_buf, n);
        } else {
            ESP_LOGE(TAG, "tick buffer too small (n=%u)", (unsigned)n);
        }
    }
}

// ---------- Preview frame upload ----------
//
// Every PREVIEW_INTERVAL_MS, grab the latest visible JPEG and ship it
// over the relay WS as a binary frame with a 24 B header.
//
// Wire format (all little-endian):
//   offset 0:  4 B magic   = "GHFR"
//   offset 4:  1 B modality (1 = visible, 2 = thermal)
//   offset 5:  1 B version  (1)
//   offset 6:  2 B reserved
//   offset 8:  4 B width
//   offset 12: 4 B height
//   offset 16: 4 B jpeg length
//   offset 20: 4 B epoch seconds
//   offset 24: <jpeg bytes>

// 1 s instead of 2 s so the integer-rounded FPS metric isn't always 0.
// At ~30-50 KB per VGA q12 JPEG that's ~30-50 KB/s upload — fine on a
// hotspot, well under the relay's WS buffer (64 KB).
#define PREVIEW_INTERVAL_MS 1000
#define PREVIEW_HDR_LEN     24
#define PREVIEW_MAGIC       "GHFR"
#define PREVIEW_MOD_VIS     1
#define PREVIEW_MOD_THERM   2
#define PREVIEW_MAX_BYTES   65536

// PSRAM-backed staging buffer. 64 KB is plenty for VGA q=12 (typical
// 30-50 KB) plus the header. Allocated lazily on first upload.
static uint8_t *s_preview_buf = NULL;

static void put_u32_le(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

// 256-entry iron-style palette (R,G,B). Each entry packed as RGB565
// big-endian (the format fmt2jpg expects for PIXFORMAT_RGB565).
// Generated by interpolating between {black, purple, red, orange,
// yellow, white} in 8-bit RGB and converting to RGB565.
static uint16_t s_iron_lut[256];
static bool s_iron_lut_built = false;

static uint16_t rgb565_be(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t v = (uint16_t)((r & 0xF8) << 8 | (g & 0xFC) << 3 | (b >> 3));
    // RGB565 BE: high byte first.
    return (uint16_t)((v >> 8) | (v << 8));
}

static void build_iron_lut(void) {
    // Iron palette key points at indices 0/64/128/192/255.
    static const uint8_t key[5][3] = {
        {  0,   0,   0},   // black
        { 80,   0, 130},   // deep purple
        {220,  30,  60},   // red-orange
        {255, 200,  40},   // yellow-amber
        {255, 255, 255},   // white
    };
    for (int i = 0; i < 256; i++) {
        int seg = i * 4 / 256;            // 0..3
        int t   = (i * 4) % 256;          // 0..255 within segment
        const uint8_t *a = key[seg];
        const uint8_t *b = key[seg + 1];
        uint8_t r = (uint8_t)(a[0] + (b[0] - a[0]) * t / 256);
        uint8_t g = (uint8_t)(a[1] + (b[1] - a[1]) * t / 256);
        uint8_t bl = (uint8_t)(a[2] + (b[2] - a[2]) * t / 256);
        s_iron_lut[i] = rgb565_be(r, g, bl);
    }
    s_iron_lut_built = true;
}

// Encode the latest thermal frame into a JPEG using the iron palette
// with per-frame auto-ranging. Output goes into `dst`, capacity `cap`.
// Returns the JPEG byte length on success, 0 on failure.
//
// Buffers:
//   raw   — 19200 uint16 from hal_lepton_get_frame (PSRAM, lazy)
//   rgb   — 19200 uint16 RGB565 BE pixels                (PSRAM, lazy)
//   jpg   — malloc'd by fmt2jpg, copied into dst, freed
static uint16_t *s_therm_raw = NULL;
static uint8_t  *s_therm_rgb = NULL;

static size_t encode_thermal_jpeg(uint8_t *dst, size_t cap) {
    if (!s_iron_lut_built) build_iron_lut();
    if (!s_therm_raw) {
        s_therm_raw = heap_caps_malloc(LEP_PIXELS * sizeof(uint16_t),
                                        MALLOC_CAP_SPIRAM);
    }
    if (!s_therm_rgb) {
        s_therm_rgb = heap_caps_malloc(LEP_PIXELS * 2, MALLOC_CAP_SPIRAM);
    }
    if (!s_therm_raw || !s_therm_rgb) return 0;

    if (!hal_lepton_get_frame(s_therm_raw)) return 0;

    // Auto-range. Lepton 3.1R reports centi-Kelvin in the uint16, but
    // we don't need absolute units — relative range is fine for a
    // preview. Skip pixels that are 0 (uninitialized columns from a
    // partial frame); the splice detector should keep these rare but
    // they bias the min downward when they slip through.
    uint16_t mn = 0xFFFF, mx = 0;
    for (int i = 0; i < LEP_PIXELS; i++) {
        uint16_t v = s_therm_raw[i];
        if (v == 0) continue;
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    if (mx <= mn) { mx = (uint16_t)(mn + 1); }
    uint32_t range = (uint32_t)(mx - mn);

    // Map raw → palette index → RGB565 BE
    uint16_t *out = (uint16_t *)s_therm_rgb;
    for (int i = 0; i < LEP_PIXELS; i++) {
        uint16_t v = s_therm_raw[i];
        if (v == 0) v = mn;            // clamp empty pixels to floor
        else if (v < mn) v = mn;
        else if (v > mx) v = mx;
        uint32_t idx = ((uint32_t)(v - mn) * 255U) / range;
        out[i] = s_iron_lut[idx & 0xFF];
    }

    uint8_t *jpg_out = NULL;
    size_t   jpg_len = 0;
    bool ok = fmt2jpg(s_therm_rgb, LEP_PIXELS * 2, LEP_W, LEP_H,
                       PIXFORMAT_RGB565, 80, &jpg_out, &jpg_len);
    if (!ok || !jpg_out) return 0;
    if (jpg_len > cap) {
        free(jpg_out);
        return 0;
    }
    memcpy(dst, jpg_out, jpg_len);
    free(jpg_out);
    return jpg_len;
}

void preview_task(void *arg) {
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(PREVIEW_INTERVAL_MS));
        if (!net_relay_is_connected())  continue;
        if (!hal_camera_ready())        continue;

        if (!s_preview_buf) {
            s_preview_buf = heap_caps_malloc(PREVIEW_MAX_BYTES, MALLOC_CAP_SPIRAM);
            if (!s_preview_buf) {
                ESP_LOGE(TAG, "preview: no PSRAM for staging buf");
                vTaskDelay(pdMS_TO_TICKS(10000));
                continue;
            }
        }

        const uint8_t *jpg = NULL;
        size_t   jpg_len = 0;
        uint32_t w = 0, h = 0;
        if (hal_camera_grab_jpeg(&jpg, &jpg_len, &w, &h) != ESP_OK) continue;

        if (jpg_len + PREVIEW_HDR_LEN > PREVIEW_MAX_BYTES) {
            ESP_LOGW(TAG, "preview: jpg %u too big — skip", (unsigned)jpg_len);
            hal_camera_release();
            continue;
        }

        memcpy(s_preview_buf, PREVIEW_MAGIC, 4);
        s_preview_buf[4] = PREVIEW_MOD_VIS;
        s_preview_buf[5] = 1;        // version
        s_preview_buf[6] = 0;
        s_preview_buf[7] = 0;
        put_u32_le(s_preview_buf + 8,  w);
        put_u32_le(s_preview_buf + 12, h);
        put_u32_le(s_preview_buf + 16, (uint32_t)jpg_len);
        put_u32_le(s_preview_buf + 20, (uint32_t)time(NULL));
        memcpy(s_preview_buf + PREVIEW_HDR_LEN, jpg, jpg_len);

        hal_camera_release();   // releases the camera FB lock immediately

        size_t total = PREVIEW_HDR_LEN + jpg_len;
        if (net_relay_send_binary(s_preview_buf, total) != ESP_OK) {
            ESP_LOGW(TAG, "preview: send failed (%u B)", (unsigned)total);
        } else {
            ESP_LOGD(TAG, "preview: sent %ux%u %u B", (unsigned)w, (unsigned)h,
                     (unsigned)jpg_len);
        }
    }
}

// Separate task for thermal previews so visible isn't blocked while
// we're palette-mapping + JPEG-encoding 160x120 each tick.
static uint8_t *s_therm_preview_buf = NULL;

void thermal_preview_task(void *arg) {
    (void)arg;
    while (1) {
        // Slower than visible — VoSPI is at most ~9 fps and the
        // preview is for a relay debug view, not a stream. 1.5 s
        // gives the splice detector time to commit a clean frame
        // between renders.
        vTaskDelay(pdMS_TO_TICKS(1500));
        if (!net_relay_is_connected()) continue;

        if (!s_therm_preview_buf) {
            s_therm_preview_buf = heap_caps_malloc(PREVIEW_MAX_BYTES,
                                                    MALLOC_CAP_SPIRAM);
            if (!s_therm_preview_buf) {
                ESP_LOGE(TAG, "therm: no PSRAM for staging buf");
                vTaskDelay(pdMS_TO_TICKS(10000));
                continue;
            }
        }

        size_t jpg_len = encode_thermal_jpeg(
            s_therm_preview_buf + PREVIEW_HDR_LEN,
            PREVIEW_MAX_BYTES - PREVIEW_HDR_LEN);
        if (jpg_len == 0) continue;     // no committed frame yet

        memcpy(s_therm_preview_buf, PREVIEW_MAGIC, 4);
        s_therm_preview_buf[4] = PREVIEW_MOD_THERM;
        s_therm_preview_buf[5] = 1;
        s_therm_preview_buf[6] = 0;
        s_therm_preview_buf[7] = 0;
        put_u32_le(s_therm_preview_buf + 8,  LEP_W);
        put_u32_le(s_therm_preview_buf + 12, LEP_H);
        put_u32_le(s_therm_preview_buf + 16, (uint32_t)jpg_len);
        put_u32_le(s_therm_preview_buf + 20, (uint32_t)time(NULL));

        size_t total = PREVIEW_HDR_LEN + jpg_len;
        if (net_relay_send_binary(s_therm_preview_buf, total) != ESP_OK) {
            ESP_LOGW(TAG, "therm: send failed (%u B)", (unsigned)total);
        }
    }
}

void app_main(void) {
    g_boot_ms = now_ms();

    ESP_LOGI(TAG, "grasshopper " GRASSHOPPER_FW_VERSION " booting");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Storage — non-fatal if a partition is missing or the SD slot is empty.
    hal_storage_littlefs_mount();
    hal_storage_sd_mount();

    if (strlen(CONFIG_GRASSHOPPER_WIFI_SSID) > 0) {
        ESP_ERROR_CHECK(net_wifi_init());
        net_wifi_connect_blocking(CONFIG_GRASSHOPPER_WIFI_SSID,
                                   CONFIG_GRASSHOPPER_WIFI_PASS);
    } else {
        ESP_LOGW(TAG, "no Wi-Fi SSID configured — skipping STA");
    }

#if CONFIG_GRASSHOPPER_RELAY_ENABLED
    ESP_LOGI(TAG, "starting relay → %s", CONFIG_GRASSHOPPER_RELAY_URL);
    ESP_ERROR_CHECK(net_relay_start());
#else
    ESP_LOGI(TAG, "relay disabled (CONFIG_GRASSHOPPER_RELAY_ENABLED=n)");
#endif

    // Bring up the Lepton. Blocks for up to ~30s waiting for first
    // frame; relay client reconnect-loops in the background until then.
    if (hal_lepton_boot() != ESP_OK) {
        ESP_LOGE(TAG, "Lepton boot failed — continuing in degraded mode");
    }

    // Bring up the visible camera. Failure is non-fatal — thermal-only
    // mode still works.
    hal_camera_cfg_t cam_cfg = {
        .framesize    = HAL_CAM_FRAMESIZE_VGA,
        .jpeg_quality = 12,
        .fb_count     = 2,
    };
    if (hal_camera_init(&cam_cfg, s_sensor_name) != ESP_OK) {
        ESP_LOGW(TAG, "camera init failed — vis-stream unavailable");
        strncpy(s_sensor_name, "absent", sizeof(s_sensor_name) - 1);
    } else {
        ESP_LOGI(TAG, "camera ready (sensor=%s)", s_sensor_name);
    }

    // OLED — needs hal_lepton's I2C bus, so it goes after hal_lepton_boot.
    if (hal_oled_start() != ESP_OK) {
        ESP_LOGW(TAG, "oled start failed — running headless");
    }

    // Tick task — TLS write through mbedtls needs ≥6 KB of locals on
    // top of our 1.5 KB JSON buf, hence 8 KB.
    xTaskCreate(tick_task, "tick", 8192, NULL, 5, NULL);

    // Preview tasks — visible JPEG (every 1 s) + thermal palette+JPEG
    // (every 1.5 s). 8 KB stack: TLS write needs ≥6 KB, plus locals.
    extern void preview_task(void *);
    extern void thermal_preview_task(void *);
    xTaskCreate(preview_task,         "preview",  8192, NULL, 4, NULL);
    xTaskCreate(thermal_preview_task, "thermprv", 8192, NULL, 4, NULL);

    ESP_LOGI(TAG, "boot complete; free heap=%u psram=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
