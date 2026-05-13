#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// One-shot firmware update over HTTPS. Spawns a background task so
// the caller (cmd dispatcher) returns immediately. Progress + final
// outcome are emitted as cmd.result events with the supplied cmd_id.
//
// On success, the task calls esp_restart() and the device reboots
// into the new image. The new image starts in pending-verify state;
// our app's boot path (ota_pending_verify_arm) is responsible for
// either marking it valid (success → permanent) or letting it
// rollback (failure → next reboot returns to the previous slot).
esp_err_t ota_start_https(const char *cmd_id, const char *url);

// Called once early from app_main after WiFi + relay are up and the
// first relay connect has succeeded. If this image is in
// pending-verify state, schedules a one-shot timer that marks the
// image valid after `confirm_ms` of being connected. If the device
// crashes / disconnects before then, the pending mark stays and the
// next bootloader pass rolls back to the previous slot.
void ota_pending_verify_arm(uint32_t confirm_ms);

#ifdef __cplusplus
}
#endif
