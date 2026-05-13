// Capture — single-shot vis+therm JPEG to SD session directory.
//
// See include/capture.h for the SD layout. Thermal encoder lives here
// because it's shared with the periodic preview task in app_main.c.
//
// Phase 3 minimum: capture_now() writes a brand-new session of one.
// Timelapse (start/stop, periodic capture loop) builds on this.

#include "capture.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "img_converters.h"

#include "hal_camera.h"
#include "hal_lepton.h"
#include "hal_storage.h"

static const char *TAG = "capture";

#define LEP_W       160
#define LEP_H       120
#define LEP_PIXELS  (LEP_W * LEP_H)
#define THERM_JPEG_MAX_BYTES   65536
#define SESSIONS_BASE_DIR       "/sdcard/timelapse"

// ---- Iron palette LUT (256 entries, RGB565 BE for fmt2jpg) ----

static uint16_t s_iron_lut[256];
static bool     s_iron_lut_built = false;

static uint16_t rgb565_be(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t v = (uint16_t)((r & 0xF8) << 8 | (g & 0xFC) << 3 | (b >> 3));
    return (uint16_t)((v >> 8) | (v << 8));
}

static void build_iron_lut(void) {
    static const uint8_t key[5][3] = {
        {  0,   0,   0},   // black
        { 80,   0, 130},   // deep purple
        {220,  30,  60},   // red-orange
        {255, 200,  40},   // yellow-amber
        {255, 255, 255},   // white
    };
    for (int i = 0; i < 256; i++) {
        int seg = i * 4 / 256;
        int t   = (i * 4) % 256;
        const uint8_t *a = key[seg];
        const uint8_t *b = key[seg + 1];
        uint8_t r = (uint8_t)(a[0] + (b[0] - a[0]) * t / 256);
        uint8_t g = (uint8_t)(a[1] + (b[1] - a[1]) * t / 256);
        uint8_t bl = (uint8_t)(a[2] + (b[2] - a[2]) * t / 256);
        s_iron_lut[i] = rgb565_be(r, g, bl);
    }
    s_iron_lut_built = true;
}

// ---- Thermal frame encoder ----

static SemaphoreHandle_t s_therm_enc_mutex = NULL;
static uint16_t *s_therm_raw = NULL;
static uint8_t  *s_therm_rgb = NULL;
static uint8_t  *s_therm_rot = NULL;   // dest for rotated RGB565 (90/180/270)

// Rotation: 0/1/2/3 → 0/90/180/270 CW. Set via capture_set_thermal_rotation.
static volatile uint8_t s_therm_rotation = 0;

void capture_set_thermal_rotation(uint8_t r) { s_therm_rotation = (uint8_t)(r & 3); }
uint8_t capture_get_thermal_rotation(void)   { return s_therm_rotation; }

static void ensure_therm_buffers(void) {
    if (!s_therm_enc_mutex) s_therm_enc_mutex = xSemaphoreCreateMutex();
    if (!s_iron_lut_built)  build_iron_lut();
    if (!s_therm_raw) {
        s_therm_raw = heap_caps_malloc(LEP_PIXELS * sizeof(uint16_t),
                                        MALLOC_CAP_SPIRAM);
    }
    if (!s_therm_rgb) {
        s_therm_rgb = heap_caps_malloc(LEP_PIXELS * 2, MALLOC_CAP_SPIRAM);
    }
    if (!s_therm_rot) {
        s_therm_rot = heap_caps_malloc(LEP_PIXELS * 2, MALLOC_CAP_SPIRAM);
    }
}

// Rotate an RGB565 buffer (LEP_W × LEP_H) by 90/180/270 CW into dst.
// dst dimensions for 90/270 are LEP_H × LEP_W. Source untouched.
static void rotate_rgb565(const uint16_t *src, uint16_t *dst, uint8_t rot) {
    const int W = LEP_W, H = LEP_H;
    if (rot == 1) {              // 90° CW
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                dst[x * H + (H - 1 - y)] = src[y * W + x];
            }
        }
    } else if (rot == 2) {       // 180°
        for (int i = 0; i < W * H; i++) dst[W * H - 1 - i] = src[i];
    } else if (rot == 3) {       // 270° CW (= 90° CCW)
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                dst[(W - 1 - x) * H + y] = src[y * W + x];
            }
        }
    }
}

size_t capture_encode_thermal_jpeg(uint8_t *dst, size_t cap) {
    ensure_therm_buffers();
    if (!s_therm_raw || !s_therm_rgb || !s_therm_enc_mutex) return 0;

    // Mutex protects the shared raw/rgb staging buffers across callers
    // (preview task + capture command).
    if (xSemaphoreTake(s_therm_enc_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return 0;
    }

    size_t out_len = 0;

    if (!hal_lepton_get_frame(s_therm_raw)) goto done;

    // Auto-range. Skip 0 pixels (uninitialized columns from a partial frame).
    uint16_t mn = 0xFFFF, mx = 0;
    for (int i = 0; i < LEP_PIXELS; i++) {
        uint16_t v = s_therm_raw[i];
        if (v == 0) continue;
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    if (mx <= mn) mx = (uint16_t)(mn + 1);
    uint32_t range = (uint32_t)(mx - mn);

    // raw → palette index → RGB565 BE
    uint16_t *out = (uint16_t *)s_therm_rgb;
    for (int i = 0; i < LEP_PIXELS; i++) {
        uint16_t v = s_therm_raw[i];
        if (v == 0) v = mn;
        else if (v < mn) v = mn;
        else if (v > mx) v = mx;
        uint32_t idx = ((uint32_t)(v - mn) * 255U) / range;
        out[i] = s_iron_lut[idx & 0xFF];
    }

    // Apply rotation (0/90/180/270 CW) before JPEG encode so both
    // preview AND recorded captures pick up the user's choice.
    uint8_t  rot = s_therm_rotation & 3;
    uint8_t *src_buf = s_therm_rgb;
    int      enc_w = LEP_W, enc_h = LEP_H;
    if (rot != 0 && s_therm_rot) {
        rotate_rgb565((const uint16_t *)s_therm_rgb,
                      (uint16_t *)s_therm_rot, rot);
        src_buf = s_therm_rot;
        if (rot == 1 || rot == 3) { enc_w = LEP_H; enc_h = LEP_W; }
    }

    uint8_t *jpg_out = NULL;
    size_t   jpg_len = 0;
    bool ok = fmt2jpg(src_buf, LEP_PIXELS * 2, enc_w, enc_h,
                       PIXFORMAT_RGB565, 80, &jpg_out, &jpg_len);
    if (!ok || !jpg_out) goto done;
    if (jpg_len > cap) {
        free(jpg_out);
        goto done;
    }
    memcpy(dst, jpg_out, jpg_len);
    free(jpg_out);
    out_len = jpg_len;

done:
    xSemaphoreGive(s_therm_enc_mutex);
    return out_len;
}

// ---- Single-shot capture session ----

// Generate a 5-digit pseudo-random session id. Matches Fox's pattern
// (random uint32 → string) loosely; chosen short enough to be readable.
static uint32_t new_session_id(void) {
    uint32_t r = esp_random();
    return 1000 + (r % 99000);   // 1000–99999
}

static esp_err_t write_file_sd_locked(const char *path,
                                       const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "fopen(%s, wb) failed: errno=%d", path, errno);
        return ESP_FAIL;
    }
    size_t wrote = fwrite(data, 1, len, f);
    fclose(f);
    if (wrote != len) {
        ESP_LOGE(TAG, "wrote %u of %u bytes to %s", (unsigned)wrote,
                 (unsigned)len, path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t capture_now(char *session_id_out, size_t session_id_cap,
                      char *msg_out, size_t msg_cap) {
    if (!hal_storage_sd_present()) {
        snprintf(msg_out, msg_cap, "SD card not mounted");
        return ESP_ERR_NOT_FOUND;
    }
    if (!hal_camera_ready()) {
        snprintf(msg_out, msg_cap, "visible camera not ready");
        return ESP_ERR_INVALID_STATE;
    }

    // Capture vis JPEG FIRST (out of SD lock, since hal_camera grabs
    // its own frame buffer lock). Hold the FB lock for as short a time
    // as possible — copy the bytes out and release.
    const uint8_t *vis_jpg = NULL;
    size_t vis_len = 0;
    uint32_t vis_w = 0, vis_h = 0;
    esp_err_t err = hal_camera_grab_jpeg(&vis_jpg, &vis_len, &vis_w, &vis_h);
    if (err != ESP_OK) {
        snprintf(msg_out, msg_cap, "camera grab failed: %s", esp_err_to_name(err));
        return err;
    }

    // Copy vis JPEG out of the camera FB (which we'll release shortly).
    uint8_t *vis_copy = heap_caps_malloc(vis_len, MALLOC_CAP_SPIRAM);
    if (!vis_copy) {
        hal_camera_release();
        snprintf(msg_out, msg_cap, "OOM copying vis JPEG (%u B)", (unsigned)vis_len);
        return ESP_ERR_NO_MEM;
    }
    memcpy(vis_copy, vis_jpg, vis_len);
    hal_camera_release();

    // Encode thermal next (also outside SD lock — it has its own mutex).
    uint8_t *therm_jpg = heap_caps_malloc(THERM_JPEG_MAX_BYTES, MALLOC_CAP_SPIRAM);
    if (!therm_jpg) {
        free(vis_copy);
        snprintf(msg_out, msg_cap, "OOM allocating thermal JPEG buffer");
        return ESP_ERR_NO_MEM;
    }
    size_t therm_len = capture_encode_thermal_jpeg(therm_jpg, THERM_JPEG_MAX_BYTES);
    bool therm_ok = therm_len > 0;
    if (!therm_ok) {
        ESP_LOGW(TAG, "thermal encode skipped — no frame yet");
    }

    // Now hold the SD lock while we mkdir + write all files atomically.
    if (!hal_storage_sd_lock(5000)) {
        free(vis_copy);
        free(therm_jpg);
        snprintf(msg_out, msg_cap, "SD lock timeout");
        return ESP_ERR_TIMEOUT;
    }

    char session_dir[96];
    uint32_t sid = new_session_id();
    snprintf(session_dir, sizeof(session_dir), "%s/session_%lu",
             SESSIONS_BASE_DIR, (unsigned long)sid);

    // Diagnostic: try writing to /sdcard root first to confirm SD writes
    // work AT ALL on this FATFS config. Then try the mkdir for the session.
    {
        FILE *probe = fopen("/sdcard/_ghprobe.txt", "wb");
        if (probe) {
            fprintf(probe, "ok\n");
            fclose(probe);
            ESP_LOGI(TAG, "SD root write probe OK");
        } else {
            ESP_LOGE(TAG, "SD root write probe FAILED: errno=%d", errno);
            hal_storage_sd_unlock();
            free(vis_copy);
            free(therm_jpg);
            snprintf(msg_out, msg_cap, "SD root write failed errno=%d", errno);
            return ESP_FAIL;
        }
    }

    if (hal_storage_sd_mkdir_p(session_dir) != ESP_OK) {
        hal_storage_sd_unlock();
        free(vis_copy);
        free(therm_jpg);
        snprintf(msg_out, msg_cap, "mkdir %s failed", session_dir);
        return ESP_FAIL;
    }

    char path[160];
    esp_err_t vis_write_err = ESP_OK;
    esp_err_t therm_write_err = ESP_OK;

    snprintf(path, sizeof(path), "%s/000001_vis.jpg", session_dir);
    vis_write_err = write_file_sd_locked(path, vis_copy, vis_len);

    if (therm_ok) {
        snprintf(path, sizeof(path), "%s/000001_therm.jpg", session_dir);
        therm_write_err = write_file_sd_locked(path, therm_jpg, therm_len);
    }

    {
        char meta[512];
        int meta_len = snprintf(meta, sizeof(meta),
            "{\"sessionId\":\"session_%lu\","
             "\"intervalSec\":0,"
             "\"captureCount\":1,"
             "\"timestamp\":%lu,"
             "\"captureVis\":true,"
             "\"captureTherm\":%s,"
             "\"mode\":\"single\","
             "\"complete\":true,"
             "\"durationSec\":0}",
            (unsigned long)sid, (unsigned long)time(NULL),
            therm_ok ? "true" : "false");
        snprintf(path, sizeof(path), "%s/session.json", session_dir);
        hal_storage_sd_atomic_write(path, meta, (size_t)meta_len);

        char cap_log[256];
        int cap_log_len = snprintf(cap_log, sizeof(cap_log),
            "{\"seq\":1,"
             "\"sessionMs\":0,"
             "\"visOk\":%s,"
             "\"thermOk\":%s,"
             "\"visBytes\":%u,"
             "\"thermBytes\":%u,"
             "\"timestamp\":%lu}\n",
            vis_write_err == ESP_OK ? "true" : "false",
            therm_write_err == ESP_OK && therm_ok ? "true" : "false",
            (unsigned)vis_len,
            (unsigned)(therm_ok ? therm_len : 0),
            (unsigned long)time(NULL));
        snprintf(path, sizeof(path), "%s/captures.jsonl", session_dir);
        write_file_sd_locked(path, cap_log, (size_t)cap_log_len);
    }

    hal_storage_sd_unlock();
    free(vis_copy);
    free(therm_jpg);

    if (session_id_out && session_id_cap > 0) {
        snprintf(session_id_out, session_id_cap, "session_%lu", (unsigned long)sid);
    }

    if (vis_write_err == ESP_OK) {
        snprintf(msg_out, msg_cap,
                 "session_%lu: vis %u B%s",
                 (unsigned long)sid, (unsigned)vis_len,
                 therm_ok ? (therm_write_err == ESP_OK ? " + therm OK" : " + therm WRITE FAILED")
                          : " (no thermal frame yet)");
        return ESP_OK;
    }
    snprintf(msg_out, msg_cap, "session_%lu: vis WRITE FAILED", (unsigned long)sid);
    return ESP_FAIL;
}

// ---- Timelapse ----
//
// Periodic capture loop. One active session at a time. Captures fire
// from a dedicated FreeRTOS task that reuses the same heap-grab / SD-
// write path as capture_now (so any fix to that path benefits both).
//
// Stop semantics: timelapse_stop sets s_tl_stop, the task wakes, writes
// final session.json with complete=true, and exits cleanly.

static SemaphoreHandle_t s_tl_mutex = NULL;
static volatile bool     s_tl_active = false;
static volatile bool     s_tl_stop   = false;
static TaskHandle_t      s_tl_task   = NULL;
static char              s_tl_session_id[32];
static char              s_tl_session_dir[80];
static volatile uint32_t s_tl_interval_sec = 30;
static volatile uint32_t s_tl_capture_count = 0;
static volatile uint64_t s_tl_started_ms = 0;
static volatile bool     s_tl_capture_vis = true;
static volatile bool     s_tl_capture_therm = true;
// 0 = run until manually stopped. Otherwise the loop exits cleanly
// (writing complete=true to session.json) once elapsed crosses this.
static volatile uint32_t s_tl_max_duration_sec = 0;

static void ensure_tl_mutex(void) {
    if (!s_tl_mutex) s_tl_mutex = xSemaphoreCreateMutex();
}

// Append to captures.jsonl; caller must hold sd_lock.
static void tl_append_capture_log(uint32_t seq, bool vis_ok, size_t vis_len,
                                   bool therm_ok, size_t therm_len) {
    char path[160];
    snprintf(path, sizeof(path), "%s/captures.jsonl", s_tl_session_dir);
    FILE *f = fopen(path, "a");
    if (!f) return;
    fprintf(f,
        "{\"seq\":%lu,"
         "\"sessionMs\":%llu,"
         "\"visOk\":%s,"
         "\"thermOk\":%s,"
         "\"visBytes\":%u,"
         "\"thermBytes\":%u,"
         "\"timestamp\":%lu}\n",
        (unsigned long)seq,
        (unsigned long long)((esp_timer_get_time() / 1000) - s_tl_started_ms),
        vis_ok    ? "true" : "false",
        therm_ok  ? "true" : "false",
        (unsigned)vis_len,
        (unsigned)therm_len,
        (unsigned long)time(NULL));
    fclose(f);
}

// Update session.json with the current count + complete flag. Caller
// must hold sd_lock.
static void tl_write_session_json(bool complete) {
    // s_tl_started_ms is monotonic (esp_timer); we want unix epoch in
    // session.json so the library can sort + render dates. Derive the
    // start epoch by subtracting elapsed monotonic seconds from now.
    // If NTP hasn't synced (time(NULL) ~= 0), this still yields a
    // small but consistent value rather than seconds-since-boot.
    uint64_t now_epoch    = (uint64_t)time(NULL);
    uint64_t elapsed_sec  = ((esp_timer_get_time() / 1000) - s_tl_started_ms) / 1000;
    uint64_t start_epoch  = now_epoch > elapsed_sec ? now_epoch - elapsed_sec : now_epoch;

    char meta[512];
    int meta_len = snprintf(meta, sizeof(meta),
        "{\"sessionId\":\"%s\","
         "\"intervalSec\":%lu,"
         "\"captureCount\":%lu,"
         "\"timestamp\":%llu,"
         "\"captureVis\":%s,"
         "\"captureTherm\":%s,"
         "\"mode\":\"timelapse\","
         "\"complete\":%s,"
         "\"durationSec\":%llu}",
        s_tl_session_id,
        (unsigned long)s_tl_interval_sec,
        (unsigned long)s_tl_capture_count,
        (unsigned long long)start_epoch,
        s_tl_capture_vis ? "true" : "false",
        s_tl_capture_therm ? "true" : "false",
        complete ? "true" : "false",
        (unsigned long long)elapsed_sec);
    char path[160];
    snprintf(path, sizeof(path), "%s/session.json", s_tl_session_dir);
    hal_storage_sd_atomic_write(path, meta, (size_t)meta_len);
}

// One capture iteration: vis + therm to the active timelapse session.
// Mirrors capture_now's logic but writes into the existing session_dir
// with a sequence number instead of creating a new session.
static void tl_capture_iteration(uint32_t seq) {
    if (!hal_camera_ready()) {
        ESP_LOGW(TAG, "tl seq %lu: camera not ready, skipping", (unsigned long)seq);
        return;
    }

    // Grab vis JPEG into a PSRAM copy so we can release the camera FB.
    const uint8_t *vis_jpg = NULL;
    size_t vis_len = 0;
    uint32_t vis_w = 0, vis_h = 0;
    bool vis_grabbed = (hal_camera_grab_jpeg(&vis_jpg, &vis_len, &vis_w, &vis_h) == ESP_OK);
    uint8_t *vis_copy = NULL;
    if (vis_grabbed && s_tl_capture_vis) {
        vis_copy = heap_caps_malloc(vis_len, MALLOC_CAP_SPIRAM);
        if (vis_copy) memcpy(vis_copy, vis_jpg, vis_len);
    }
    if (vis_grabbed) hal_camera_release();

    // Encode thermal.
    uint8_t *therm_jpg = NULL;
    size_t therm_len = 0;
    if (s_tl_capture_therm) {
        therm_jpg = heap_caps_malloc(THERM_JPEG_MAX_BYTES, MALLOC_CAP_SPIRAM);
        if (therm_jpg) {
            therm_len = capture_encode_thermal_jpeg(therm_jpg, THERM_JPEG_MAX_BYTES);
            if (therm_len == 0) { free(therm_jpg); therm_jpg = NULL; }
        }
    }

    if (!hal_storage_sd_lock(5000)) {
        ESP_LOGW(TAG, "tl seq %lu: SD lock timeout", (unsigned long)seq);
        if (vis_copy) free(vis_copy);
        if (therm_jpg) free(therm_jpg);
        return;
    }

    char path[160];
    bool vis_written = false, therm_written = false;
    size_t vis_bytes = 0, therm_bytes = 0;

    if (vis_copy && s_tl_capture_vis) {
        snprintf(path, sizeof(path), "%s/%06lu_vis.jpg",
                 s_tl_session_dir, (unsigned long)seq);
        if (write_file_sd_locked(path, vis_copy, vis_len) == ESP_OK) {
            vis_written = true;
            vis_bytes = vis_len;
        }
    }
    if (therm_jpg && s_tl_capture_therm) {
        snprintf(path, sizeof(path), "%s/%06lu_therm.jpg",
                 s_tl_session_dir, (unsigned long)seq);
        if (write_file_sd_locked(path, therm_jpg, therm_len) == ESP_OK) {
            therm_written = true;
            therm_bytes = therm_len;
        }
    }
    tl_append_capture_log(seq, vis_written, vis_bytes, therm_written, therm_bytes);
    s_tl_capture_count = seq;
    tl_write_session_json(false);   // running, not complete
    hal_storage_sd_unlock();

    if (vis_copy) free(vis_copy);
    if (therm_jpg) free(therm_jpg);

    ESP_LOGI(TAG, "tl seq %lu: vis=%u therm=%u",
             (unsigned long)seq, (unsigned)vis_bytes, (unsigned)therm_bytes);
}

static void tl_task(void *arg) {
    (void)arg;
    uint32_t seq = 1;
    // Fire the first capture immediately, then wait interval_sec between.
    while (!s_tl_stop) {
        tl_capture_iteration(seq);
        seq++;
        // Self-stop on duration cap. Computed AFTER the iteration so the
        // last capture lands inside the window rather than just outside.
        if (s_tl_max_duration_sec > 0) {
            uint64_t elapsed_sec = ((esp_timer_get_time() / 1000) -
                                     s_tl_started_ms) / 1000;
            if (elapsed_sec >= s_tl_max_duration_sec) {
                ESP_LOGI(TAG, "timelapse hit duration cap (%lus) — stopping",
                         (unsigned long)s_tl_max_duration_sec);
                break;
            }
        }
        // Wait in 200 ms slices so stop signal is responsive.
        uint32_t waited_ms = 0;
        uint32_t target_ms = s_tl_interval_sec * 1000;
        while (waited_ms < target_ms && !s_tl_stop) {
            vTaskDelay(pdMS_TO_TICKS(200));
            waited_ms += 200;
        }
    }

    // Finalize session.json with complete=true.
    if (hal_storage_sd_lock(5000)) {
        tl_write_session_json(true);
        hal_storage_sd_unlock();
    }
    s_tl_active = false;
    s_tl_task = NULL;
    ESP_LOGI(TAG, "timelapse task exiting (final count=%lu)",
             (unsigned long)s_tl_capture_count);
    vTaskDelete(NULL);
}

esp_err_t timelapse_start(uint32_t interval_sec,
                          bool capture_vis, bool capture_therm,
                          uint32_t max_duration_sec,
                          char *session_id_out, size_t session_id_cap,
                          char *msg_out, size_t msg_cap) {
    ensure_tl_mutex();
    if (xSemaphoreTake(s_tl_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        snprintf(msg_out, msg_cap, "timelapse mutex busy");
        return ESP_ERR_TIMEOUT;
    }
    if (s_tl_active) {
        xSemaphoreGive(s_tl_mutex);
        snprintf(msg_out, msg_cap, "timelapse already running (%s)", s_tl_session_id);
        return ESP_ERR_INVALID_STATE;
    }
    if (interval_sec < 1) interval_sec = 1;
    if (interval_sec > 3600) interval_sec = 3600;
    s_tl_max_duration_sec = max_duration_sec;   // 0 = unlimited
    if (!capture_vis && !capture_therm) {
        xSemaphoreGive(s_tl_mutex);
        snprintf(msg_out, msg_cap, "must enable at least one of vis/therm");
        return ESP_ERR_INVALID_ARG;
    }
    if (!hal_storage_sd_present()) {
        xSemaphoreGive(s_tl_mutex);
        snprintf(msg_out, msg_cap, "SD card not mounted");
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t sid = new_session_id();
    snprintf(s_tl_session_id, sizeof(s_tl_session_id), "session_%lu",
             (unsigned long)sid);
    snprintf(s_tl_session_dir, sizeof(s_tl_session_dir),
             "%s/%s", SESSIONS_BASE_DIR, s_tl_session_id);

    if (!hal_storage_sd_lock(5000)) {
        xSemaphoreGive(s_tl_mutex);
        snprintf(msg_out, msg_cap, "SD lock timeout");
        return ESP_ERR_TIMEOUT;
    }
    if (hal_storage_sd_mkdir_p(s_tl_session_dir) != ESP_OK) {
        hal_storage_sd_unlock();
        xSemaphoreGive(s_tl_mutex);
        snprintf(msg_out, msg_cap, "mkdir %s failed", s_tl_session_dir);
        return ESP_FAIL;
    }
    s_tl_started_ms = (uint64_t)(esp_timer_get_time() / 1000);
    s_tl_interval_sec = interval_sec;
    s_tl_capture_vis = capture_vis;
    s_tl_capture_therm = capture_therm;
    s_tl_capture_count = 0;
    tl_write_session_json(false);
    hal_storage_sd_unlock();

    if (session_id_out) {
        snprintf(session_id_out, session_id_cap, "%s", s_tl_session_id);
    }

    s_tl_stop = false;
    s_tl_active = true;
    // Pin to core 0 so SPI reader on core 1 isn't displaced by the
    // capture iteration's heap_caps_malloc + JPEG encode + SD write.
    BaseType_t r = xTaskCreatePinnedToCore(tl_task, "tl", 6144, NULL, 5, &s_tl_task, 0);
    if (r != pdPASS) {
        s_tl_active = false;
        xSemaphoreGive(s_tl_mutex);
        snprintf(msg_out, msg_cap, "task create failed");
        return ESP_FAIL;
    }
    xSemaphoreGive(s_tl_mutex);

    snprintf(msg_out, msg_cap, "%s started, interval %lus, vis=%s therm=%s",
             s_tl_session_id, (unsigned long)interval_sec,
             capture_vis ? "on" : "off", capture_therm ? "on" : "off");
    return ESP_OK;
}

esp_err_t timelapse_stop(char *msg_out, size_t msg_cap) {
    ensure_tl_mutex();
    if (xSemaphoreTake(s_tl_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        snprintf(msg_out, msg_cap, "timelapse mutex busy");
        return ESP_ERR_TIMEOUT;
    }
    if (!s_tl_active) {
        xSemaphoreGive(s_tl_mutex);
        snprintf(msg_out, msg_cap, "no active timelapse");
        return ESP_ERR_INVALID_STATE;
    }
    char captured_id[32];
    uint32_t captured_count = s_tl_capture_count;
    snprintf(captured_id, sizeof(captured_id), "%s", s_tl_session_id);
    s_tl_stop = true;
    xSemaphoreGive(s_tl_mutex);

    snprintf(msg_out, msg_cap, "%s stopping (%lu captures)",
             captured_id, (unsigned long)captured_count);
    return ESP_OK;
}

void timelapse_get_status(timelapse_status_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->active = s_tl_active;
    if (s_tl_active) {
        snprintf(out->session_id, sizeof(out->session_id), "%s", s_tl_session_id);
        snprintf(out->session_dir, sizeof(out->session_dir), "%s", s_tl_session_dir);
        out->interval_sec = s_tl_interval_sec;
        out->capture_count = s_tl_capture_count;
        out->started_ms = s_tl_started_ms;
        out->capture_vis = s_tl_capture_vis;
        out->capture_therm = s_tl_capture_therm;
    }
}
