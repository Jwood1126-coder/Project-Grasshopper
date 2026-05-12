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

#include "capture.h"
#include "sessions.h"
#include "cJSON.h"

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
        // Inject timelapse status — proto schema doesn't have it yet.
        timelapse_status_t tl = {0};
        timelapse_get_status(&tl);
        if (n > 1 && s_json_buf[n - 1] == '}') {
            int extra;
            if (tl.active) {
                extra = snprintf(s_json_buf + n - 1,
                    sizeof(s_json_buf) - (n - 1),
                    ",\"timelapse\":{"
                      "\"active\":true,"
                      "\"sessionId\":\"%s\","
                      "\"intervalSec\":%lu,"
                      "\"captureCount\":%lu,"
                      "\"startedMs\":%llu,"
                      "\"captureVis\":%s,"
                      "\"captureTherm\":%s"
                    "}}",
                    tl.session_id,
                    (unsigned long)tl.interval_sec,
                    (unsigned long)tl.capture_count,
                    (unsigned long long)tl.started_ms,
                    tl.capture_vis ? "true" : "false",
                    tl.capture_therm ? "true" : "false");
            } else {
                extra = snprintf(s_json_buf + n - 1,
                    sizeof(s_json_buf) - (n - 1),
                    ",\"timelapse\":{\"active\":false}}");
            }
            if (extra > 0 && (size_t)(n - 1 + extra) < sizeof(s_json_buf)) {
                n = (size_t)(n - 1 + extra);
            }
        }
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
            // Inject timelapse status into the JSON before sending. The
            // proto schema doesn't have a Timelapse_t yet (would require
            // codegen changes), so we splice it in by hand: trim the
            // trailing } and append ,"timelapse":{...}}.
            timelapse_status_t tl = {0};
            timelapse_get_status(&tl);
            if (n > 1 && s_json_buf[n - 1] == '}') {
                int extra;
                if (tl.active) {
                    extra = snprintf(s_json_buf + n - 1,
                        sizeof(s_json_buf) - (n - 1),
                        ",\"timelapse\":{"
                          "\"active\":true,"
                          "\"sessionId\":\"%s\","
                          "\"intervalSec\":%lu,"
                          "\"captureCount\":%lu,"
                          "\"startedMs\":%llu,"
                          "\"captureVis\":%s,"
                          "\"captureTherm\":%s"
                        "}}",
                        tl.session_id,
                        (unsigned long)tl.interval_sec,
                        (unsigned long)tl.capture_count,
                        (unsigned long long)tl.started_ms,
                        tl.capture_vis ? "true" : "false",
                        tl.capture_therm ? "true" : "false");
                } else {
                    extra = snprintf(s_json_buf + n - 1,
                        sizeof(s_json_buf) - (n - 1),
                        ",\"timelapse\":{\"active\":false}}");
                }
                if (extra > 0 && (size_t)(n - 1 + extra) < sizeof(s_json_buf)) {
                    n = (size_t)(n - 1 + extra);
                }
            }
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

// Thermal JPEG encoder + iron palette LUT moved to capture.c so the
// capture command can share them with the periodic preview task.
// See main/include/capture.h.

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
        // VoSPI is at most ~9 fps. 750 ms gives the splice detector
        // time to commit a clean frame between renders, while keeping
        // the UI feeling live (1.5 s was perceptibly stale).
        vTaskDelay(pdMS_TO_TICKS(750));
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

        size_t jpg_len = capture_encode_thermal_jpeg(
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

// Command handler — dispatched by net_relay for non-built-in commands.
// Return values per net_relay_cmd_status_t (OK/FAIL/DEFERRED).
static net_relay_cmd_status_t app_cmd_handler(const char *cmd, const char *id,
                                               const void *payload_json_root,
                                               char *msg_out, size_t msg_cap) {
    (void)id;
    const cJSON *payload = (const cJSON *)payload_json_root;

    if (strcmp(cmd, "capture.now") == 0) {
        char session_id[64];
        esp_err_t err = capture_now(session_id, sizeof(session_id),
                                     msg_out, msg_cap);
        return err == ESP_OK ? NET_RELAY_CMD_OK : NET_RELAY_CMD_FAIL;
    }

    if (strcmp(cmd, "timelapse.start") == 0) {
        // Payload: { intervalSec: number, captureVis: bool, captureTherm: bool }
        uint32_t interval = 30;
        bool capture_vis = true, capture_therm = true;
        if (payload) {
            const cJSON *iv = cJSON_GetObjectItemCaseSensitive(payload, "intervalSec");
            if (cJSON_IsNumber(iv)) interval = (uint32_t)iv->valueint;
            const cJSON *cv = cJSON_GetObjectItemCaseSensitive(payload, "captureVis");
            if (cJSON_IsBool(cv)) capture_vis = cJSON_IsTrue(cv);
            const cJSON *ct = cJSON_GetObjectItemCaseSensitive(payload, "captureTherm");
            if (cJSON_IsBool(ct)) capture_therm = cJSON_IsTrue(ct);
        }
        char session_id[64];
        esp_err_t err = timelapse_start(interval, capture_vis, capture_therm,
                                         session_id, sizeof(session_id),
                                         msg_out, msg_cap);
        return err == ESP_OK ? NET_RELAY_CMD_OK : NET_RELAY_CMD_FAIL;
    }

    if (strcmp(cmd, "timelapse.stop") == 0) {
        esp_err_t err = timelapse_stop(msg_out, msg_cap);
        return err == ESP_OK ? NET_RELAY_CMD_OK : NET_RELAY_CMD_FAIL;
    }

    // sessions.* — offload to worker so the WS task isn't blocked by SD.
    if (strcmp(cmd, "sessions.list") == 0) {
        if (sessions_enqueue_list(id) != ESP_OK) {
            snprintf(msg_out, msg_cap, "sessions queue full");
            return NET_RELAY_CMD_FAIL;
        }
        snprintf(msg_out, msg_cap, "queued");
        return NET_RELAY_CMD_DEFERRED;
    }
    if (strcmp(cmd, "sessions.get") == 0) {
        const char *sid = NULL;
        if (payload) {
            const cJSON *s = cJSON_GetObjectItemCaseSensitive(payload, "sessionId");
            if (cJSON_IsString(s)) sid = s->valuestring;
        }
        if (!sid) {
            snprintf(msg_out, msg_cap, "missing sessionId");
            return NET_RELAY_CMD_FAIL;
        }
        if (sessions_enqueue_get(id, sid) != ESP_OK) {
            snprintf(msg_out, msg_cap, "sessions queue full");
            return NET_RELAY_CMD_FAIL;
        }
        snprintf(msg_out, msg_cap, "queued");
        return NET_RELAY_CMD_DEFERRED;
    }
    if (strcmp(cmd, "session.read_file") == 0) {
        const char *sid = NULL, *fn = NULL;
        uint32_t off = 0, mlen = 0;
        if (payload) {
            const cJSON *s = cJSON_GetObjectItemCaseSensitive(payload, "sessionId");
            const cJSON *f = cJSON_GetObjectItemCaseSensitive(payload, "filename");
            const cJSON *o = cJSON_GetObjectItemCaseSensitive(payload, "offset");
            const cJSON *m = cJSON_GetObjectItemCaseSensitive(payload, "maxLen");
            if (cJSON_IsString(s)) sid = s->valuestring;
            if (cJSON_IsString(f)) fn = f->valuestring;
            if (cJSON_IsNumber(o)) off = (uint32_t)o->valuedouble;
            if (cJSON_IsNumber(m)) mlen = (uint32_t)m->valuedouble;
        }
        if (!sid || !fn) {
            snprintf(msg_out, msg_cap, "missing sessionId/filename");
            return NET_RELAY_CMD_FAIL;
        }
        if (sessions_enqueue_read_file(id, sid, fn, off, mlen) != ESP_OK) {
            snprintf(msg_out, msg_cap, "sessions queue full");
            return NET_RELAY_CMD_FAIL;
        }
        snprintf(msg_out, msg_cap, "queued");
        return NET_RELAY_CMD_DEFERRED;
    }

    snprintf(msg_out, msg_cap, "unknown command: %s", cmd);
    return NET_RELAY_CMD_FAIL;
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

    if (sessions_init() != ESP_OK) {
        ESP_LOGW(TAG, "sessions worker failed to start — sessions.* will be unavailable");
    }

#if CONFIG_GRASSHOPPER_RELAY_ENABLED
    ESP_LOGI(TAG, "starting relay → %s", CONFIG_GRASSHOPPER_RELAY_URL);
    net_relay_register_cmd_handler(app_cmd_handler);
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
