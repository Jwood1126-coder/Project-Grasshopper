// Capture — single-shot vis+therm JPEG to SD session directory.
//
// See include/capture.h for the SD layout. Thermal encoder lives here
// because it's shared with the periodic preview task in app_main.c.
//
// Phase 3 minimum: capture_now() writes a brand-new session of one.
// Timelapse (start/stop, periodic capture loop) builds on this.

#include "capture.h"

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
#include "session_store.h"
#include "system_phase.h"

static const char *TAG = "capture";

#define LEP_W       160
#define LEP_H       120
#define LEP_PIXELS  (LEP_W * LEP_H)
#define THERM_JPEG_MAX_BYTES   65536

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

// ---- Capture engine ----
//
// Produces in-memory artifacts only. No SD, no journal, no aggregate
// state. tl_capture_iteration and capture_now compose this with their
// own session-store logic; the deep-sleep wake handler will too.

void capture_artifacts_free(capture_artifacts_t *a) {
    if (!a) return;
    if (a->vis_jpg)   free(a->vis_jpg);
    if (a->therm_jpg) free(a->therm_jpg);
    if (a->therm_raw) free(a->therm_raw);
    memset(a, 0, sizeof(*a));
}

esp_err_t capture_engine_take_one(bool want_visible,
                                   bool want_thermal,
                                   bool want_thermal_raw,
                                   capture_artifacts_t *out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!want_visible && !want_thermal) return ESP_ERR_INVALID_ARG;

    // Snapshot capture-time settings so the sidecar JSON describes the
    // exact frame, not whatever the live system rotated to by commit.
    out->vis_rotation   = s_vis_rotation;
    out->therm_rotation = s_therm_rotation;
    out->agc_enabled    = hal_lepton_agc_enabled();
    out->gain_mode      = hal_lepton_gain_mode();
    {
        bool ta = false, ar = false; uint16_t res = 0;
        lepton_cci_get_tlinear_state(&ta, &ar, &res);
        out->tlinear_active   = ta;
        out->tlinear_auto_res = ar;
    }

    bool any = false;

    // ---- Visible JPEG (with optional 90/270 SW rotation) ----
    if (want_visible) {
        uint64_t t0 = esp_timer_get_time();
        if (hal_camera_ready()) {
            const uint8_t *vis_fb = NULL;
            size_t   vis_len_fb = 0;
            uint32_t vis_w = 0, vis_h = 0;
            if (hal_camera_grab_jpeg(&vis_fb, &vis_len_fb, &vis_w, &vis_h) == ESP_OK) {
                uint8_t *rot_jpg = NULL; size_t rot_len = 0;
                uint32_t rw = vis_w, rh = vis_h;
                if (capture_rotate_visible_jpeg_if_needed(vis_fb, vis_len_fb,
                                                           vis_w, vis_h,
                                                           &rot_jpg, &rot_len,
                                                           &rw, &rh)) {
                    out->vis_jpg = rot_jpg;
                    out->vis_len = rot_len;
                    out->vis_w   = rw;
                    out->vis_h   = rh;
                } else {
                    uint8_t *copy = heap_caps_malloc(vis_len_fb, MALLOC_CAP_SPIRAM);
                    if (copy) {
                        memcpy(copy, vis_fb, vis_len_fb);
                        out->vis_jpg = copy;
                        out->vis_len = vis_len_fb;
                        out->vis_w   = vis_w;
                        out->vis_h   = vis_h;
                    }
                }
                hal_camera_release();
                if (out->vis_jpg) any = true;
            } else {
                ESP_LOGW(TAG, "capture_engine: camera grab failed");
            }
        } else {
            ESP_LOGW(TAG, "capture_engine: camera not ready");
        }
        out->visible_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    }

    // ---- Thermal JPEG + optional raw snapshot (atomic, single mutex) ----
    if (want_thermal) {
        uint64_t t0 = esp_timer_get_time();
        uint8_t *therm_jpg = heap_caps_malloc(THERM_JPEG_MAX_BYTES, MALLOC_CAP_SPIRAM);
        uint16_t *raw_snap = NULL;
        if (want_thermal_raw) {
            raw_snap = heap_caps_malloc(LEP_PIXELS * sizeof(uint16_t),
                                         MALLOC_CAP_SPIRAM);
        }
        if (therm_jpg) {
            therm_frame_stats_t fs = {0};
            size_t therm_len = capture_encode_thermal_jpeg_atomic(
                therm_jpg, THERM_JPEG_MAX_BYTES, &fs,
                raw_snap, raw_snap ? LEP_PIXELS * sizeof(uint16_t) : 0);
            if (therm_len > 0) {
                out->therm_jpg   = therm_jpg;
                out->therm_len   = therm_len;
                out->therm_stats = fs;
                if (fs.valid && raw_snap) {
                    out->therm_raw = raw_snap;
                    raw_snap = NULL;
                }
                any = true;
            } else {
                free(therm_jpg);
                ESP_LOGW(TAG, "capture_engine: thermal encode skipped (no frame yet)");
            }
        }
        if (raw_snap) free(raw_snap);
        out->thermal_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    }

    return any ? ESP_OK : ESP_FAIL;
}

// ---- Single-shot capture session ----

// Generate a 5-digit pseudo-random session id. Matches Fox's pattern
// (random uint32 → string) loosely; chosen short enough to be readable.
static uint32_t new_session_id(void) {
    uint32_t r = esp_random();
    return 1000 + (r % 99000);   // 1000–99999
}

// MAINTAINER NOTE — read before adding any code path here:
//
// This function calls system_phase_enter(PHASE_CAPTURE) midway through.
// Every exit MUST go through the `out:` label so phase returns to LIVE.
// A bare `return` after the phase enter() leaves preview tasks paused
// forever and the dashboard's chip stuck on "CAPTURE" until the next
// phase transition (which may never come). This is exactly the kind of
// silent-failure footgun the system_phase design was built to avoid.
//
// If you need an early return after the CAPTURE enter, use:
//
//     if (something_failed) { rc = ESP_FAIL; goto out; }
//
// The pre-enter validation block (SD present, camera ready) is the only
// place bare returns are safe — phase has not been entered yet.
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
    // Live-mode capture path: explicit, one-directional. Always exits
    // back to LIVE — capture_now is only called from the cmd handler
    // in live context, never nested inside DEEP_SLEEP_CAPTURE.
    system_phase_enter(PHASE_CAPTURE);

    // Produce vis + therm artifacts in memory (engine is shared with
    // the live timelapse path and the deep-sleep wake handler).
    capture_artifacts_t art = {0};
    esp_err_t cap_err = capture_engine_take_one(/*want_visible*/ true,
                                                  /*want_thermal*/ true,
                                                  /*want_thermal_raw*/ true,
                                                  &art);
    esp_err_t rc;
    if (cap_err != ESP_OK || !art.vis_jpg) {
        capture_artifacts_free(&art);
        snprintf(msg_out, msg_cap, "camera grab failed");
        rc = ESP_FAIL;
        goto out;
    }
    bool therm_ok = (art.therm_jpg != NULL);
    size_t vis_bytes = art.vis_len;          // capture before free()
    if (!therm_ok) ESP_LOGW(TAG, "thermal encode skipped — no frame yet");

    // Open a single-shot session, commit one frame, close.
    char session_id[32];
    snprintf(session_id, sizeof(session_id), "session_%lu",
             (unsigned long)new_session_id());
    session_store_open_args_t args = {
        .mode         = "single",
        .interval_sec = 0,
        .capture_vis  = true,
        .capture_therm = true,
    };
    session_store_handle_t *h = session_store_open(session_id, &args);
    if (!h) {
        capture_artifacts_free(&art);
        snprintf(msg_out, msg_cap, "%s: session_store_open failed", session_id);
        rc = ESP_FAIL;
        goto out;
    }
    esp_err_t commit_err = session_store_commit(h, &art, /*meta*/ NULL);
    session_store_close(h);
    capture_artifacts_free(&art);

    if (session_id_out && session_id_cap > 0) {
        snprintf(session_id_out, session_id_cap, "%s", session_id);
    }
    if (commit_err == ESP_OK) {
        snprintf(msg_out, msg_cap,
                 "%s: vis %u B%s",
                 session_id, (unsigned)vis_bytes,
                 therm_ok ? " + therm OK" : " (no thermal frame yet)");
        rc = ESP_OK;
        goto out;
    }
    snprintf(msg_out, msg_cap, "%s: commit failed", session_id);
    rc = ESP_FAIL;
out:
    // Single exit point so every error path returns CAPTURE → LIVE
    // explicitly. NEVER use save/restore semantics — capture_now is
    // only called from a LIVE context, period.
    system_phase_enter(PHASE_LIVE);
    return rc;
}

// ---- Timelapse ----
//
// Periodic capture loop on a dedicated FreeRTOS task. One active session
// at a time. All durable bookkeeping (mkdir, atomic file writes, journal
// append, session.json rewrite, finalize) lives in session_store; this
// module just owns the loop, the stop signal, and the capture_engine
// call path.
//
// Stop semantics: timelapse_stop sets s_tl_stop, the task wakes up,
// session_store_close() finalizes the session.json with complete=true,
// the task exits.

static SemaphoreHandle_t        s_tl_mutex = NULL;
static volatile bool            s_tl_active = false;
static volatile bool            s_tl_stop   = false;
static TaskHandle_t             s_tl_task   = NULL;
static session_store_handle_t  *s_tl_handle = NULL;
static volatile uint32_t        s_tl_interval_sec = 30;
static volatile uint64_t        s_tl_started_ms = 0;
static volatile bool            s_tl_capture_vis = true;
static volatile bool            s_tl_capture_therm = true;
// 0 = run until manually stopped. Otherwise the loop exits cleanly
// (writing complete=true to session.json) once elapsed crosses this.
static volatile uint32_t        s_tl_max_duration_sec = 0;

static void ensure_tl_mutex(void) {
    if (!s_tl_mutex) s_tl_mutex = xSemaphoreCreateMutex();
}

// MAINTAINER NOTE: same single-exit discipline as capture_now. Every
// path after system_phase_enter(PHASE_CAPTURE) must restore PHASE_LIVE
// before returning. The two return statements below are mirror copies
// of each other for that reason — do not collapse them by adding an
// early return that skips the restore.
//
// One capture iteration: produce artifacts, hand them to session_store
// for transactional commit. session_store owns the SD lock + journal +
// session.json — this loop just chains the engine and the store.
//
// Phase: live timelapse only ever runs from LIVE. Each iteration enters
// CAPTURE for the encode+commit window, then explicitly returns to LIVE.
// No save/restore — the live TL task is the only caller and its outer
// state is always LIVE.
static void tl_capture_iteration(void) {
    if (!s_tl_handle) return;
    system_phase_enter(PHASE_CAPTURE);
    capture_artifacts_t art = {0};
    esp_err_t cap_err = capture_engine_take_one(
        s_tl_capture_vis, s_tl_capture_therm,
        /*want_thermal_raw*/ s_tl_capture_therm,
        &art);
    if (cap_err != ESP_OK) {
        ESP_LOGW(TAG, "tl: nothing produced (%s)", esp_err_to_name(cap_err));
        capture_artifacts_free(&art);
        system_phase_enter(PHASE_LIVE);
        return;
    }
    // session_store_commit handles SD lock + atomic writes + journal
    // (the COMMIT POINT) + session.json rewrite. Per-artifact failures
    // are recorded in the journal; only journal-append failure causes
    // the whole commit to fail (and recovery sweeps any orphans).
    session_store_commit(s_tl_handle, &art, /*meta*/ NULL);
    capture_artifacts_free(&art);
    system_phase_enter(PHASE_LIVE);
}

static void tl_task(void *arg) {
    (void)arg;
    while (!s_tl_stop) {
        tl_capture_iteration();
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

    // Finalize: complete=true session.json + free handle.
    if (s_tl_handle) {
        session_store_close(s_tl_handle);
        s_tl_handle = NULL;
    }
    s_tl_active = false;
    s_tl_task = NULL;
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
        const char *id = s_tl_handle ? session_store_id(s_tl_handle) : "?";
        xSemaphoreGive(s_tl_mutex);
        snprintf(msg_out, msg_cap, "timelapse already running (%s)", id);
        return ESP_ERR_INVALID_STATE;
    }
    if (interval_sec < 1) interval_sec = 1;
    if (interval_sec > 3600) interval_sec = 3600;
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

    char session_id[32];
    snprintf(session_id, sizeof(session_id), "session_%lu",
             (unsigned long)new_session_id());
    session_store_open_args_t args = {
        .mode          = "timelapse",
        .interval_sec  = interval_sec,
        .capture_vis   = capture_vis,
        .capture_therm = capture_therm,
    };
    s_tl_handle = session_store_open(session_id, &args);
    if (!s_tl_handle) {
        xSemaphoreGive(s_tl_mutex);
        snprintf(msg_out, msg_cap, "session_store_open failed");
        return ESP_FAIL;
    }

    s_tl_max_duration_sec = max_duration_sec;
    s_tl_started_ms       = (uint64_t)(esp_timer_get_time() / 1000);
    s_tl_interval_sec     = interval_sec;
    s_tl_capture_vis      = capture_vis;
    s_tl_capture_therm    = capture_therm;

    if (session_id_out) {
        snprintf(session_id_out, session_id_cap, "%s", session_id);
    }

    s_tl_stop = false;
    s_tl_active = true;
    // Pin to core 0 so SPI reader on core 1 isn't displaced by the
    // capture iteration's heap_caps_malloc + JPEG encode + SD write.
    BaseType_t r = xTaskCreatePinnedToCore(tl_task, "tl", 6144, NULL, 5, &s_tl_task, 0);
    if (r != pdPASS) {
        s_tl_active = false;
        session_store_close(s_tl_handle);
        s_tl_handle = NULL;
        xSemaphoreGive(s_tl_mutex);
        snprintf(msg_out, msg_cap, "task create failed");
        return ESP_FAIL;
    }
    xSemaphoreGive(s_tl_mutex);

    snprintf(msg_out, msg_cap, "%s started, interval %lus, vis=%s therm=%s",
             session_id, (unsigned long)interval_sec,
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
    char captured_id[32] = "?";
    uint32_t captured_count = 0;
    if (s_tl_handle) {
        snprintf(captured_id, sizeof(captured_id), "%s", session_store_id(s_tl_handle));
        captured_count = session_store_capture_count(s_tl_handle);
    }
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
    if (s_tl_active && s_tl_handle) {
        snprintf(out->session_id, sizeof(out->session_id), "%s",
                 session_store_id(s_tl_handle));
        snprintf(out->session_dir, sizeof(out->session_dir), "%s",
                 session_store_dir(s_tl_handle));
        out->interval_sec  = s_tl_interval_sec;
        out->capture_count = session_store_capture_count(s_tl_handle);
        out->started_ms    = s_tl_started_ms;
        out->capture_vis   = s_tl_capture_vis;
        out->capture_therm = s_tl_capture_therm;
    }
}
