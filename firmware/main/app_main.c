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
#include "nvs.h"
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
#include "session_store.h"
#include "ds_scheduler.h"
#include "power_manager.h"
#include "esp_random.h"
#include "sessions.h"
#include "ota.h"
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

// Build the radiometric block — appended into both init and tick.
// Codex's Phase 1 checklist: report capability flags + raw resolution
// alongside derived display-temp fields. UI does its own °C/°F
// conversion; centerTempF is included for at-a-glance reading.
//
// scale_x100 = number of centi-Kelvin per raw count.
//   resolution=0 → 0.1 K/count → 10 cK/count
//   resolution=1 → 0.01 K/count → 1 cK/count
static int build_radiometric_json(char *out, size_t cap) {
    bool active = false, auto_res = false;
    uint16_t res = 0;
    lepton_cci_get_tlinear_state(&active, &auto_res, &res);
    int scale_x100 = (res == 1) ? 1 : 10;

    uint32_t mn_raw = 0, mx_raw = 0, ce_raw = 0;
    capture_get_last_thermal_temps_ck(&mn_raw, &mx_raw, &ce_raw);

    // Convert raw → °F via centi-Kelvin. cK = raw * scale_x100, then
    // C = cK/100 - 273.15, F = C*9/5 + 32. Inlined per call below.
    #define RAW_TO_F(raw) \
        ((((double)(raw) * (double)scale_x100) / 100.0 - 273.15) * 9.0 / 5.0 + 32.0)
    bool have_temps = active && (mx_raw > 0);
    if (!have_temps) {
        return snprintf(out, cap,
            ",\"radiometric\":{\"active\":%s,\"tlinearAutoRes\":%s,"
            "\"tlinearResolution\":%u,\"haveTemps\":false}",
            active ? "true" : "false",
            auto_res ? "true" : "false",
            (unsigned)res);
    }
    int n = snprintf(out, cap,
        ",\"radiometric\":{\"active\":true,\"tlinearAutoRes\":%s,"
        "\"tlinearResolution\":%u,\"haveTemps\":true,"
        "\"minRaw\":%lu,\"maxRaw\":%lu,\"centerRaw\":%lu,"
        "\"minTempF\":%.2f,\"maxTempF\":%.2f,\"centerTempF\":%.2f}",
        auto_res ? "true" : "false",
        (unsigned)res,
        (unsigned long)mn_raw, (unsigned long)mx_raw, (unsigned long)ce_raw,
        RAW_TO_F(mn_raw), RAW_TO_F(mx_raw), RAW_TO_F(ce_raw));
    #undef RAW_TO_F
    return n;
}

// Splice extra fields (timelapse status + user settings + radiometric)
// into a JSON envelope produced by Init_to_json/Tick_to_json. Both
// fields live outside the proto schema, so we hand-append by trimming
// the trailing `}` and writing the additions before re-closing.
static size_t splice_extras(char *buf, size_t n, size_t cap) {
    if (n == 0 || n + 1 >= cap || buf[n - 1] != '}') return n;

    timelapse_status_t tl = {0};
    timelapse_get_status(&tl);

    // Drop the trailing } and start appending.
    n -= 1;
    int extra;
    if (tl.active) {
        extra = snprintf(buf + n, cap - n,
            ",\"timelapse\":{"
              "\"active\":true,"
              "\"sessionId\":\"%s\","
              "\"intervalSec\":%lu,"
              "\"captureCount\":%lu,"
              "\"startedMs\":%llu,"
              "\"captureVis\":%s,"
              "\"captureTherm\":%s"
            "},"
            "\"settings\":{"
              "\"visRotation\":%u,"
              "\"thermRotation\":%u"
            "}",
            tl.session_id,
            (unsigned long)tl.interval_sec,
            (unsigned long)tl.capture_count,
            (unsigned long long)tl.started_ms,
            tl.capture_vis ? "true" : "false",
            tl.capture_therm ? "true" : "false",
            (unsigned)capture_get_visible_rotation(),
            (unsigned)capture_get_thermal_rotation());
    } else {
        extra = snprintf(buf + n, cap - n,
            ",\"timelapse\":{\"active\":false},"
            "\"settings\":{"
              "\"visRotation\":%u,"
              "\"thermRotation\":%u"
            "}",
            (unsigned)capture_get_visible_rotation(),
            (unsigned)capture_get_thermal_rotation());
    }
    if (extra <= 0 || (size_t)(n + extra) >= cap) return n + 1;
    n += extra;

    extra = build_radiometric_json(buf + n, cap - n);
    if (extra > 0 && (size_t)(n + extra) < cap) n += extra;

    // Deep-sleep block — only when a session is active. The dashboard
    // reads this to render "session in progress, next wake at T+30s,
    // 4/12 captures done" during wake-window phases (the only times
    // the device is online during a DS session).
    if (ds_scheduler_state() == DS_ACTIVE) {
        extra = snprintf(buf + n, cap - n,
            ",\"deepSleep\":{"
              "\"active\":true,"
              "\"sessionId\":\"%s\","
              "\"nextSeq\":%lu,"
              "\"maxCaptures\":%lu"
            "}",
            ds_scheduler_session_id(),
            (unsigned long)ds_scheduler_next_seq(),
            (unsigned long)ds_scheduler_max_captures());
        if (extra > 0 && (size_t)(n + extra) < cap) n += extra;
    }

    if (n + 1 >= cap) return n;
    buf[n++] = '}';
    return n;
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
        n = splice_extras(s_json_buf, n, sizeof(s_json_buf));
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
            n = splice_extras(s_json_buf, n, sizeof(s_json_buf));
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

// 2 s rather than 1 s — at the previous 1 Hz visible + 1.33 Hz thermal
// (~106 KB/s combined), the WS-client mutex was held for 1-2 s per
// large send on iPhone-hotspot uplinks. Other senders (tick, log,
// thermal preview) timed out at 1000 ms with "Could not lock
// ws-client within 1000 timeout", and the relay-side ping/pong stalled
// long enough for the server to drop us with code=1006 — perpetual
// thrash. Halving preview rates gives the WS task headroom; integer
// fps still rounds nonzero because grabs land at ~0.5 Hz.
#define PREVIEW_INTERVAL_MS 2000
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

        // For 90°/270° visible rotation, decode/rotate/re-encode here so
        // the dashboard preview matches what recordings will look like.
        // The rotated buffer owns its own memory; copy it before release.
        uint8_t *rot_jpg = NULL; size_t rot_len = 0;
        uint32_t rw = w, rh = h;
        bool rotated = capture_rotate_visible_jpeg_if_needed(
            jpg, jpg_len, w, h, &rot_jpg, &rot_len, &rw, &rh);
        const uint8_t *send_jpg = rotated ? rot_jpg : jpg;
        size_t         send_len = rotated ? rot_len : jpg_len;

        if (send_len + PREVIEW_HDR_LEN > PREVIEW_MAX_BYTES) {
            ESP_LOGW(TAG, "preview: jpg %u too big — skip", (unsigned)send_len);
            if (rot_jpg) free(rot_jpg);
            hal_camera_release();
            continue;
        }

        memcpy(s_preview_buf, PREVIEW_MAGIC, 4);
        s_preview_buf[4] = PREVIEW_MOD_VIS;
        s_preview_buf[5] = 1;        // version
        s_preview_buf[6] = 0;
        s_preview_buf[7] = 0;
        put_u32_le(s_preview_buf + 8,  rw);
        put_u32_le(s_preview_buf + 12, rh);
        put_u32_le(s_preview_buf + 16, (uint32_t)send_len);
        put_u32_le(s_preview_buf + 20, (uint32_t)time(NULL));
        memcpy(s_preview_buf + PREVIEW_HDR_LEN, send_jpg, send_len);

        if (rot_jpg) free(rot_jpg);
        hal_camera_release();   // releases the camera FB lock immediately
        size_t  jpg_len_dummy = send_len;
        (void)jpg_len_dummy;     // keep below ESP_LOGD happy
        jpg_len = send_len;

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
        // 1.5 s rather than 750 ms — combined with the doubled visible
        // interval, brings total preview throughput to ~50 KB/s,
        // leaving the WS client enough cycles to ping/pong + ship
        // ticks/cmd.results without timing out on the client mutex.
        // 1.5 s is perceptible but acceptable until the WS sender
        // is rebuilt as a single-task queue (see PR-C backlog).
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

        // Reserve room at the tail for the raw uint16 frame (160*120*2
        // = 38400 B). Trailer comes after the JPEG bytes; relay/UI
        // detect it via total > HDR+jpg_len. Lets the dashboard show
        // per-pixel temps on hover with no extra request round-trip.
        const size_t RAW_BYTES = (size_t)LEP_W * LEP_H * 2;
        size_t jpg_cap = PREVIEW_MAX_BYTES - PREVIEW_HDR_LEN - RAW_BYTES;
        size_t jpg_len = capture_encode_thermal_jpeg(
            s_therm_preview_buf + PREVIEW_HDR_LEN, jpg_cap);
        if (jpg_len == 0) continue;     // no committed frame yet

        memcpy(s_therm_preview_buf, PREVIEW_MAGIC, 4);
        s_therm_preview_buf[4] = PREVIEW_MOD_THERM;
        s_therm_preview_buf[5] = 2;        // version 2: raw trailer present
        s_therm_preview_buf[6] = 0;
        s_therm_preview_buf[7] = 0;
        put_u32_le(s_therm_preview_buf + 8,  LEP_W);
        put_u32_le(s_therm_preview_buf + 12, LEP_H);
        put_u32_le(s_therm_preview_buf + 16, (uint32_t)jpg_len);
        put_u32_le(s_therm_preview_buf + 20, (uint32_t)time(NULL));

        // Append the raw uint16 frame (pre-rotation). The first capture
        // before any thermal commit returns false → skip the trailer
        // and fall back to v1 layout.
        bool raw_ok = capture_snapshot_thermal_raw(
            (uint16_t *)(s_therm_preview_buf + PREVIEW_HDR_LEN + jpg_len),
            RAW_BYTES);
        size_t trailer = raw_ok ? RAW_BYTES : 0;
        if (!raw_ok) s_therm_preview_buf[5] = 1;   // back to v1 if no raw

        size_t total = PREVIEW_HDR_LEN + jpg_len + trailer;
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
        // Payload: { intervalSec, captureVis, captureTherm, maxDurationSec?,
        //            deepSleep?, maxCaptures?, wakeWifi?, wakeWindowSec? }
        // deepSleep:true  → switch to ds_scheduler (PR-C). intervalSec must
        //                   be ≥60. maxCaptures is required (≥1).
        // wakeWifi:true   → after each commit, before sleep, bring up Wi-Fi
        //                   + relay for wakeWindowSec (default 15, range
        //                   1..60) and accept timelapse.stop.
        uint32_t interval = 30, max_dur = 0, max_caps = 0;
        bool capture_vis = true, capture_therm = true;
        bool deep_sleep  = false;
        bool wake_wifi   = false;
        uint32_t wake_window = 15;
        uint32_t wake_wifi_every = 1;     // PR-G schema-stable; firmware
                                           // honors 1 (= every wake) today,
                                           // higher values stored but ignored
                                           // until soak data informs default
        if (payload) {
            const cJSON *iv = cJSON_GetObjectItemCaseSensitive(payload, "intervalSec");
            if (cJSON_IsNumber(iv)) interval = (uint32_t)iv->valueint;
            const cJSON *cv = cJSON_GetObjectItemCaseSensitive(payload, "captureVis");
            if (cJSON_IsBool(cv)) capture_vis = cJSON_IsTrue(cv);
            const cJSON *ct = cJSON_GetObjectItemCaseSensitive(payload, "captureTherm");
            if (cJSON_IsBool(ct)) capture_therm = cJSON_IsTrue(ct);
            const cJSON *md = cJSON_GetObjectItemCaseSensitive(payload, "maxDurationSec");
            if (cJSON_IsNumber(md) && md->valueint > 0) max_dur = (uint32_t)md->valueint;
            const cJSON *ds = cJSON_GetObjectItemCaseSensitive(payload, "deepSleep");
            if (cJSON_IsBool(ds)) deep_sleep = cJSON_IsTrue(ds);
            const cJSON *mc = cJSON_GetObjectItemCaseSensitive(payload, "maxCaptures");
            if (cJSON_IsNumber(mc) && mc->valueint > 0) max_caps = (uint32_t)mc->valueint;
            const cJSON *ww = cJSON_GetObjectItemCaseSensitive(payload, "wakeWifi");
            if (cJSON_IsBool(ww)) wake_wifi = cJSON_IsTrue(ww);
            const cJSON *ws = cJSON_GetObjectItemCaseSensitive(payload, "wakeWindowSec");
            if (cJSON_IsNumber(ws) && ws->valueint > 0) wake_window = (uint32_t)ws->valueint;
            const cJSON *we = cJSON_GetObjectItemCaseSensitive(payload, "wakeWifiEvery");
            if (cJSON_IsNumber(we) && we->valueint > 0) wake_wifi_every = (uint32_t)we->valueint;
        }

        if (deep_sleep) {
            if (max_caps == 0) {
                snprintf(msg_out, msg_cap, "deep-sleep mode requires maxCaptures ≥ 1");
                return NET_RELAY_CMD_FAIL;
            }
            if (interval < 60) {
                snprintf(msg_out, msg_cap, "deep-sleep mode requires intervalSec ≥ 60 (Lepton boot ~10s/wake)");
                return NET_RELAY_CMD_FAIL;
            }
            if (wake_window > 60) wake_window = 60;
            char session_id[64];
            uint32_t r = (uint32_t)esp_random();
            snprintf(session_id, sizeof(session_id), "session_%lu",
                     (unsigned long)(1000 + (r % 99000)));
            ds_arm_args_t args = {
                .max_captures    = max_caps,
                .interval_sec    = interval,
                .capture_vis     = capture_vis,
                .capture_therm   = capture_therm,
                .wake_wifi       = wake_wifi,
                .wake_window_sec = wake_window,
                .wake_wifi_every = wake_wifi_every,
            };
            esp_err_t err = ds_scheduler_arm(session_id, &args);
            if (err != ESP_OK) {
                snprintf(msg_out, msg_cap, "ds_scheduler_arm failed: %s",
                         esp_err_to_name(err));
                return NET_RELAY_CMD_FAIL;
            }
            // Emit the success cmd.result BEFORE returning, so the UI
            // sees "armed" and clears its 8 s pending-cmd timeout. The
            // worker (below) sleeps 1.5 s before bringing peripherals
            // up, which gives this message time to actually flush over
            // TLS — running the long sensor bring-up + capture in the
            // WS task context (the old pattern) was blocking the WS
            // event loop and starving the sender, so the cmd.result
            // never made it onto the wire before deep_sleep killed
            // the radio.
            net_relay_emit_cmd_result("timelapse.start", id, true,
                                      "deep-sleep session armed", NULL);
            char m[256];
            snprintf(m, sizeof(m),
                     "%s armed: %lu captures x %lus, vis=%s therm=%s — first capture firing, then deep sleep",
                     session_id, (unsigned long)max_caps, (unsigned long)interval,
                     capture_vis ? "on" : "off", capture_therm ? "on" : "off");
            ESP_LOGI(TAG, "%s", m);
            // Spawn worker; cmd handler returns DEFERRED so the
            // dispatcher does NOT emit a second cmd.result.
            if (ds_scheduler_run_one_cycle_async() != ESP_OK) {
                // Worker failed to spawn — abort the arm so we don't
                // leave NVS pointing at a session that'll never run.
                ds_scheduler_abort();
                snprintf(msg_out, msg_cap, "ds worker spawn failed");
                return NET_RELAY_CMD_FAIL;
            }
            return NET_RELAY_CMD_DEFERRED;
        }

        // Live (non-deep-sleep) timelapse path — unchanged.
        char session_id[64];
        esp_err_t err = timelapse_start(interval, capture_vis, capture_therm,
                                         max_dur,
                                         session_id, sizeof(session_id),
                                         msg_out, msg_cap);
        return err == ESP_OK ? NET_RELAY_CMD_OK : NET_RELAY_CMD_FAIL;
    }

    if (strcmp(cmd, "timelapse.stop") == 0) {
        // Route to whichever subsystem owns the active session. During
        // a DS wake-window phase the live timelapse module isn't
        // running, so timelapse_stop would say "no active timelapse"
        // even though the user clearly wants to stop the deep-sleep
        // session they just set up. ds_scheduler_state() tells us.
        if (ds_scheduler_state() == DS_ACTIVE) {
            ds_scheduler_request_stop();
            snprintf(msg_out, msg_cap,
                     "deep-sleep session %s will finalize at end of this wake window",
                     ds_scheduler_session_id());
            return NET_RELAY_CMD_OK;
        }
        esp_err_t err = timelapse_stop(msg_out, msg_cap);
        return err == ESP_OK ? NET_RELAY_CMD_OK : NET_RELAY_CMD_FAIL;
    }

    // settings.update: live-apply user orientation choices and persist
    // to NVS so they survive reboot. Payload may include any subset of
    // { visRotation: 0|1|2|3, thermRotation: 0|1|2|3 }.
    if (strcmp(cmd, "settings.update") == 0) {
        nvs_handle_t h = 0;
        bool nvs_ok = nvs_open("ghset", NVS_READWRITE, &h) == ESP_OK;
        int applied = 0;
        if (payload) {
            const cJSON *vr = cJSON_GetObjectItemCaseSensitive(payload, "visRotation");
            const cJSON *tr = cJSON_GetObjectItemCaseSensitive(payload, "thermRotation");
            if (cJSON_IsNumber(vr)) {
                uint8_t r = (uint8_t)vr->valueint & 3;
                capture_set_visible_rotation(r);
                if (nvs_ok) nvs_set_u8(h, "vr", r);
                applied++;
            }
            if (cJSON_IsNumber(tr)) {
                uint8_t r = (uint8_t)tr->valueint & 3;
                capture_set_thermal_rotation(r);
                if (nvs_ok) nvs_set_u8(h, "tr", r);
                applied++;
            }
        }
        if (nvs_ok) { nvs_commit(h); nvs_close(h); }
        snprintf(msg_out, msg_cap,
                 "applied %d setting(s): visRot=%d thermRot=%d",
                 applied,
                 capture_get_visible_rotation(),
                 capture_get_thermal_rotation());
        return NET_RELAY_CMD_OK;
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
    if (strcmp(cmd, "firmware.update") == 0) {
        // Payload: { url: "https://…/grasshopper.bin" }. Spawns an
        // OTA worker that streams the binary into the inactive slot,
        // verifies, and reboots. cmd.result events report progress
        // and final outcome.
        const char *url = NULL;
        if (payload) {
            const cJSON *u = cJSON_GetObjectItemCaseSensitive(payload, "url");
            if (cJSON_IsString(u)) url = u->valuestring;
        }
        if (!url || !*url) {
            snprintf(msg_out, msg_cap, "missing url");
            return NET_RELAY_CMD_FAIL;
        }
        if (ota_start_https(id, url) != ESP_OK) {
            snprintf(msg_out, msg_cap, "ota start failed");
            return NET_RELAY_CMD_FAIL;
        }
        snprintf(msg_out, msg_cap, "ota started");
        return NET_RELAY_CMD_DEFERRED;
    }
    if (strcmp(cmd, "sessions.delete") == 0) {
        const char *sid = NULL;
        if (payload) {
            const cJSON *s = cJSON_GetObjectItemCaseSensitive(payload, "sessionId");
            if (cJSON_IsString(s)) sid = s->valuestring;
        }
        if (!sid) {
            snprintf(msg_out, msg_cap, "missing sessionId");
            return NET_RELAY_CMD_FAIL;
        }
        if (sessions_enqueue_delete(id, sid) != ESP_OK) {
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

    // Sweep any incomplete sessions left over from a crash / power loss
    // mid-write. Repairs session.json from journal, deletes orphan
    // finalized files (seq > journal max), deletes any *.tmp orphans.
    // Must run before any session_store_open call, while no other SD
    // writers are around. Cheap when there's nothing to do.
    session_store_recover_all();

    // Register the cmd handler BEFORE ds_scheduler_maybe_handle_wake.
    // The wake-window phase (PR-D, when wakeWifi:true) brings up the
    // relay inside the wake cycle to accept timelapse.stop, so the
    // handler must already be installed by then. net_relay_register
    // is just a function-pointer store — safe to call before relay
    // start, no side effects.
    net_relay_register_cmd_handler(app_cmd_handler);

    // Deep-sleep wake handler. Examines wakeup cause + NVS state and:
    //   - timer wake + active session → run one capture cycle (returns
    //     here only if the session completed OR was stopped via
    //     wake-window cmd; otherwise sleeps and does not return)
    //   - any other wake → ESP_ERR_INVALID_STATE; we fall through to
    //     normal boot. This keeps the cold-boot path unchanged for
    //     non-DS use, while making the DS wake path cheap (it skips
    //     WiFi/relay/preview/OLED/tick init entirely when wakeWifi
    //     is false).
    ds_scheduler_init();
    esp_err_t ds_wake_rc = ds_scheduler_maybe_handle_wake();
    // ESP_OK from maybe_handle_wake means the DS cycle ran and the
    // session is now complete (last capture committed, or remote-stopped
    // via wake-window cmd). At this point we have peripherals partly
    // brought up by run_one_cycle (Lepton, possibly camera, possibly
    // Wi-Fi/relay if wake-window fired) — re-running the live-mode init
    // path on top of that produces double-init bugs. Cleanest path back
    // to normal operation is a fresh boot: NVS session is already cleared,
    // so the next boot's wake handler will see no DS state and run the
    // standard live-mode init from a known-good cold start.
    if (ds_wake_rc == ESP_OK) {
        ESP_LOGI(TAG, "DS session completed in this wake — restarting for clean live-mode boot");
        vTaskDelay(pdMS_TO_TICKS(500));   // flush logs
        esp_restart();
    }
    // Fell through with INVALID_STATE — not a DS wake (cold boot, brownout,
    // user reset). Continue normal boot unchanged.

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
    ESP_ERROR_CHECK(net_relay_start());
    // OTA pending-verify watchdog: if this image is awaiting validation
    // (just booted from a fresh OTA), wait until the relay has been
    // connected for ~15 s before marking the image as known-good.
    // Anything that crashes / blocks the relay before then triggers
    // a rollback to the previous slot on next boot.
    ota_pending_verify_arm(15000);
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

    // Load persisted user settings (vis flip, thermal rotation) and
    // apply them to the just-initialised hal_camera + capture pipeline
    // so live preview comes up with the user's last orientation.
    {
        nvs_handle_t h;
        if (nvs_open("ghset", NVS_READONLY, &h) == ESP_OK) {
            uint8_t v = 0;
            // visRotation supersedes the older visHmirror/visVflip pair.
            // capture_set_visible_rotation drives the OV2640 flip
            // registers as a side-effect.
            if (nvs_get_u8(h, "vr", &v) == ESP_OK) capture_set_visible_rotation(v);
            v = 0;
            if (nvs_get_u8(h, "tr", &v) == ESP_OK) capture_set_thermal_rotation(v);
            nvs_close(h);
            ESP_LOGI(TAG, "settings loaded: visRot=%d thermRot=%d",
                     capture_get_visible_rotation(),
                     capture_get_thermal_rotation());
        }
    }

    // OLED — needs hal_lepton's I2C bus, so it goes after hal_lepton_boot.
    if (hal_oled_start() != ESP_OK) {
        ESP_LOGW(TAG, "oled start failed — running headless");
    }

    // Tick + preview tasks all pinned to core 0. Core 1 is reserved
    // for the VoSPI reader; any host-side stall there starves the
    // Lepton's continuous packet stream and produces lineMismatch
    // aborts. WiFi + esp_timer are already on core 0 by sdkconfig.
    xTaskCreatePinnedToCore(tick_task, "tick", 8192, NULL, 5, NULL, 0);

    extern void preview_task(void *);
    extern void thermal_preview_task(void *);
    xTaskCreatePinnedToCore(preview_task,         "preview",  8192, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(thermal_preview_task, "thermprv", 8192, NULL, 4, NULL, 0);

    ESP_LOGI(TAG, "boot complete; free heap=%u psram=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
