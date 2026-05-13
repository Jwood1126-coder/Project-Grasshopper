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

// Forward decl: scale used to convert raw counts → centi-Kelvin given
// the active TLinear resolution. resolution=0 → 10 cK/count (0.1K),
// resolution=1 → 1 cK/count (0.01K).
static inline int tlinear_scale_x100(uint16_t res) { return (res == 1) ? 1 : 10; }
static inline double raw_to_F_with_scale(uint32_t raw, int scale_x100) {
    double cK = (double)raw * (double)scale_x100;
    double C  = cK / 100.0 - 273.15;
    return C * 9.0 / 5.0 + 32.0;
}

// Forward decls — capture_now (single-shot) appears earlier in this
// file than the timelapse helpers it shares.
typedef struct {
    bool     valid;
    uint32_t min_raw;
    uint32_t max_raw;
    uint32_t center_raw;
    uint16_t resolution;
} therm_stats_t;

static void write_thermal_sidecars(const char *session_dir, uint32_t seq,
                                    const uint16_t *raw_frame,
                                    const therm_stats_t *ts,
                                    bool tlinear_active, bool tlinear_auto_res,
                                    uint8_t therm_rotation,
                                    uint32_t last_ffc_ms,
                                    bool agc_enabled, int gain_mode);

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

// Last-frame thermal stats from raw Lepton pixels. Updated each call to
// capture_encode_thermal_jpeg. With TLinear=1 these are scaled Kelvin
// values (resolution 0.01K or 0.1K depending on Lepton AUTO_RESOLUTION
// choice — caller queries lepton_cci_get_tlinear_state for scale).
static volatile uint16_t s_last_min_raw    = 0;
static volatile uint16_t s_last_max_raw    = 0;
static volatile uint16_t s_last_center_raw = 0;
static volatile bool     s_last_temps_valid = false;

void capture_get_last_thermal_temps_ck(uint32_t *min_ck, uint32_t *max_ck,
                                        uint32_t *center_ck) {
    // Caller's "ck" naming assumes 0.01K — we report whatever raw scale
    // the Lepton produced; the unit is encoded in the tlinear state
    // exposed alongside this in tick metadata. Caller is responsible
    // for multiplying by 10 if resolution=0 (0.1K).
    if (min_ck)    *min_ck    = s_last_temps_valid ? s_last_min_raw    : 0;
    if (max_ck)    *max_ck    = s_last_temps_valid ? s_last_max_raw    : 0;
    if (center_ck) *center_ck = s_last_temps_valid ? s_last_center_raw : 0;
}

bool capture_snapshot_thermal_raw(uint16_t *dst, size_t dst_bytes) {
    if (!dst || dst_bytes < (size_t)(LEP_PIXELS * sizeof(uint16_t))) return false;
    if (!s_therm_raw || !s_therm_enc_mutex) return false;
    if (!s_last_temps_valid) return false;
    if (xSemaphoreTake(s_therm_enc_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) return false;
    memcpy(dst, s_therm_raw, LEP_PIXELS * sizeof(uint16_t));
    xSemaphoreGive(s_therm_enc_mutex);
    return true;
}

void capture_set_thermal_rotation(uint8_t r) { s_therm_rotation = (uint8_t)(r & 3); }
uint8_t capture_get_thermal_rotation(void)   { return s_therm_rotation; }

// Visible rotation. 0 and 180 are free (sensor hflip+vflip); 90/270
// trigger SW decode→rotate→re-encode in apply_visible_rotation().
static volatile uint8_t s_vis_rotation = 0;

void capture_set_visible_rotation(uint8_t r) {
    r = (uint8_t)(r & 3);
    s_vis_rotation = r;
    // Map rotation to sensor flips. 180° is the only state we can do
    // entirely in hardware; 0/90/270 leave the sensor un-flipped and
    // (for 90/270) defer rotation to the software path.
    bool h = (r == 2);
    bool v = (r == 2);
    hal_camera_set_hmirror(h);
    hal_camera_set_vflip(v);
}
uint8_t capture_get_visible_rotation(void) { return s_vis_rotation; }

// Rotate an RGB888 buffer (W × H) 90 or 270 CW into dst (which must be
// H × W). Tightly packed, 3 bytes per pixel.
static void rotate_rgb888(const uint8_t *src, uint8_t *dst,
                          uint32_t W, uint32_t H, uint8_t rot) {
    if (rot == 1) {              // 90° CW
        for (uint32_t y = 0; y < H; y++) {
            for (uint32_t x = 0; x < W; x++) {
                const uint8_t *s = src + (y * W + x) * 3;
                uint8_t *d = dst + (x * H + (H - 1 - y)) * 3;
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
            }
        }
    } else if (rot == 3) {       // 270° CW
        for (uint32_t y = 0; y < H; y++) {
            for (uint32_t x = 0; x < W; x++) {
                const uint8_t *s = src + (y * W + x) * 3;
                uint8_t *d = dst + ((W - 1 - x) * H + y) * 3;
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
            }
        }
    }
}

bool capture_rotate_visible_jpeg_if_needed(const uint8_t *in_jpg, size_t in_len,
                                            uint32_t in_w, uint32_t in_h,
                                            uint8_t **out_jpg, size_t *out_len,
                                            uint32_t *out_w, uint32_t *out_h) {
    uint8_t r = s_vis_rotation;
    if (r != 1 && r != 3) return false;
    if (!in_jpg || in_len == 0 || in_w == 0 || in_h == 0) return false;

    size_t pix_len = (size_t)in_w * in_h * 3;
    uint8_t *rgb = heap_caps_malloc(pix_len, MALLOC_CAP_SPIRAM);
    if (!rgb) return false;

    if (!fmt2rgb888(in_jpg, in_len, PIXFORMAT_JPEG, rgb)) {
        free(rgb);
        return false;
    }
    uint8_t *rot = heap_caps_malloc(pix_len, MALLOC_CAP_SPIRAM);
    if (!rot) { free(rgb); return false; }

    rotate_rgb888(rgb, rot, in_w, in_h, r);
    free(rgb);

    uint8_t *jpg = NULL; size_t jpg_len = 0;
    bool ok = fmt2jpg(rot, pix_len, in_h, in_w, PIXFORMAT_RGB888, 80,
                       &jpg, &jpg_len);
    free(rot);
    if (!ok || !jpg) return false;

    *out_jpg = jpg;
    *out_len = jpg_len;
    *out_w = in_h;          // dims swapped
    *out_h = in_w;
    return true;
}

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

size_t capture_encode_thermal_jpeg_atomic(uint8_t *dst, size_t cap,
                                           therm_frame_stats_t *stats_out,
                                           uint16_t *raw_out, size_t raw_out_bytes) {
    ensure_therm_buffers();
    if (!s_therm_raw || !s_therm_rgb || !s_therm_enc_mutex) return 0;
    if (stats_out) memset(stats_out, 0, sizeof(*stats_out));

    // Mutex protects the shared raw/rgb staging buffers across callers
    // (preview task + capture command). Held across encode + raw copy
    // + stats fill so the three outputs all describe the same frame.
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

    // Stash stats for tick payload. Center pixel is taken before any
    // rotation since rotation only affects the encoded output.
    uint32_t ce = s_therm_raw[(LEP_H / 2) * LEP_W + (LEP_W / 2)];
    s_last_min_raw    = mn;
    s_last_max_raw    = mx;
    s_last_center_raw = ce;
    s_last_temps_valid = true;

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

    // Snapshot the raw frame + stats for the caller while we still
    // hold the encoder mutex. After release another encode could
    // overwrite s_therm_raw in microseconds.
    if (raw_out && raw_out_bytes >= LEP_PIXELS * sizeof(uint16_t)) {
        memcpy(raw_out, s_therm_raw, LEP_PIXELS * sizeof(uint16_t));
    }
    if (stats_out) {
        bool tl_active = false, tl_auto = false; uint16_t res = 0;
        lepton_cci_get_tlinear_state(&tl_active, &tl_auto, &res);
        stats_out->min_raw    = mn;
        stats_out->max_raw    = mx;
        stats_out->center_raw = ce;
        stats_out->resolution = res;
        stats_out->valid      = (tl_active && mx > 0);
    }

done:
    xSemaphoreGive(s_therm_enc_mutex);
    return out_len;
}

size_t capture_encode_thermal_jpeg(uint8_t *dst, size_t cap) {
    return capture_encode_thermal_jpeg_atomic(dst, cap, NULL, NULL, 0);
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

    // For 90/270 visible rotation, decode → rotate → re-encode here so
    // recordings save in the chosen orientation.
    uint8_t *vis_copy = NULL;
    {
        uint8_t *rot_jpg = NULL; size_t rot_len = 0;
        uint32_t rw = vis_w, rh = vis_h;
        if (capture_rotate_visible_jpeg_if_needed(vis_jpg, vis_len, vis_w, vis_h,
                                                   &rot_jpg, &rot_len, &rw, &rh)) {
            vis_copy = rot_jpg;     // already heap-allocated; we own it
            vis_len  = rot_len;
            vis_w    = rw;
            vis_h    = rh;
        } else {
            vis_copy = heap_caps_malloc(vis_len, MALLOC_CAP_SPIRAM);
            if (!vis_copy) {
                hal_camera_release();
                snprintf(msg_out, msg_cap, "OOM copying vis JPEG (%u B)", (unsigned)vis_len);
                return ESP_ERR_NO_MEM;
            }
            memcpy(vis_copy, vis_jpg, vis_len);
        }
    }
    hal_camera_release();

    // Encode thermal next (also outside SD lock — it has its own mutex).
    uint8_t *therm_jpg = heap_caps_malloc(THERM_JPEG_MAX_BYTES, MALLOC_CAP_SPIRAM);
    if (!therm_jpg) {
        free(vis_copy);
        snprintf(msg_out, msg_cap, "OOM allocating thermal JPEG buffer");
        return ESP_ERR_NO_MEM;
    }
    // Atomic encode + raw snapshot + stats — same frame, one mutex
    // acquire, no race with the preview task's parallel encodes.
    therm_frame_stats_t fs = {0};
    uint16_t *raw_snap = heap_caps_malloc(LEP_PIXELS * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    size_t therm_len = capture_encode_thermal_jpeg_atomic(
        therm_jpg, THERM_JPEG_MAX_BYTES, &fs,
        raw_snap, raw_snap ? LEP_PIXELS * sizeof(uint16_t) : 0);
    bool therm_ok = therm_len > 0;
    therm_stats_t ts = {0};
    bool tl_active = false, tl_auto = false; uint16_t tl_res = 0;
    if (therm_ok) {
        ts.min_raw = fs.min_raw; ts.max_raw = fs.max_raw;
        ts.center_raw = fs.center_raw; ts.resolution = fs.resolution;
        ts.valid = fs.valid;
        // For the sidecar JSON we still want the live tlinear state
        // booleans (active/auto_res). Resolution comes from `fs`.
        lepton_cci_get_tlinear_state(&tl_active, &tl_auto, &tl_res);
    } else {
        ESP_LOGW(TAG, "thermal encode skipped — no frame yet");
    }
    if (!ts.valid && raw_snap) { free(raw_snap); raw_snap = NULL; }

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

    // Sidecars: .raw16 + .json — only when thermal succeeded + radiometric on.
    if (therm_ok && ts.valid) {
        write_thermal_sidecars(session_dir, /*seq*/ 1, raw_snap, &ts,
                                tl_active, tl_auto,
                                capture_get_thermal_rotation(),
                                /*last_ffc_ms*/ 0,
                                hal_lepton_agc_enabled(),
                                hal_lepton_gain_mode());
    }

    {
        char meta[768];
        int meta_len = snprintf(meta, sizeof(meta),
            "{\"sessionId\":\"session_%lu\","
             "\"intervalSec\":0,"
             "\"captureCount\":1,"
             "\"timestamp\":%lu,"
             "\"captureVis\":true,"
             "\"captureTherm\":%s,"
             "\"mode\":\"single\","
             "\"complete\":true,"
             "\"durationSec\":0",
            (unsigned long)sid, (unsigned long)time(NULL),
            therm_ok ? "true" : "false");
        if (ts.valid) {
            int sx = tlinear_scale_x100(ts.resolution);
            int n = snprintf(meta + meta_len, sizeof(meta) - meta_len,
                ",\"tempStats\":{"
                  "\"minRaw\":%lu,\"maxRaw\":%lu,\"avgCenterRaw\":%lu,"
                  "\"tlinearResolution\":%u,"
                  "\"minTempF\":%.2f,\"maxTempF\":%.2f,\"avgCenterTempF\":%.2f"
                "}",
                (unsigned long)ts.min_raw,
                (unsigned long)ts.max_raw,
                (unsigned long)ts.center_raw,
                (unsigned)ts.resolution,
                raw_to_F_with_scale(ts.min_raw,    sx),
                raw_to_F_with_scale(ts.max_raw,    sx),
                raw_to_F_with_scale(ts.center_raw, sx));
            if (n > 0 && meta_len + n < (int)sizeof(meta)) meta_len += n;
        }
        if (meta_len + 2 < (int)sizeof(meta)) {
            meta[meta_len++] = '}';
            meta[meta_len]   = '\0';
        }
        snprintf(path, sizeof(path), "%s/session.json", session_dir);
        hal_storage_sd_atomic_write(path, meta, (size_t)meta_len);

        // captures.jsonl: hand-build a row with the new temp fields.
        char cap_log[384];
        int cap_log_len = snprintf(cap_log, sizeof(cap_log),
            "{\"seq\":1,"
             "\"sessionMs\":0,"
             "\"visOk\":%s,"
             "\"thermOk\":%s,"
             "\"visBytes\":%u,"
             "\"thermBytes\":%u,"
             "\"timestamp\":%lu",
            vis_write_err == ESP_OK ? "true" : "false",
            therm_write_err == ESP_OK && therm_ok ? "true" : "false",
            (unsigned)vis_len,
            (unsigned)(therm_ok ? therm_len : 0),
            (unsigned long)time(NULL));
        if (ts.valid) {
            int sx = tlinear_scale_x100(ts.resolution);
            int n = snprintf(cap_log + cap_log_len, sizeof(cap_log) - cap_log_len,
                ",\"minRaw\":%lu,\"maxRaw\":%lu,\"centerRaw\":%lu,"
                "\"tlinearResolution\":%u,"
                "\"minTempF\":%.2f,\"maxTempF\":%.2f,\"centerTempF\":%.2f",
                (unsigned long)ts.min_raw,
                (unsigned long)ts.max_raw,
                (unsigned long)ts.center_raw,
                (unsigned)ts.resolution,
                raw_to_F_with_scale(ts.min_raw,    sx),
                raw_to_F_with_scale(ts.max_raw,    sx),
                raw_to_F_with_scale(ts.center_raw, sx));
            if (n > 0 && cap_log_len + n < (int)sizeof(cap_log)) cap_log_len += n;
        }
        if (cap_log_len + 3 < (int)sizeof(cap_log)) {
            cap_log[cap_log_len++] = '}';
            cap_log[cap_log_len++] = '\n';
            cap_log[cap_log_len]   = '\0';
        }
        snprintf(path, sizeof(path), "%s/captures.jsonl", session_dir);
        write_file_sd_locked(path, cap_log, (size_t)cap_log_len);
    }

    hal_storage_sd_unlock();
    free(vis_copy);
    free(therm_jpg);
    if (raw_snap) free(raw_snap);

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
// Per-session radiometric aggregates (raw Lepton counts).
// minSession = min over all captures' minRaw; maxSession = max over
// all captures' maxRaw; sumCenter / validThermCount → mean center temp.
// validThermCount is a separate denominator so failed thermal captures
// don't bias the average toward 0 (codex #4).
static volatile uint32_t s_tl_min_session         = 0xFFFFFFFFu;
static volatile uint32_t s_tl_max_session         = 0;
static volatile uint64_t s_tl_sum_center_raw      = 0;
static volatile uint32_t s_tl_valid_therm_count   = 0;

static void ensure_tl_mutex(void) {
    if (!s_tl_mutex) s_tl_mutex = xSemaphoreCreateMutex();
}

// Append to captures.jsonl; caller must hold sd_lock.
// therm_stats_t is forward-declared near the top of this file so
// capture_now (which precedes the timelapse helpers) can reference it.

static void tl_append_capture_log(uint32_t seq, bool vis_ok, size_t vis_len,
                                   bool therm_ok, size_t therm_len,
                                   const therm_stats_t *ts) {
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
         "\"timestamp\":%lu",
        (unsigned long)seq,
        (unsigned long long)((esp_timer_get_time() / 1000) - s_tl_started_ms),
        vis_ok    ? "true" : "false",
        therm_ok  ? "true" : "false",
        (unsigned)vis_len,
        (unsigned)therm_len,
        (unsigned long)time(NULL));
    if (ts && ts->valid) {
        int sx = tlinear_scale_x100(ts->resolution);
        fprintf(f,
            ",\"minRaw\":%lu,\"maxRaw\":%lu,\"centerRaw\":%lu,"
            "\"tlinearResolution\":%u,"
            "\"minTempF\":%.2f,\"maxTempF\":%.2f,\"centerTempF\":%.2f",
            (unsigned long)ts->min_raw,
            (unsigned long)ts->max_raw,
            (unsigned long)ts->center_raw,
            (unsigned)ts->resolution,
            raw_to_F_with_scale(ts->min_raw,    sx),
            raw_to_F_with_scale(ts->max_raw,    sx),
            raw_to_F_with_scale(ts->center_raw, sx));
    }
    fputs("}\n", f);
    fclose(f);
}

// Write the .raw16 and .json sidecars for one thermal capture.
// .raw16 is 38400 bytes of pre-rotation native-orientation pixels —
// this is the scientifically useful artifact, lossless and complete
// (the .jpg loses bits to palette mapping + rotation + JPEG quant).
// .json captures the metadata needed to interpret the .raw16 later.
// Caller holds sd_lock.
static void write_thermal_sidecars(const char *session_dir, uint32_t seq,
                                    const uint16_t *raw_frame,
                                    const therm_stats_t *ts,
                                    bool tlinear_active, bool tlinear_auto_res,
                                    uint8_t therm_rotation,
                                    uint32_t last_ffc_ms,
                                    bool agc_enabled, int gain_mode) {
    char path[200];
    if (raw_frame) {
        snprintf(path, sizeof(path), "%s/%06lu_therm.raw16",
                 session_dir, (unsigned long)seq);
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(raw_frame, sizeof(uint16_t), LEP_PIXELS, f);
            fclose(f);
        }
    }
    snprintf(path, sizeof(path), "%s/%06lu_therm.json",
             session_dir, (unsigned long)seq);
    FILE *f = fopen(path, "w");
    if (!f) return;
    int sx = ts ? tlinear_scale_x100(ts->resolution) : 1;
    fprintf(f,
        "{\"seq\":%lu,"
         "\"timestamp\":%lu,"
         "\"rawDims\":{\"w\":%d,\"h\":%d},"
         "\"thermRotation\":%u,"
         "\"tlinearActive\":%s,"
         "\"tlinearAutoRes\":%s,"
         "\"tlinearResolution\":%u,"
         "\"agcEnabled\":%s,"
         "\"gainMode\":%d,"
         "\"lastFFCMs\":%lu",
        (unsigned long)seq,
        (unsigned long)time(NULL),
        LEP_W, LEP_H,
        (unsigned)therm_rotation,
        tlinear_active   ? "true" : "false",
        tlinear_auto_res ? "true" : "false",
        ts ? (unsigned)ts->resolution : 0u,
        agc_enabled ? "true" : "false",
        gain_mode,
        (unsigned long)last_ffc_ms);
    if (ts && ts->valid) {
        fprintf(f,
            ",\"minRaw\":%lu,\"maxRaw\":%lu,\"centerRaw\":%lu,"
            "\"minTempF\":%.2f,\"maxTempF\":%.2f,\"centerTempF\":%.2f",
            (unsigned long)ts->min_raw,
            (unsigned long)ts->max_raw,
            (unsigned long)ts->center_raw,
            raw_to_F_with_scale(ts->min_raw,    sx),
            raw_to_F_with_scale(ts->max_raw,    sx),
            raw_to_F_with_scale(ts->center_raw, sx));
    }
    fputs("}\n", f);
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

    char meta[768];
    int meta_len = snprintf(meta, sizeof(meta),
        "{\"sessionId\":\"%s\","
         "\"intervalSec\":%lu,"
         "\"captureCount\":%lu,"
         "\"timestamp\":%llu,"
         "\"captureVis\":%s,"
         "\"captureTherm\":%s,"
         "\"mode\":\"timelapse\","
         "\"complete\":%s,"
         "\"durationSec\":%llu",
        s_tl_session_id,
        (unsigned long)s_tl_interval_sec,
        (unsigned long)s_tl_capture_count,
        (unsigned long long)start_epoch,
        s_tl_capture_vis ? "true" : "false",
        s_tl_capture_therm ? "true" : "false",
        complete ? "true" : "false",
        (unsigned long long)elapsed_sec);

    // Aggregated radiometric stats across the session, when we have
    // at least one valid radiometric thermal capture. Avg uses the
    // valid count, not the raw capture count, so a partial-failure
    // session doesn't bias the mean toward 0 (codex #4).
    if (s_tl_valid_therm_count > 0 && s_tl_max_session > 0 &&
        s_tl_min_session != 0xFFFFFFFFu) {
        bool tl_active = false, tl_auto = false; uint16_t res = 0;
        lepton_cci_get_tlinear_state(&tl_active, &tl_auto, &res);
        int sx = tlinear_scale_x100(res);
        uint64_t avg_ctr = s_tl_sum_center_raw / s_tl_valid_therm_count;
        int n = snprintf(meta + meta_len, sizeof(meta) - meta_len,
            ",\"tempStats\":{"
              "\"minRaw\":%lu,\"maxRaw\":%lu,\"avgCenterRaw\":%llu,"
              "\"tlinearResolution\":%u,"
              "\"validThermCount\":%lu,"
              "\"minTempF\":%.2f,\"maxTempF\":%.2f,\"avgCenterTempF\":%.2f"
            "}",
            (unsigned long)s_tl_min_session,
            (unsigned long)s_tl_max_session,
            (unsigned long long)avg_ctr,
            (unsigned)res,
            (unsigned long)s_tl_valid_therm_count,
            raw_to_F_with_scale(s_tl_min_session,    sx),
            raw_to_F_with_scale(s_tl_max_session,    sx),
            raw_to_F_with_scale((uint32_t)avg_ctr,   sx));
        if (n > 0 && meta_len + n < (int)sizeof(meta)) meta_len += n;
    }
    if (meta_len + 2 < (int)sizeof(meta)) {
        meta[meta_len++] = '}';
        meta[meta_len]   = '\0';
    }

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
    // SW rotation (90°/270°) happens here so each timelapse capture is
    // saved in the user-selected orientation.
    const uint8_t *vis_jpg = NULL;
    size_t vis_len = 0;
    uint32_t vis_w = 0, vis_h = 0;
    bool vis_grabbed = (hal_camera_grab_jpeg(&vis_jpg, &vis_len, &vis_w, &vis_h) == ESP_OK);
    uint8_t *vis_copy = NULL;
    if (vis_grabbed && s_tl_capture_vis) {
        uint8_t *rot_jpg = NULL; size_t rot_len = 0;
        uint32_t rw = vis_w, rh = vis_h;
        if (capture_rotate_visible_jpeg_if_needed(vis_jpg, vis_len, vis_w, vis_h,
                                                   &rot_jpg, &rot_len, &rw, &rh)) {
            vis_copy = rot_jpg;
            vis_len  = rot_len;
        } else {
            vis_copy = heap_caps_malloc(vis_len, MALLOC_CAP_SPIRAM);
            if (vis_copy) memcpy(vis_copy, vis_jpg, vis_len);
        }
    }
    if (vis_grabbed) hal_camera_release();

    // Encode thermal + snapshot raw + stats atomically (same frame,
    // one mutex acquire). Eliminates the race where a preview encode
    // between encode + snapshot could swap s_therm_raw underneath us.
    uint8_t *therm_jpg = NULL;
    size_t therm_len = 0;
    therm_stats_t ts = {0};
    uint16_t *raw_snap = NULL;
    if (s_tl_capture_therm) {
        therm_jpg = heap_caps_malloc(THERM_JPEG_MAX_BYTES, MALLOC_CAP_SPIRAM);
        raw_snap  = heap_caps_malloc(LEP_PIXELS * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (therm_jpg) {
            therm_frame_stats_t fs = {0};
            therm_len = capture_encode_thermal_jpeg_atomic(
                therm_jpg, THERM_JPEG_MAX_BYTES, &fs,
                raw_snap, raw_snap ? LEP_PIXELS * sizeof(uint16_t) : 0);
            if (therm_len == 0) { free(therm_jpg); therm_jpg = NULL; }
            ts.min_raw = fs.min_raw; ts.max_raw = fs.max_raw;
            ts.center_raw = fs.center_raw; ts.resolution = fs.resolution;
            ts.valid = fs.valid;
        }
        if (!ts.valid) {
            if (raw_snap) { free(raw_snap); raw_snap = NULL; }
        } else {
            // Fold into session aggregates. validThermCount tracks the
            // denominator separately so a partial-failure session
            // doesn't bias avgCenterRaw down (codex #4).
            if (ts.min_raw < s_tl_min_session) s_tl_min_session = ts.min_raw;
            if (ts.max_raw > s_tl_max_session) s_tl_max_session = ts.max_raw;
            s_tl_sum_center_raw  += ts.center_raw;
            s_tl_valid_therm_count++;
        }
    }

    if (!hal_storage_sd_lock(5000)) {
        ESP_LOGW(TAG, "tl seq %lu: SD lock timeout", (unsigned long)seq);
        if (vis_copy) free(vis_copy);
        if (therm_jpg) free(therm_jpg);
        if (raw_snap) free(raw_snap);
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

    // Per-capture sidecars (.raw16 + .json) — only when thermal was
    // actually requested + radiometric is on. Caller already holds lock.
    if (s_tl_capture_therm && ts.valid) {
        bool tl_active = false, tl_auto = false; uint16_t res = 0;
        lepton_cci_get_tlinear_state(&tl_active, &tl_auto, &res);
        write_thermal_sidecars(s_tl_session_dir, seq, raw_snap, &ts,
                                tl_active, tl_auto,
                                capture_get_thermal_rotation(),
                                /*last_ffc_ms*/ 0,    // tracked by hal_lepton stats; not exposed yet
                                hal_lepton_agc_enabled(),
                                hal_lepton_gain_mode());
    }
    tl_append_capture_log(seq, vis_written, vis_bytes, therm_written, therm_bytes,
                           ts.valid ? &ts : NULL);
    s_tl_capture_count = seq;
    tl_write_session_json(false);   // running, not complete
    hal_storage_sd_unlock();

    if (vis_copy) free(vis_copy);
    if (therm_jpg) free(therm_jpg);
    if (raw_snap)  free(raw_snap);

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
    // Reset radiometric aggregates for this session.
    s_tl_min_session       = 0xFFFFFFFFu;
    s_tl_max_session       = 0;
    s_tl_sum_center_raw    = 0;
    s_tl_valid_therm_count = 0;
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
