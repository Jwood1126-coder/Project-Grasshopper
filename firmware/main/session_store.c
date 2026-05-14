// session_store: durable on-SD session record-keeping.
//
// Owns SD layout for /sdcard/timelapse/session_<id>/. The journal
// (captures.jsonl) is the COMMIT POINT — anything renamed to its
// final name but not in the journal is treated as an orphan and
// deleted on recovery.

#include "session_store.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "hal_lepton.h"
#include "hal_storage.h"

static const char *TAG = "session_store";

#define LEP_PIXELS              (160 * 120)
#define SESSIONS_BASE_DIR       "/sdcard/timelapse"

// ---- TLinear scale helpers (mirrors capture.c's local copy) ----
//
// Centi-Kelvin per Lepton count. resolution=0 → 10 cK/count (0.1 K),
// resolution=1 → 1 cK/count (0.01 K).
static inline int tlinear_scale_x100(uint16_t res) {
    return (res == 1) ? 1 : 10;
}
static inline double raw_to_F_with_scale(uint32_t raw, int scale_x100) {
    double cK = (double)raw * (double)scale_x100;
    double C  = cK / 100.0 - 273.15;
    return C * 9.0 / 5.0 + 32.0;
}

// ---- Handle ----

struct session_store_handle {
    char     session_id[32];
    char     session_dir[160];   // SESSIONS_BASE_DIR (≤32) + '/' + session_id (≤31) + slack
    char     mode[16];
    uint32_t interval_sec;
    bool     capture_vis;
    bool     capture_therm;

    // Aggregates (populated either from open-time journal replay
    // or from cumulative session_store_commit calls).
    uint32_t capture_count;          // committed seqs
    uint32_t valid_therm_count;      // captures with valid radiometric thermal
    uint32_t min_raw_session;        // 0xFFFFFFFFu = unset
    uint32_t max_raw_session;
    uint64_t sum_center_raw;
    uint16_t resolution;             // last seen TLinear resolution

    uint64_t started_ms;             // monotonic ms (from esp_timer)
    uint64_t started_epoch;          // wall-clock seconds (0 if no NTP)
};

// ---- Filename helpers ----

static void make_image_path(char *out, size_t cap,
                             const char *session_dir, uint32_t seq,
                             const char *suffix) {
    snprintf(out, cap, "%s/%06lu_%s",
             session_dir, (unsigned long)seq, suffix);
}

// Parse a 6-digit prefix from a filename like "000007_vis.jpg".
// Returns -1 if it doesn't match.
static int filename_seq(const char *name) {
    if (strlen(name) < 7) return -1;
    for (int i = 0; i < 6; i++) {
        if (name[i] < '0' || name[i] > '9') return -1;
    }
    if (name[6] != '_') return -1;
    return atoi(name);
}

// ---- Journal replay ----
//
// Reads captures.jsonl line by line, returns max_seq + per-session
// aggregates. Tolerates a partial last line (truncated mid-write —
// power loss between fwrite and fsync). Caller pre-zeroes `agg`
// fields as needed; min_raw_session must come in as 0xFFFFFFFFu.

typedef struct {
    uint32_t capture_count;
    uint32_t valid_therm_count;
    uint32_t min_raw_session;
    uint32_t max_raw_session;
    uint64_t sum_center_raw;
    uint16_t resolution;
    uint32_t max_seq;
    uint64_t earliest_epoch;         // for derivation of started_epoch
    uint64_t latest_epoch;
} replay_agg_t;

static void replay_init(replay_agg_t *agg) {
    memset(agg, 0, sizeof(*agg));
    agg->min_raw_session = 0xFFFFFFFFu;
}

static void replay_fold_line(replay_agg_t *agg, const char *line) {
    cJSON *root = cJSON_Parse(line);
    if (!root) return;   // malformed — likely partial last line
    cJSON *jseq = cJSON_GetObjectItemCaseSensitive(root, "seq");
    if (!cJSON_IsNumber(jseq)) { cJSON_Delete(root); return; }

    uint32_t seq = (uint32_t)jseq->valuedouble;
    if (seq > agg->max_seq) agg->max_seq = seq;
    agg->capture_count++;

    cJSON *jts = cJSON_GetObjectItemCaseSensitive(root, "timestamp");
    if (cJSON_IsNumber(jts)) {
        uint64_t ts = (uint64_t)jts->valuedouble;
        if (ts > 0) {
            if (agg->earliest_epoch == 0 || ts < agg->earliest_epoch) {
                agg->earliest_epoch = ts;
            }
            if (ts > agg->latest_epoch) agg->latest_epoch = ts;
        }
    }

    cJSON *jthok = cJSON_GetObjectItemCaseSensitive(root, "thermOk");
    cJSON *jmin  = cJSON_GetObjectItemCaseSensitive(root, "minRaw");
    cJSON *jmax  = cJSON_GetObjectItemCaseSensitive(root, "maxRaw");
    cJSON *jctr  = cJSON_GetObjectItemCaseSensitive(root, "centerRaw");
    cJSON *jres  = cJSON_GetObjectItemCaseSensitive(root, "tlinearResolution");
    bool therm_ok = cJSON_IsTrue(jthok);
    if (therm_ok && cJSON_IsNumber(jmin) && cJSON_IsNumber(jmax) &&
        cJSON_IsNumber(jctr)) {
        uint32_t mn  = (uint32_t)jmin->valuedouble;
        uint32_t mx  = (uint32_t)jmax->valuedouble;
        uint32_t ctr = (uint32_t)jctr->valuedouble;
        if (mn < agg->min_raw_session) agg->min_raw_session = mn;
        if (mx > agg->max_raw_session) agg->max_raw_session = mx;
        agg->sum_center_raw += ctr;
        agg->valid_therm_count++;
        if (cJSON_IsNumber(jres)) agg->resolution = (uint16_t)jres->valuedouble;
    }
    cJSON_Delete(root);
}

// Returns ESP_OK if file exists and was scanned (even if 0 records).
// ESP_ERR_NOT_FOUND if the file isn't there. Other errors on I/O fail.
static esp_err_t replay_journal(const char *session_dir, replay_agg_t *agg) {
    char path[256];
    snprintf(path, sizeof(path), "%s/captures.jsonl", session_dir);
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT) return ESP_ERR_NOT_FOUND;
        return ESP_FAIL;
    }
    char buf[768];
    while (fgets(buf, sizeof(buf), f)) {
        // fgets includes the trailing newline if present. A partial
        // last line (no trailing \n) is malformed and replay_fold_line
        // will reject it via cJSON_Parse, which is the right behavior.
        replay_fold_line(agg, buf);
    }
    fclose(f);
    return ESP_OK;
}

// ---- Sidecar writers ----

// Per-capture .raw16 (38400 B) + _therm.json. Caller holds SD lock.
// Returns ok flags via *raw16_ok / *meta_ok (writes are best-effort:
// failure of one doesn't abort the other).
static void write_thermal_sidecars(const struct session_store_handle *h,
                                    uint32_t seq,
                                    const capture_artifacts_t *art,
                                    bool *raw16_ok, bool *meta_ok) {
    *raw16_ok = false;
    *meta_ok  = false;

    char path[256];
    if (art->therm_raw) {
        make_image_path(path, sizeof(path), h->session_dir, seq, "therm.raw16");
        if (hal_storage_sd_atomic_write(path, art->therm_raw,
                                         LEP_PIXELS * sizeof(uint16_t)) > 0) {
            *raw16_ok = true;
        } else {
            ESP_LOGW(TAG, "raw16 write failed: %s", path);
        }
    }

    char meta[640];
    int n = snprintf(meta, sizeof(meta),
        "{\"seq\":%lu,"
         "\"timestamp\":%lu,"
         "\"rawDims\":{\"w\":160,\"h\":120},"
         "\"thermRotation\":%u,"
         "\"tlinearActive\":%s,"
         "\"tlinearAutoRes\":%s,"
         "\"tlinearResolution\":%u,"
         "\"agcEnabled\":%s,"
         "\"gainMode\":%d,"
         "\"lastFFCMs\":0",
        (unsigned long)seq,
        (unsigned long)time(NULL),
        (unsigned)art->therm_rotation,
        art->tlinear_active   ? "true" : "false",
        art->tlinear_auto_res ? "true" : "false",
        (unsigned)art->therm_stats.resolution,
        art->agc_enabled ? "true" : "false",
        art->gain_mode);
    if (art->therm_stats.valid && n > 0 && n < (int)sizeof(meta)) {
        int sx = tlinear_scale_x100(art->therm_stats.resolution);
        int n2 = snprintf(meta + n, sizeof(meta) - n,
            ",\"minRaw\":%lu,\"maxRaw\":%lu,\"centerRaw\":%lu,"
            "\"minTempF\":%.2f,\"maxTempF\":%.2f,\"centerTempF\":%.2f",
            (unsigned long)art->therm_stats.min_raw,
            (unsigned long)art->therm_stats.max_raw,
            (unsigned long)art->therm_stats.center_raw,
            raw_to_F_with_scale(art->therm_stats.min_raw,    sx),
            raw_to_F_with_scale(art->therm_stats.max_raw,    sx),
            raw_to_F_with_scale(art->therm_stats.center_raw, sx));
        if (n2 > 0 && n + n2 < (int)sizeof(meta)) n += n2;
    }
    if (n + 2 < (int)sizeof(meta)) {
        meta[n++] = '}';
        meta[n]   = '\0';
    }
    make_image_path(path, sizeof(path), h->session_dir, seq, "therm.json");
    if (hal_storage_sd_atomic_write(path, meta, (size_t)n) > 0) {
        *meta_ok = true;
    } else {
        ESP_LOGW(TAG, "therm.json write failed: %s", path);
    }
}

// Append one journal record (the COMMIT). Caller holds SD lock.
// Returns true on successful append + fsync.
static bool append_journal(const struct session_store_handle *h,
                            uint32_t seq,
                            bool vis_ok, size_t vis_bytes,
                            bool therm_ok, size_t therm_bytes,
                            const capture_artifacts_t *art,
                            const session_capture_meta_t *meta) {
    char path[256];
    snprintf(path, sizeof(path), "%s/captures.jsonl", h->session_dir);
    FILE *f = fopen(path, "a");
    if (!f) {
        ESP_LOGE(TAG, "journal open(%s) failed errno=%d", path, errno);
        return false;
    }
    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    uint64_t session_ms = (h->started_ms > 0 && now_ms >= h->started_ms)
                          ? (now_ms - h->started_ms) : 0;
    uint64_t actual_epoch = (uint64_t)time(NULL);

    // time-quality: 2 if NTP synced (epoch is real), 1 if estimated
    // (caller forced it via meta), 0 if completely unknown.
    uint8_t tq = 0;
    if (meta && meta->time_quality > 0) {
        tq = meta->time_quality;
    } else {
        tq = (actual_epoch > 1700000000ULL) ? 2 : 0;  // 2024+ → synced
    }
    uint64_t intended = (meta && meta->intended_epoch > 0) ? meta->intended_epoch
                                                            : actual_epoch;

    int n = fprintf(f,
        "{\"seq\":%lu,"
         "\"sessionMs\":%llu,"
         "\"visOk\":%s,"
         "\"thermOk\":%s,"
         "\"visBytes\":%u,"
         "\"thermBytes\":%u,"
         "\"timestamp\":%llu,"
         "\"intendedEpoch\":%llu,"
         "\"timeQuality\":%u",
        (unsigned long)seq,
        (unsigned long long)session_ms,
        vis_ok    ? "true" : "false",
        therm_ok  ? "true" : "false",
        (unsigned)vis_bytes,
        (unsigned)therm_bytes,
        (unsigned long long)actual_epoch,
        (unsigned long long)intended,
        (unsigned)tq);
    if (n < 0) { fclose(f); return false; }

    if (meta && meta->wake_ms > 0) {
        fprintf(f, ",\"wakeMs\":%lu", (unsigned long)meta->wake_ms);
    }
    if (art->visible_ms > 0) {
        fprintf(f, ",\"visibleMs\":%lu", (unsigned long)art->visible_ms);
    }
    if (art->thermal_ms > 0) {
        fprintf(f, ",\"thermalMs\":%lu", (unsigned long)art->thermal_ms);
    }

    if (art->therm_stats.valid) {
        int sx = tlinear_scale_x100(art->therm_stats.resolution);
        fprintf(f,
            ",\"minRaw\":%lu,\"maxRaw\":%lu,\"centerRaw\":%lu,"
            "\"tlinearResolution\":%u,"
            "\"minTempF\":%.2f,\"maxTempF\":%.2f,\"centerTempF\":%.2f",
            (unsigned long)art->therm_stats.min_raw,
            (unsigned long)art->therm_stats.max_raw,
            (unsigned long)art->therm_stats.center_raw,
            (unsigned)art->therm_stats.resolution,
            raw_to_F_with_scale(art->therm_stats.min_raw,    sx),
            raw_to_F_with_scale(art->therm_stats.max_raw,    sx),
            raw_to_F_with_scale(art->therm_stats.center_raw, sx));
    }
    fputs("}\n", f);
    fflush(f);
    int fd = fileno(f);
    if (fd >= 0) fsync(fd);
    fclose(f);
    return true;
}

// Build + atomically write session.json from current handle state.
// Caller holds SD lock.
static esp_err_t write_session_json(const struct session_store_handle *h,
                                     bool complete) {
    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    uint64_t elapsed_sec = (h->started_ms > 0 && now_ms >= h->started_ms)
                           ? (now_ms - h->started_ms) / 1000 : 0;
    uint64_t now_epoch = (uint64_t)time(NULL);
    uint64_t start_epoch = h->started_epoch;
    if (start_epoch == 0) {
        // NTP wasn't synced at open time. If it's synced now, derive
        // start_epoch as now - elapsed; otherwise leave at 0.
        if (now_epoch > elapsed_sec) start_epoch = now_epoch - elapsed_sec;
    }

    char meta[768];
    int n = snprintf(meta, sizeof(meta),
        "{\"sessionId\":\"%s\","
         "\"intervalSec\":%lu,"
         "\"captureCount\":%lu,"
         "\"timestamp\":%llu,"
         "\"captureVis\":%s,"
         "\"captureTherm\":%s,"
         "\"mode\":\"%s\","
         "\"complete\":%s,"
         "\"durationSec\":%llu",
        h->session_id,
        (unsigned long)h->interval_sec,
        (unsigned long)h->capture_count,
        (unsigned long long)start_epoch,
        h->capture_vis ? "true" : "false",
        h->capture_therm ? "true" : "false",
        h->mode,
        complete ? "true" : "false",
        (unsigned long long)elapsed_sec);

    if (h->valid_therm_count > 0 && h->max_raw_session > 0 &&
        h->min_raw_session != 0xFFFFFFFFu && n > 0 && n < (int)sizeof(meta)) {
        int sx = tlinear_scale_x100(h->resolution);
        uint64_t avg_ctr = h->sum_center_raw / h->valid_therm_count;
        int n2 = snprintf(meta + n, sizeof(meta) - n,
            ",\"tempStats\":{"
              "\"minRaw\":%lu,\"maxRaw\":%lu,\"avgCenterRaw\":%llu,"
              "\"tlinearResolution\":%u,"
              "\"validThermCount\":%lu,"
              "\"minTempF\":%.2f,\"maxTempF\":%.2f,\"avgCenterTempF\":%.2f"
            "}",
            (unsigned long)h->min_raw_session,
            (unsigned long)h->max_raw_session,
            (unsigned long long)avg_ctr,
            (unsigned)h->resolution,
            (unsigned long)h->valid_therm_count,
            raw_to_F_with_scale(h->min_raw_session, sx),
            raw_to_F_with_scale(h->max_raw_session, sx),
            raw_to_F_with_scale((uint32_t)avg_ctr,  sx));
        if (n2 > 0 && n + n2 < (int)sizeof(meta)) n += n2;
    }
    if (n + 2 < (int)sizeof(meta)) {
        meta[n++] = '}';
        meta[n]   = '\0';
    }

    char path[256];
    snprintf(path, sizeof(path), "%s/session.json", h->session_dir);
    int w = hal_storage_sd_atomic_write(path, meta, (size_t)n);
    return (w > 0) ? ESP_OK : ESP_FAIL;
}

// ---- Public API ----

session_store_handle_t *session_store_open(const char *session_id,
                                            const session_store_open_args_t *args) {
    if (!session_id || !args || !args->mode) return NULL;
    if (!hal_storage_sd_present()) {
        ESP_LOGE(TAG, "open: SD not mounted");
        return NULL;
    }

    struct session_store_handle *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    snprintf(h->session_id, sizeof(h->session_id), "%s", session_id);
    snprintf(h->session_dir, sizeof(h->session_dir),
             "%s/%s", SESSIONS_BASE_DIR, session_id);
    snprintf(h->mode, sizeof(h->mode), "%s", args->mode);
    h->interval_sec   = args->interval_sec;
    h->capture_vis    = args->capture_vis;
    h->capture_therm  = args->capture_therm;
    h->min_raw_session = 0xFFFFFFFFu;
    h->started_ms     = (uint64_t)(esp_timer_get_time() / 1000);
    h->started_epoch  = (uint64_t)time(NULL);

    if (!hal_storage_sd_lock(5000)) {
        ESP_LOGE(TAG, "open: SD lock timeout");
        free(h);
        return NULL;
    }
    if (hal_storage_sd_mkdir_p(h->session_dir) != ESP_OK) {
        ESP_LOGE(TAG, "open: mkdir(%s) failed", h->session_dir);
        hal_storage_sd_unlock();
        free(h);
        return NULL;
    }

    // Resume support: if captures.jsonl already exists, replay it
    // and seed our aggregates so resumed sessions don't reset.
    replay_agg_t agg; replay_init(&agg);
    if (replay_journal(h->session_dir, &agg) == ESP_OK && agg.capture_count > 0) {
        h->capture_count     = agg.capture_count;
        h->valid_therm_count = agg.valid_therm_count;
        h->min_raw_session   = agg.min_raw_session;
        h->max_raw_session   = agg.max_raw_session;
        h->sum_center_raw    = agg.sum_center_raw;
        h->resolution        = agg.resolution;
        if (agg.earliest_epoch > 0) h->started_epoch = agg.earliest_epoch;
        ESP_LOGI(TAG, "open: resumed %s with %lu prior captures",
                 session_id, (unsigned long)h->capture_count);
    } else {
        ESP_LOGI(TAG, "open: new session %s", session_id);
    }

    write_session_json(h, /*complete*/ false);
    hal_storage_sd_unlock();
    return h;
}

esp_err_t session_store_commit(session_store_handle_t *h,
                                const capture_artifacts_t *art,
                                const session_capture_meta_t *meta) {
    if (!h || !art) return ESP_ERR_INVALID_ARG;
    if (!hal_storage_sd_lock(5000)) {
        ESP_LOGW(TAG, "commit: SD lock timeout");
        return ESP_ERR_TIMEOUT;
    }
    uint32_t seq = h->capture_count + 1;

    char path[256];
    bool   vis_ok = false, therm_ok = false;
    size_t vis_bytes = 0, therm_bytes = 0;

    // ---- Image artifacts: each via atomic .tmp → fsync → rename ----
    if (art->vis_jpg && h->capture_vis) {
        make_image_path(path, sizeof(path), h->session_dir, seq, "vis.jpg");
        if (hal_storage_sd_atomic_write(path, art->vis_jpg, art->vis_len) > 0) {
            vis_ok = true;
            vis_bytes = art->vis_len;
        } else {
            ESP_LOGW(TAG, "vis write failed: %s", path);
        }
    }
    if (art->therm_jpg && h->capture_therm) {
        make_image_path(path, sizeof(path), h->session_dir, seq, "therm.jpg");
        if (hal_storage_sd_atomic_write(path, art->therm_jpg, art->therm_len) > 0) {
            therm_ok = true;
            therm_bytes = art->therm_len;
        } else {
            ESP_LOGW(TAG, "therm write failed: %s", path);
        }
    }
    bool raw16_ok = false, meta_ok = false;
    if (h->capture_therm && art->therm_stats.valid) {
        write_thermal_sidecars(h, seq, art, &raw16_ok, &meta_ok);
    }
    (void)raw16_ok; (void)meta_ok;   // recorded only via existence on disk

    // ---- COMMIT POINT: append journal ----
    if (!append_journal(h, seq, vis_ok, vis_bytes, therm_ok, therm_bytes,
                         art, meta)) {
        ESP_LOGE(TAG, "commit: journal append failed — orphans will be cleaned on next recovery");
        hal_storage_sd_unlock();
        return ESP_FAIL;
    }

    // ---- Update aggregates AFTER journal commit ----
    h->capture_count = seq;
    if (art->therm_stats.valid) {
        if (art->therm_stats.min_raw < h->min_raw_session)
            h->min_raw_session = art->therm_stats.min_raw;
        if (art->therm_stats.max_raw > h->max_raw_session)
            h->max_raw_session = art->therm_stats.max_raw;
        h->sum_center_raw  += art->therm_stats.center_raw;
        h->valid_therm_count++;
        h->resolution = art->therm_stats.resolution;
    }

    // ---- Rewrite session.json (derived; safe to fail — recovery rebuilds) ----
    write_session_json(h, /*complete*/ false);
    hal_storage_sd_unlock();

    ESP_LOGI(TAG, "commit %s seq=%lu vis=%u therm=%u",
             h->session_id, (unsigned long)seq,
             (unsigned)vis_bytes, (unsigned)therm_bytes);
    return ESP_OK;
}

esp_err_t session_store_mark_aborted(const char *session_id,
                                       const char *reason) {
    if (!session_id || !*session_id) return ESP_ERR_INVALID_ARG;
    if (!reason || !*reason) reason = "interrupted-cold-boot";

    if (!hal_storage_sd_lock(5000)) {
        ESP_LOGW(TAG, "mark_aborted: SD lock timeout");
        return ESP_ERR_TIMEOUT;
    }

    char dir[160];
    snprintf(dir, sizeof(dir), "%s/%s", SESSIONS_BASE_DIR, session_id);
    // Create dir in case the session was armed but never reached
    // session_store_open (worker crashed, etc).
    hal_storage_sd_mkdir_p(dir);

    // Replay any existing journal so the aborted marker carries the
    // capture count and aggregates the user already invested in. If
    // there's no journal, agg stays zeroed and we write a 0-capture
    // marker — still useful as a "this session existed but committed
    // nothing" signal.
    replay_agg_t agg; replay_init(&agg);
    (void)replay_journal(dir, &agg);

    char path[256];
    snprintf(path, sizeof(path), "%s/session.json", dir);

    char meta[768];
    int n = snprintf(meta, sizeof(meta),
        "{\"sessionId\":\"%s\","
         "\"mode\":\"timelapse\","
         "\"complete\":false,"
         "\"aborted\":true,"
         "\"abortedReason\":\"%s\","
         "\"abortedAt\":%llu,"
         "\"captureCount\":%lu,"
         "\"timestamp\":%llu,"
         "\"intervalSec\":0,"
         "\"durationSec\":0,"
         "\"captureVis\":true,"
         "\"captureTherm\":true}",
        session_id, reason,
        (unsigned long long)time(NULL),
        (unsigned long)agg.capture_count,
        (unsigned long long)agg.earliest_epoch);

    int w = (n > 0 && (size_t)n < sizeof(meta))
            ? hal_storage_sd_atomic_write(path, meta, (size_t)n)
            : -1;
    hal_storage_sd_unlock();
    if (w <= 0) {
        ESP_LOGE(TAG, "mark_aborted: write %s failed", path);
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "mark_aborted: %s reason=\"%s\" prior_captures=%lu",
             session_id, reason, (unsigned long)agg.capture_count);
    return ESP_OK;
}

uint32_t session_store_journal_max_seq(const char *session_id) {
    if (!session_id || !*session_id) return 0;
    char dir[160];
    snprintf(dir, sizeof(dir), "%s/%s", SESSIONS_BASE_DIR, session_id);
    if (!hal_storage_sd_lock(5000)) {
        ESP_LOGW(TAG, "journal_max_seq: SD lock timeout");
        return 0;
    }
    replay_agg_t agg; replay_init(&agg);
    esp_err_t r = replay_journal(dir, &agg);
    hal_storage_sd_unlock();
    if (r != ESP_OK) return 0;
    return agg.max_seq;
}

esp_err_t session_store_close(session_store_handle_t *h) {
    if (!h) return ESP_ERR_INVALID_ARG;
    if (hal_storage_sd_lock(5000)) {
        write_session_json(h, /*complete*/ true);
        hal_storage_sd_unlock();
    } else {
        ESP_LOGW(TAG, "close: SD lock timeout — session.json may stay incomplete (recovery will fix)");
    }
    ESP_LOGI(TAG, "close %s (final count=%lu)",
             h->session_id, (unsigned long)h->capture_count);
    free(h);
    return ESP_OK;
}

void session_store_release(session_store_handle_t *h) {
    if (!h) return;
    ESP_LOGI(TAG, "release %s (count=%lu, NOT marking complete)",
             h->session_id, (unsigned long)h->capture_count);
    free(h);
}

uint32_t    session_store_capture_count(const session_store_handle_t *h) {
    return h ? h->capture_count : 0;
}
uint64_t    session_store_started_ms(const session_store_handle_t *h) {
    return h ? h->started_ms : 0;
}
const char *session_store_id(const session_store_handle_t *h) {
    return h ? h->session_id : "";
}
const char *session_store_dir(const session_store_handle_t *h) {
    return h ? h->session_dir : "";
}

// ---- Recovery ----
//
// For each /sdcard/timelapse/session_<id>/ dir whose session.json
// says complete=false (or is missing): replay journal, delete files
// with seq > max committed seq, delete *.tmp orphans, rewrite
// session.json from journal aggregates. Idempotent.

static bool session_marked_complete(const char *session_dir) {
    char path[256];
    snprintf(path, sizeof(path), "%s/session.json", session_dir);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char buf[768];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return false;
    buf[n] = '\0';
    cJSON *root = cJSON_Parse(buf);
    if (!root) return false;
    cJSON *c = cJSON_GetObjectItemCaseSensitive(root, "complete");
    bool complete = cJSON_IsTrue(c);
    cJSON_Delete(root);
    return complete;
}

static void recover_one_session(const char *session_id) {
    char dir[160];
    snprintf(dir, sizeof(dir), "%s/%s", SESSIONS_BASE_DIR, session_id);

    if (session_marked_complete(dir)) return;   // already finalized

    if (!hal_storage_sd_lock(5000)) {
        ESP_LOGW(TAG, "recover: SD lock timeout for %s", session_id);
        return;
    }

    replay_agg_t agg; replay_init(&agg);
    esp_err_t r = replay_journal(dir, &agg);
    if (r == ESP_ERR_NOT_FOUND) {
        // No journal → no captures committed. Delete any orphan
        // image/.tmp files; leave session.json (or absence of it)
        // alone — caller may want to know "session existed but had
        // nothing in it". Future deep-sleep recovery may want to
        // delete the empty dir; for now, just clean files.
        ESP_LOGI(TAG, "recover: %s has no journal — cleaning orphans", session_id);
    } else if (r != ESP_OK) {
        ESP_LOGW(TAG, "recover: journal read failed for %s", session_id);
        hal_storage_sd_unlock();
        return;
    }

    DIR *d = opendir(dir);
    if (!d) {
        ESP_LOGW(TAG, "recover: opendir(%s) errno=%d", dir, errno);
        hal_storage_sd_unlock();
        return;
    }
    struct dirent *e;
    int orphans_deleted = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        size_t L = strlen(e->d_name);

        // Always nuke *.tmp orphans — atomic_write would have
        // cleaned its own on success, so any .tmp here is from a
        // crash mid-write.
        if (L > 4 && strcmp(e->d_name + L - 4, ".tmp") == 0) {
            // dir ≤160, e->d_name ≤NAME_MAX (255 on FATFS).
            char p[480];
            snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
            if (unlink(p) == 0) orphans_deleted++;
            continue;
        }

        // NNNNNN_*.* orphans: seq > max_seq from journal.
        int seq = filename_seq(e->d_name);
        if (seq > 0 && (uint32_t)seq > agg.max_seq) {
            // dir ≤160, e->d_name ≤NAME_MAX (255 on FATFS).
            char p[480];
            snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
            if (unlink(p) == 0) orphans_deleted++;
        }
    }
    closedir(d);

    // Apply replay aggregates into a transient handle and rewrite
    // session.json so the UI sees real numbers next time.
    if (agg.capture_count > 0) {
        struct session_store_handle h = {0};
        snprintf(h.session_id, sizeof(h.session_id), "%s", session_id);
        snprintf(h.session_dir, sizeof(h.session_dir), "%s", dir);
        snprintf(h.mode, sizeof(h.mode), "%s",
                 agg.capture_count > 1 ? "timelapse" : "single");
        h.interval_sec      = 0;     // unknown post-recovery
        h.capture_vis       = true;  // best-effort
        h.capture_therm     = true;
        h.capture_count     = agg.capture_count;
        h.valid_therm_count = agg.valid_therm_count;
        h.min_raw_session   = agg.min_raw_session;
        h.max_raw_session   = agg.max_raw_session;
        h.sum_center_raw    = agg.sum_center_raw;
        h.resolution        = agg.resolution;
        if (agg.earliest_epoch > 0) h.started_epoch = agg.earliest_epoch;
        // started_ms left at 0 → durationSec computes to 0; epoch
        // span (earliest..latest) could be derived but the field
        // isn't load-bearing on a recovered session.
        write_session_json(&h, /*complete*/ false);
    }
    hal_storage_sd_unlock();

    if (orphans_deleted > 0 || agg.capture_count > 0) {
        ESP_LOGI(TAG, "recover %s: %lu committed, %d orphan(s) deleted",
                 session_id, (unsigned long)agg.capture_count, orphans_deleted);
    }
}

esp_err_t session_store_recover_all(void) {
    if (!hal_storage_sd_present()) return ESP_ERR_INVALID_STATE;
    DIR *base = opendir(SESSIONS_BASE_DIR);
    if (!base) {
        if (errno == ENOENT) return ESP_OK;   // no sessions yet
        ESP_LOGW(TAG, "recover_all: opendir errno=%d", errno);
        return ESP_FAIL;
    }
    struct dirent *e;
    int scanned = 0;
    while ((e = readdir(base))) {
        if (strncmp(e->d_name, "session_", 8) != 0) continue;
        recover_one_session(e->d_name);
        scanned++;
    }
    closedir(base);
    ESP_LOGI(TAG, "recover_all: scanned %d session dir(s)", scanned);
    return ESP_OK;
}
