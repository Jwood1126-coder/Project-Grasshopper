#include "sessions.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

#include "hal_storage.h"
#include "net_relay.h"

static const char *TAG = "sessions";

#define SESSIONS_BASE_DIR     "/sdcard/timelapse"
#define SESS_LIST_MAX          50      // cap session list response size
#define SESS_CAPTURES_MAX      200     // cap captures.jsonl entries inline
#define SESS_FILE_CHUNK_MAX    16384   // 16 KB binary -> ~22 KB base64
#define SESS_QUEUE_LEN         4

typedef enum {
    SESS_REQ_LIST,
    SESS_REQ_GET,
    SESS_REQ_READ_FILE,
} sess_req_type_t;

typedef struct {
    sess_req_type_t type;
    char cmd_id[40];
    char session_id[40];
    char filename[64];
    uint32_t offset;
    uint32_t max_len;
} sess_req_t;

static QueueHandle_t s_queue = NULL;
static TaskHandle_t  s_task  = NULL;

// ───────────── helpers ─────────────

// Returns the cmd-name corresponding to a request type. Used when
// emitting the cmd.result so id+cmd are paired correctly.
static const char *req_cmd_name(sess_req_type_t t) {
    switch (t) {
        case SESS_REQ_LIST:      return "sessions.list";
        case SESS_REQ_GET:       return "sessions.get";
        case SESS_REQ_READ_FILE: return "session.read_file";
    }
    return "sessions.?";
}

// Read a file fully into a malloc'd buffer (PSRAM if available).
// Caller must hold sd_lock. Returns NULL on error; on success, *out_len
// is set and caller frees the buffer.
static char *read_file_text(const char *path, size_t *out_len, size_t max_bytes) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    if ((size_t)sz > max_bytes) { fclose(f); return NULL; }
    rewind(f);
    char *buf = heap_caps_malloc((size_t)sz + 1, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    if (out_len) *out_len = n;
    return buf;
}

// Parse a session.json buffer and return the cJSON object (caller frees).
// On parse error returns NULL.
static cJSON *parse_session_json(const char *buf) {
    if (!buf) return NULL;
    return cJSON_Parse(buf);
}

// Append captures from captures.jsonl into the cJSON array `arr`.
// Returns the number of lines appended; sets *truncated_out true if
// the file had more lines than the cap. Caller holds sd_lock.
static uint32_t append_captures_jsonl(const char *path, cJSON *arr,
                                       uint32_t cap, bool *truncated_out) {
    if (truncated_out) *truncated_out = false;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char line[512];
    uint32_t added = 0;
    while (fgets(line, sizeof(line), f)) {
        if (added >= cap) {
            // Drain rest just to know if we truncated
            if (truncated_out && fgets(line, sizeof(line), f)) *truncated_out = true;
            break;
        }
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) continue;
        cJSON *obj = cJSON_Parse(line);
        if (obj) {
            cJSON_AddItemToArray(arr, obj);
            added++;
        }
    }
    fclose(f);
    return added;
}

// Pull a numeric field with default. Used for sorting the list response.
static double json_num(const cJSON *obj, const char *key, double dflt) {
    if (!obj) return dflt;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valuedouble : dflt;
}

// Validate that session_id and filename are safe path components: no
// slashes, no parent refs, basic charset only. Prevents UI from
// reaching outside /sdcard/timelapse/.
static bool safe_name(const char *s) {
    if (!s || !*s) return false;
    for (const char *p = s; *p; p++) {
        char c = *p;
        if (c == '/' || c == '\\' || c == ':') return false;
        if (c == '.' && p[1] == '.') return false;
    }
    return strlen(s) < 64;
}

// ───────────── handlers ─────────────

static void do_list(const sess_req_t *req) {
    if (!hal_storage_sd_present()) {
        net_relay_emit_cmd_result(req_cmd_name(req->type), req->cmd_id,
                                   false, "SD not mounted", NULL);
        return;
    }
    if (!hal_storage_sd_lock(5000)) {
        net_relay_emit_cmd_result(req_cmd_name(req->type), req->cmd_id,
                                   false, "SD lock timeout", NULL);
        return;
    }

    cJSON *arr = cJSON_CreateArray();
    DIR *dir = opendir(SESSIONS_BASE_DIR);
    uint32_t total = 0, listed = 0;
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strncmp(ent->d_name, "session_", 8) != 0) continue;
            total++;

            char meta_path[320];
            snprintf(meta_path, sizeof(meta_path),
                     "%s/%s/session.json", SESSIONS_BASE_DIR, ent->d_name);
            size_t mlen = 0;
            char *meta_buf = read_file_text(meta_path, &mlen, 4096);
            if (!meta_buf) continue;
            cJSON *meta = parse_session_json(meta_buf);
            free(meta_buf);
            if (!meta) continue;

            // session.json may not include sessionId for older format —
            // fall back to dir name.
            const cJSON *sid_field = cJSON_GetObjectItemCaseSensitive(meta, "sessionId");
            if (!cJSON_IsString(sid_field)) {
                cJSON_DeleteItemFromObject(meta, "sessionId");
                cJSON_AddStringToObject(meta, "sessionId", ent->d_name);
            }
            cJSON_AddItemToArray(arr, meta);
            listed++;

            if (listed >= SESS_LIST_MAX) break;
        }
        closedir(dir);
    } else {
        ESP_LOGW(TAG, "opendir %s failed errno=%d", SESSIONS_BASE_DIR, errno);
    }
    hal_storage_sd_unlock();

    // Sort newest-first by timestamp using cJSON's array index swap.
    // Simple bubble sort since SESS_LIST_MAX is small (50).
    int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - 1 - i; j++) {
            cJSON *a = cJSON_GetArrayItem(arr, j);
            cJSON *b = cJSON_GetArrayItem(arr, j + 1);
            if (json_num(a, "timestamp", 0) < json_num(b, "timestamp", 0)) {
                cJSON_DetachItemFromArray(arr, j + 1);
                cJSON_InsertItemInArray(arr, j, b);
            }
        }
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "sessions", arr);
    cJSON_AddNumberToObject(root, "total", total);
    cJSON_AddNumberToObject(root, "listed", listed);
    cJSON_AddBoolToObject(root, "truncated", total > listed);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    char msg[64];
    snprintf(msg, sizeof(msg), "%lu sessions (%lu listed)",
             (unsigned long)total, (unsigned long)listed);
    net_relay_emit_cmd_result(req_cmd_name(req->type), req->cmd_id,
                               true, msg, json);
    free(json);
}

static void do_get(const sess_req_t *req) {
    if (!req->session_id[0]) {
        net_relay_emit_cmd_result(req_cmd_name(req->type), req->cmd_id,
                                   false, "missing sessionId", NULL);
        return;
    }
    if (!safe_name(req->session_id)) {
        net_relay_emit_cmd_result(req_cmd_name(req->type), req->cmd_id,
                                   false, "bad sessionId", NULL);
        return;
    }
    if (!hal_storage_sd_present()) {
        net_relay_emit_cmd_result(req_cmd_name(req->type), req->cmd_id,
                                   false, "SD not mounted", NULL);
        return;
    }
    if (!hal_storage_sd_lock(5000)) {
        net_relay_emit_cmd_result(req_cmd_name(req->type), req->cmd_id,
                                   false, "SD lock timeout", NULL);
        return;
    }

    char meta_path[200], cap_path[200];
    snprintf(meta_path, sizeof(meta_path),
             "%s/%s/session.json", SESSIONS_BASE_DIR, req->session_id);
    snprintf(cap_path,  sizeof(cap_path),
             "%s/%s/captures.jsonl", SESSIONS_BASE_DIR, req->session_id);

    size_t mlen = 0;
    char *meta_buf = read_file_text(meta_path, &mlen, 4096);
    cJSON *meta = parse_session_json(meta_buf);
    free(meta_buf);
    cJSON *captures = cJSON_CreateArray();
    bool truncated = false;
    uint32_t added = append_captures_jsonl(cap_path, captures,
                                            SESS_CAPTURES_MAX, &truncated);

    hal_storage_sd_unlock();

    if (!meta) {
        cJSON_Delete(captures);
        char msg[80];
        snprintf(msg, sizeof(msg), "session.json missing for %s", req->session_id);
        net_relay_emit_cmd_result(req_cmd_name(req->type), req->cmd_id,
                                   false, msg, NULL);
        return;
    }

    // captureCount = authoritative count from meta (session.json).
    // returnedCount = how many rows we actually included from
    // captures.jsonl. They differ when we hit SESS_CAPTURES_MAX or
    // when the session crashed before writing one of the two files.
    double meta_count = json_num(meta, "captureCount", -1);
    uint32_t cap_count = (meta_count >= 0) ? (uint32_t)meta_count : added;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "sessionId", req->session_id);
    cJSON_AddItemToObject(root, "meta", meta);
    cJSON_AddItemToObject(root, "captures", captures);
    cJSON_AddNumberToObject(root, "captureCount", cap_count);
    cJSON_AddNumberToObject(root, "returnedCount", added);
    cJSON_AddBoolToObject(root, "truncated", truncated);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    char msg[100];
    snprintf(msg, sizeof(msg), "%s: %lu/%lu captures%s",
             req->session_id, (unsigned long)added, (unsigned long)cap_count,
             truncated ? " (truncated)" : "");
    net_relay_emit_cmd_result(req_cmd_name(req->type), req->cmd_id,
                               true, msg, json);
    free(json);
}

static void do_read_file(const sess_req_t *req) {
    if (!safe_name(req->session_id) || !safe_name(req->filename)) {
        net_relay_emit_cmd_result(req_cmd_name(req->type), req->cmd_id,
                                   false, "bad sessionId or filename", NULL);
        return;
    }
    if (!hal_storage_sd_present()) {
        net_relay_emit_cmd_result(req_cmd_name(req->type), req->cmd_id,
                                   false, "SD not mounted", NULL);
        return;
    }
    uint32_t want = req->max_len;
    if (want == 0 || want > SESS_FILE_CHUNK_MAX) want = SESS_FILE_CHUNK_MAX;

    if (!hal_storage_sd_lock(5000)) {
        net_relay_emit_cmd_result(req_cmd_name(req->type), req->cmd_id,
                                   false, "SD lock timeout", NULL);
        return;
    }

    char path[200];
    snprintf(path, sizeof(path), "%s/%s/%s",
             SESSIONS_BASE_DIR, req->session_id, req->filename);
    FILE *f = fopen(path, "rb");
    if (!f) {
        hal_storage_sd_unlock();
        char msg[220];
        snprintf(msg, sizeof(msg), "open %s: errno=%d", path, errno);
        net_relay_emit_cmd_result(req_cmd_name(req->type), req->cmd_id,
                                   false, msg, NULL);
        return;
    }
    fseek(f, 0, SEEK_END);
    long total = ftell(f);
    if (total < 0) total = 0;
    if ((long)req->offset > total) {
        fclose(f);
        hal_storage_sd_unlock();
        net_relay_emit_cmd_result(req_cmd_name(req->type), req->cmd_id,
                                   false, "offset past EOF", NULL);
        return;
    }
    fseek(f, (long)req->offset, SEEK_SET);

    uint8_t *bin = heap_caps_malloc(want, MALLOC_CAP_SPIRAM);
    if (!bin) bin = malloc(want);
    if (!bin) {
        fclose(f);
        hal_storage_sd_unlock();
        net_relay_emit_cmd_result(req_cmd_name(req->type), req->cmd_id,
                                   false, "out of memory", NULL);
        return;
    }
    size_t got = fread(bin, 1, want, f);
    fclose(f);
    hal_storage_sd_unlock();

    size_t b64_cap = ((got + 2) / 3) * 4 + 4;
    uint8_t *b64 = heap_caps_malloc(b64_cap, MALLOC_CAP_SPIRAM);
    if (!b64) b64 = malloc(b64_cap);
    if (!b64) {
        free(bin);
        net_relay_emit_cmd_result(req_cmd_name(req->type), req->cmd_id,
                                   false, "out of memory (b64)", NULL);
        return;
    }
    size_t b64_len = 0;
    int rc = mbedtls_base64_encode(b64, b64_cap, &b64_len, bin, got);
    free(bin);
    if (rc != 0) {
        free(b64);
        net_relay_emit_cmd_result(req_cmd_name(req->type), req->cmd_id,
                                   false, "base64 encode failed", NULL);
        return;
    }
    b64[b64_len] = '\0';

    bool eof = ((long)(req->offset + got) >= total);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "sessionId", req->session_id);
    cJSON_AddStringToObject(root, "filename",  req->filename);
    cJSON_AddNumberToObject(root, "offset",    (double)req->offset);
    cJSON_AddNumberToObject(root, "len",       (double)got);
    cJSON_AddNumberToObject(root, "totalSize", (double)total);
    cJSON_AddBoolToObject(  root, "eof",       eof);
    cJSON_AddStringToObject(root, "b64",       (const char *)b64);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(b64);

    // Keep msg short — sessionId/filename are already in data.
    char msg[80];
    snprintf(msg, sizeof(msg), "[%lu..%lu)/%lu%s",
             (unsigned long)req->offset,
             (unsigned long)(req->offset + got),
             (unsigned long)total,
             eof ? " EOF" : "");
    net_relay_emit_cmd_result(req_cmd_name(req->type), req->cmd_id,
                               true, msg, json);
    free(json);
}

// ───────────── worker task ─────────────

static void worker_task(void *arg) {
    (void)arg;
    sess_req_t req;
    for (;;) {
        if (xQueueReceive(s_queue, &req, portMAX_DELAY) != pdTRUE) continue;
        ESP_LOGI(TAG, "worker: cmd=%s id=%s", req_cmd_name(req.type), req.cmd_id);
        switch (req.type) {
            case SESS_REQ_LIST:      do_list(&req); break;
            case SESS_REQ_GET:       do_get(&req); break;
            case SESS_REQ_READ_FILE: do_read_file(&req); break;
        }
    }
}

// ───────────── public API ─────────────

esp_err_t sessions_init(void) {
    if (s_queue) return ESP_OK;
    s_queue = xQueueCreate(SESS_QUEUE_LEN, sizeof(sess_req_t));
    if (!s_queue) return ESP_ERR_NO_MEM;
    BaseType_t r = xTaskCreate(worker_task, "sessions", 6144, NULL, 4, &s_task);
    if (r != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "worker task started");
    return ESP_OK;
}

static esp_err_t enqueue(const sess_req_t *req) {
    if (!s_queue) return ESP_ERR_INVALID_STATE;
    if (xQueueSend(s_queue, req, 0) != pdTRUE) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

esp_err_t sessions_enqueue_list(const char *cmd_id) {
    sess_req_t r = { .type = SESS_REQ_LIST };
    if (cmd_id) snprintf(r.cmd_id, sizeof(r.cmd_id), "%s", cmd_id);
    return enqueue(&r);
}

esp_err_t sessions_enqueue_get(const char *cmd_id, const char *session_id) {
    sess_req_t r = { .type = SESS_REQ_GET };
    if (cmd_id)     snprintf(r.cmd_id,    sizeof(r.cmd_id),    "%s", cmd_id);
    if (session_id) snprintf(r.session_id, sizeof(r.session_id), "%s", session_id);
    return enqueue(&r);
}

esp_err_t sessions_enqueue_read_file(const char *cmd_id,
                                      const char *session_id,
                                      const char *filename,
                                      uint32_t offset, uint32_t max_len) {
    sess_req_t r = { .type = SESS_REQ_READ_FILE,
                     .offset = offset, .max_len = max_len };
    if (cmd_id)     snprintf(r.cmd_id,     sizeof(r.cmd_id),     "%s", cmd_id);
    if (session_id) snprintf(r.session_id, sizeof(r.session_id), "%s", session_id);
    if (filename)   snprintf(r.filename,   sizeof(r.filename),   "%s", filename);
    return enqueue(&r);
}
