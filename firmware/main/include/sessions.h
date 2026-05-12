#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Sessions worker — services sessions.list / sessions.get /
// session.read_file commands on a background task so heavy SD work
// doesn't run inline in the websocket dispatcher.
//
// All three commands return their result asynchronously via
// net_relay_emit_cmd_result(); the dispatcher returns DEFERRED.

esp_err_t sessions_init(void);

// Enqueue a sessions.list request. UI gets back a list of all
// sessions in /sdcard/timelapse/, newest first, capped at SESS_LIST_MAX.
esp_err_t sessions_enqueue_list(const char *cmd_id);

// Enqueue a sessions.get request. UI gets back the session.json meta
// plus up to SESS_CAPTURES_MAX entries from captures.jsonl. If the
// session has more captures, `truncated` will be true in the response
// and the UI can fetch the full file via session.read_file.
esp_err_t sessions_enqueue_get(const char *cmd_id, const char *session_id);

// Enqueue a session.read_file request. Returns a chunk of <= max_len
// bytes starting at offset, base64-encoded. UI loops until eof=true.
// max_len is clamped to SESS_FILE_CHUNK_MAX.
esp_err_t sessions_enqueue_read_file(const char *cmd_id,
                                      const char *session_id,
                                      const char *filename,
                                      uint32_t offset, uint32_t max_len);

#ifdef __cplusplus
}
#endif
