#include "net_relay.h"

#include <string.h>
#include <stdlib.h>
#include <sys/time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "hal_lepton.h"
#include "proto_gen.h"
#include "sdkconfig.h"
#include "system_phase.h"

#ifndef GRASSHOPPER_FW_VERSION
#define GRASSHOPPER_FW_VERSION "0.1.0-dev"
#endif

static const char *TAG = "net_relay";
static esp_websocket_client_handle_t s_client = NULL;
static volatile bool s_connected = false;
static net_relay_cmd_handler_t s_cmd_handler = NULL;

// ---- Inbound message buffer ----
//
// Bun fragments large WS messages on the relay side, so we have to be
// able to reassemble incoming text frames. Practical inbound JSON is
// small (cmd payloads), so 4 KB is plenty.
#define INBOUND_BUF_SIZE 4096
static char s_inbound_buf[INBOUND_BUF_SIZE];
static size_t s_inbound_len = 0;

// ---- Single-sender queue (PR-F) ----
//
// Previously every sender (preview, tick, log, cmd.result) called
// esp_websocket_client_send_* directly. The WS client takes its
// own internal mutex during the entire TLS+TCP write, so a slow
// 50 KB preview send (1-2 s on iPhone-hotspot uplink) blocks tick
// + cmd.result for that whole window. Tick failed at its 1000 ms
// timeout, the WS event task couldn't process pings/pongs while
// the mutex was held, and the relay closed the connection with
// code=1006 — the WS thrash we hunted in PR-A.
//
// Now: callers post messages to a bounded queue. A single sender
// task drains the queue and owns esp_websocket_client_send_*
// exclusively. Tick + cmd.result + preview no longer race on the
// mutex; each just enqueues and returns immediately.
//
// Buffers are heap-copied at enqueue (callers don't have to manage
// lifetime), freed by the sender after the send completes.
//
// Drop policy: if the queue is full, the new message is dropped
// (return ESP_FAIL). 16 slots is enough for ~10 s of buildup at
// the steady-state ~2 msg/s rate; any more and we're behind on
// throughput, in which case dropping the newest is the right call.

typedef enum {
    SEND_TEXT   = 0,
    SEND_BINARY = 1,
} send_kind_t;

typedef struct {
    send_kind_t kind;
    uint8_t    *buf;       // heap-owned (PSRAM for binary, internal for text)
    size_t      len;
} send_msg_t;

#define SEND_QUEUE_DEPTH 16
static QueueHandle_t s_send_queue = NULL;
static TaskHandle_t  s_sender_task = NULL;
static volatile bool s_sender_stop = false;

// Forward decls — these are defined further down but referenced by
// emit_cmd_result (early), net_relay_start, and net_relay_stop.
static esp_err_t enqueue(send_kind_t kind, const void *src, size_t len);
static void      send_queue_flush(void);
static void      sender_task(void *arg);

static void send_hello(void) {
    Hello_t h = {
        .type = "hello",
        .deviceId = CONFIG_GRASSHOPPER_DEVICE_ID,
        .token = CONFIG_GRASSHOPPER_RELAY_TOKEN,
        .fwVersion = GRASSHOPPER_FW_VERSION,
        .gitSha = "dev",
        .bootReason = "cold",
    };
    char buf[512];
    size_t n = Hello_to_json(buf, sizeof(buf), &h);
    if (n == 0 || n >= sizeof(buf)) return;

    // Splice the system phase block into hello so the dashboard knows
    // the device's current phase the moment a connection comes up,
    // without waiting up to 1.5 s for the first tick. Critical for
    // wake-Wi-Fi windows: the WS connection is the ONLY message the
    // dashboard sees during the first second of the window, before
    // any tick task is even running. Hand-append the same way
    // splice_extras does in app_main.c — trim the trailing `}` and
    // re-close.
    if (n > 0 && buf[n - 1] == '}') {
        n -= 1;
        int extra = snprintf(buf + n, sizeof(buf) - n,
            ",\"system\":{\"phase\":\"%s\",\"phaseEnteredMs\":%lu}}",
            system_phase_name(system_phase_get()),
            (unsigned long)system_phase_entered_ms());
        if (extra > 0 && (size_t)(n + extra) < sizeof(buf)) {
            n += extra;
        } else {
            // Append failed — restore the trailing `}` so we still
            // ship a valid JSON object.
            buf[n++] = '}';
        }
    }

    esp_websocket_client_send_text(s_client, buf, (int)n,
                                    pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "hello sent (%u B)", (unsigned)n);
}

// ---- Command result emit ----
//
// Every command returns a `cmd.result` event back to the relay. UI
// matches the `id` field to its outstanding command so retries over a
// flaky link don't produce stale state.
//
// Built with cJSON so the msg field is properly escaped. Earlier snprintf
// version was fine for fixed messages but unsafe once SD paths, file
// names, or user labels could appear in the msg (a single embedded
// quote or backslash would corrupt the JSON envelope). The optional
// data_json argument is parsed and embedded under "data" — used by
// sessions.* commands to attach structured payloads (file lists, file
// chunks) that can't fit in a free-text msg.
void net_relay_emit_cmd_result(const char *cmd_type, const char *cmd_id,
                                bool ok, const char *msg,
                                const char *data_json) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "type", "event");
    cJSON_AddStringToObject(root, "kind", "cmd.result");
    cJSON_AddStringToObject(root, "id",   cmd_id   ? cmd_id   : "");
    cJSON_AddStringToObject(root, "cmd",  cmd_type ? cmd_type : "");
    cJSON_AddBoolToObject(root,   "ok",   ok);
    cJSON_AddStringToObject(root, "msg",  msg      ? msg      : "");
    cJSON_AddNumberToObject(root, "ts",   (double)(esp_timer_get_time() / 1000));

    if (data_json && *data_json) {
        cJSON *data = cJSON_Parse(data_json);
        if (data) {
            cJSON_AddItemToObject(root, "data", data);
        } else {
            ESP_LOGW(TAG, "cmd.result data_json failed to parse, omitting");
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return;

    if (s_connected) {
        size_t len = strlen(json);
        // Goes through the single-sender queue so it doesn't race with
        // tick / preview on the WS client mutex (PR-F).
        enqueue(SEND_TEXT, json, len);
        ESP_LOGI(TAG, "cmd.result %s id=%s %s msg=\"%s\"%s",
                 cmd_type, cmd_id, ok ? "OK" : "FAIL", msg,
                 data_json ? " (+data)" : "");
    }
    free(json);
}

// Convenience wrapper for the dispatcher's own use: no data payload.
static inline void send_cmd_result(const char *cmd_type, const char *cmd_id,
                                    bool ok, const char *msg) {
    net_relay_emit_cmd_result(cmd_type, cmd_id, ok, msg, NULL);
}

// ---- Command dispatch ----
//
// Recognized commands (Phase 2):
//   thermal.ffc    — call hal_lepton_run_ffc()
//   device.reboot  — esp_restart() after sending result
//   capture.now    — STUB (Phase 3 needs SD session backend)
//   timelapse.start, timelapse.stop — STUB (Phase 3)
//   ping           — health-check, always OK
//
// Each command MUST emit exactly one result event referencing the cmd id.
static void dispatch_cmd(const cJSON *root) {
    const cJSON *cmd_field = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    const cJSON *id_field  = cJSON_GetObjectItemCaseSensitive(root, "id");
    const char *cmd = cJSON_IsString(cmd_field) ? cmd_field->valuestring : "";
    const char *id  = cJSON_IsString(id_field)  ? id_field->valuestring  : "";

    if (!cmd || !*cmd) {
        send_cmd_result("(unknown)", id, false, "missing cmd field");
        return;
    }

    ESP_LOGI(TAG, "cmd received: %s id=%s", cmd, id);

    if (strcmp(cmd, "ping") == 0) {
        send_cmd_result(cmd, id, true, "pong");
        return;
    }

    if (strcmp(cmd, "thermal.ffc") == 0) {
        esp_err_t err = hal_lepton_run_ffc();
        if (err == ESP_OK) {
            send_cmd_result(cmd, id, true, "FFC executed");
        } else {
            send_cmd_result(cmd, id, false, esp_err_to_name(err));
        }
        return;
    }

    if (strcmp(cmd, "device.reboot") == 0) {
        // Send result FIRST, then reboot. Brief delay so the WS frame
        // makes it onto the wire before esp_restart kills the radio.
        send_cmd_result(cmd, id, true, "rebooting in 500 ms");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        return;  // unreachable
    }

    // Forward anything else to the registered handler (capture.now,
    // timelapse.start/stop, settings.update, sessions.*, etc.). Handler
    // signature takes the payload as a void* to keep cJSON out of the
    // public header.
    //
    // DEFERRED return = handler enqueued the work to a background task
    // and will emit cmd.result itself when done (used by sessions.* so
    // SD scans don't block the websocket task). For DEFERRED, msg may
    // be empty or hold a queue-status string for logs only.
    if (s_cmd_handler) {
        char msg[160] = {0};
        net_relay_cmd_status_t st = s_cmd_handler(cmd, id, root, msg, sizeof(msg));
        switch (st) {
            case NET_RELAY_CMD_OK:
                send_cmd_result(cmd, id, true, msg);
                break;
            case NET_RELAY_CMD_FAIL:
                send_cmd_result(cmd, id, false, msg);
                break;
            case NET_RELAY_CMD_DEFERRED:
                ESP_LOGI(TAG, "cmd %s id=%s deferred to worker (%s)",
                         cmd, id, msg[0] ? msg : "no-msg");
                break;
        }
        return;
    }

    send_cmd_result(cmd, id, false, "unknown command (no handler registered)");
}

static void handle_text_frame(const char *data, size_t len) {
    if (len == 0 || len >= INBOUND_BUF_SIZE - 1) {
        ESP_LOGW(TAG, "rx text len=%u out of range", (unsigned)len);
        return;
    }
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) {
        ESP_LOGW(TAG, "rx text: invalid JSON (%u B)", (unsigned)len);
        return;
    }
    const cJSON *type_field = cJSON_GetObjectItemCaseSensitive(root, "type");
    const char *type = cJSON_IsString(type_field) ? type_field->valuestring : "";
    if (strcmp(type, "cmd") == 0) {
        dispatch_cmd(root);
    } else if (strcmp(type, "time") == 0) {
        // Relay pushes wall-clock right after hello so we don't depend
        // on NTP working through whatever NAT we're behind. Apply via
        // settimeofday so time(NULL) reflects epoch immediately.
        const cJSON *epoch_field = cJSON_GetObjectItemCaseSensitive(root, "epochMs");
        if (cJSON_IsNumber(epoch_field)) {
            int64_t epoch_ms = (int64_t)epoch_field->valuedouble;
            struct timeval tv = {
                .tv_sec  = (time_t)(epoch_ms / 1000),
                .tv_usec = (suseconds_t)((epoch_ms % 1000) * 1000),
            };
            settimeofday(&tv, NULL);
            ESP_LOGI(TAG, "system clock set from relay: epoch=%lld ms", (long long)epoch_ms);
        } else {
            ESP_LOGW(TAG, "rx time msg without numeric epochMs");
        }
    } else {
        ESP_LOGI(TAG, "rx text type=\"%s\" (no handler)", type);
    }
    cJSON_Delete(root);
}

static void on_event(void *arg, esp_event_base_t base, int32_t event_id,
                      void *event_data) {
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "connected");
            s_connected = true;
            s_inbound_len = 0;
            send_hello();
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "disconnected");
            s_connected = false;
            s_inbound_len = 0;
            break;
        case WEBSOCKET_EVENT_DATA:
            // op_code 0x01 = text, 0x02 = binary, 0x00 = continuation,
            // 0x09 = ping, 0x0A = pong. We only care about text for cmd.
            if (ev->op_code == 0x01 || ev->op_code == 0x00) {
                if (ev->data_len > 0 && ev->data_ptr) {
                    if (s_inbound_len + ev->data_len < INBOUND_BUF_SIZE) {
                        memcpy(s_inbound_buf + s_inbound_len, ev->data_ptr, ev->data_len);
                        s_inbound_len += ev->data_len;
                    } else {
                        ESP_LOGW(TAG, "inbound buffer overflow, dropping");
                        s_inbound_len = 0;
                    }
                }
                // ev->payload_offset + ev->data_len == payload_len → final fragment
                if (ev->payload_offset + ev->data_len >= ev->payload_len) {
                    if (s_inbound_len > 0) {
                        handle_text_frame(s_inbound_buf, s_inbound_len);
                    }
                    s_inbound_len = 0;
                }
            }
            break;
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "ws error");
            break;
        default:
            break;
    }
}

esp_err_t net_relay_start(void) {
    esp_websocket_client_config_t cfg = {
        .uri = CONFIG_GRASSHOPPER_RELAY_URL,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .task_stack = 6144,
        // Sized for VGA JPEG previews (~30-50 KB at q=12) plus our 24 B
        // header. esp_websocket_client_send_bin() chunks larger payloads
        // automatically, but a single big buffer is simpler.
        .buffer_size = 65536,
        // ---- Connection liveness ----
        // iPhone hotspot NAT silently drops idle TCP connections after
        // ~60-180 s. Without these, the WS appears alive until the next
        // send, then closes with code 1006. Three layers:
        //
        //   1. ping_interval_sec=15  — application-level WS PING every
        //      15 s; provokes a PONG so the NAT keeps the mapping warm.
        //   2. pingpong_timeout_sec=30 — if the device doesn't get a
        //      PONG within 30 s, drop and reconnect (don't sit there
        //      thinking we're connected).
        //   3. keep_alive_*            — TCP-layer keepalives so the
        //      kernel notices a dead peer even between WS frames.
        .ping_interval_sec       = 15,
        .pingpong_timeout_sec    = 30,
        .keep_alive_enable       = true,
        .keep_alive_idle         = 30,
        .keep_alive_interval     = 10,
        .keep_alive_count        = 3,
    };
    s_client = esp_websocket_client_init(&cfg);
    if (!s_client) return ESP_FAIL;

    ESP_ERROR_CHECK(esp_websocket_register_events(
        s_client, WEBSOCKET_EVENT_ANY, on_event, NULL));

    // Single-sender setup (PR-F). Spawned before client_start so the
    // sender is draining by the time the first send queues up.
    if (!s_send_queue) {
        s_send_queue = xQueueCreate(SEND_QUEUE_DEPTH, sizeof(send_msg_t));
        if (!s_send_queue) {
            ESP_LOGE(TAG, "send queue create failed");
            esp_websocket_client_destroy(s_client);
            s_client = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    s_sender_stop = false;
    if (!s_sender_task) {
        // Pinned to core 0 alongside the WS event task so we don't
        // share core 1 with the VoSPI reader.
        xTaskCreatePinnedToCore(sender_task, "ws-sender", 4096, NULL, 6,
                                 &s_sender_task, 0);
    }

    return esp_websocket_client_start(s_client);
}

void net_relay_stop(void) {
    // Tell sender to exit at next queue-receive boundary. The sender
    // can be blocked inside esp_websocket_client_send_*() for up to
    // 5000 ms (its tx-lock timeout), so the wait window has to be at
    // LEAST as long as that — otherwise we'd race past it, free the
    // queue and the client out from under an in-flight send and the
    // sender task would crash on use-after-free. 6000 ms covers the
    // 5 s send timeout plus a tick of slack.
    if (s_sender_task) {
        s_sender_stop = true;
        // Sender wakes every 250 ms when idle; when busy, blocks up
        // to 5 s inside send_*. Poll for self-deletion at 50 ms cadence.
        for (int i = 0; i < 120 && s_sender_task; i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (s_sender_task) {
            ESP_LOGW(TAG, "sender did not exit within 6 s — forcing teardown");
        }
    }
    send_queue_flush();
    if (s_send_queue) {
        vQueueDelete(s_send_queue);
        s_send_queue = NULL;
    }

    if (!s_client) return;
    // close + destroy. The relay sees a graceful close (not 1006)
    // because esp_websocket_client_close sends a CLOSE frame.
    esp_websocket_client_close(s_client, pdMS_TO_TICKS(1000));
    esp_websocket_client_stop(s_client);
    esp_websocket_client_destroy(s_client);
    s_client = NULL;
    s_connected = false;
    ESP_LOGI(TAG, "stopped");
}

// Drain the send queue, freeing any pending buffers. Used on stop /
// shutdown so we don't leak heap.
static void send_queue_flush(void) {
    if (!s_send_queue) return;
    send_msg_t m;
    while (xQueueReceive(s_send_queue, &m, 0) == pdTRUE) {
        if (m.buf) free(m.buf);
    }
}

// Sender task — single owner of esp_websocket_client_send_*. Drains
// the queue, sends each message, frees the heap buffer. Holds the
// WS client mutex (internally, inside send_*) for as long as each
// individual send takes; other senders no longer race on it because
// they only enqueue, they never call send_* themselves.
static void sender_task(void *arg) {
    (void)arg;
    while (!s_sender_stop) {
        send_msg_t m;
        if (xQueueReceive(s_send_queue, &m, pdMS_TO_TICKS(250)) != pdTRUE) {
            continue;
        }
        if (s_connected && s_client && m.buf) {
            int sent;
            if (m.kind == SEND_TEXT) {
                sent = esp_websocket_client_send_text(
                    s_client, (const char *)m.buf, (int)m.len,
                    pdMS_TO_TICKS(5000));
            } else {
                sent = esp_websocket_client_send_bin(
                    s_client, (const char *)m.buf, (int)m.len,
                    pdMS_TO_TICKS(5000));
            }
            if (sent <= 0) {
                ESP_LOGW(TAG, "sender: %s send failed (%u B)",
                         m.kind == SEND_TEXT ? "text" : "binary",
                         (unsigned)m.len);
            }
        }
        if (m.buf) free(m.buf);
    }
    // Drain remaining queue contents so we don't leak.
    send_queue_flush();
    s_sender_task = NULL;
    vTaskDelete(NULL);
}

// Enqueue a message for the sender task. Copies the payload to a
// freshly-allocated heap buffer so the caller can release/free its
// own buffer immediately after this returns. binary=true allocates
// from PSRAM (~50 KB preview frames), false from internal RAM
// (small JSON text fits comfortably).
static esp_err_t enqueue(send_kind_t kind, const void *src, size_t len) {
    if (!s_connected || !s_client || !s_send_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len == 0) return ESP_ERR_INVALID_ARG;
    uint32_t caps = (kind == SEND_BINARY)
                    ? MALLOC_CAP_SPIRAM
                    : MALLOC_CAP_8BIT;
    uint8_t *copy = (uint8_t *)heap_caps_malloc(len, caps);
    if (!copy) {
        // Fallback: if SPIRAM allocator says no for binary, try the
        // generic heap. Internal text alloc failures are real OOM and
        // should propagate.
        if (kind == SEND_BINARY) {
            copy = (uint8_t *)malloc(len);
        }
        if (!copy) return ESP_ERR_NO_MEM;
    }
    memcpy(copy, src, len);
    send_msg_t m = { .kind = kind, .buf = copy, .len = len };
    if (xQueueSend(s_send_queue, &m, 0) != pdTRUE) {
        // Queue full — drop. With SEND_QUEUE_DEPTH=16 and steady-state
        // ~2 msg/s, only happens if the link is genuinely too slow to
        // keep up; the right call is to drop the newest rather than
        // back-pressure callers (which would re-introduce the lock
        // contention we built the queue to eliminate).
        free(copy);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t net_relay_send(const char *json, size_t len) {
    return enqueue(SEND_TEXT, json, len);
}

esp_err_t net_relay_send_binary(const void *buf, size_t len) {
    return enqueue(SEND_BINARY, buf, len);
}

bool net_relay_is_connected(void) { return s_connected; }

void net_relay_register_cmd_handler(net_relay_cmd_handler_t fn) {
    s_cmd_handler = fn;
}
