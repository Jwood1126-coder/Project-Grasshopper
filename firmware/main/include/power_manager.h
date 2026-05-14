#pragma once

// power_manager: peripheral shutdown + deep-sleep entry.
//
// Owns the order in which peripherals get disabled, powered off,
// and unmounted before esp_deep_sleep_start(). Separated from
// ds_scheduler so the deep-sleep state machine doesn't have to
// know about Lepton MOSFETs, SD unmount, OLED I2C, etc.
//
// All entry points are safe to call when the corresponding
// peripheral wasn't brought up (no-op if it isn't running).

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bring everything down in dependency order:
//   - thermal preview / VoSPI reader (RAM dies on sleep anyway, but
//     stop logging so the last few ms aren't noisy)
//   - Lepton power-off (MOSFET LOW)
//   - camera deinit (if vis was active)
//   - SD unmount + fsync
//   - OLED off
//   - Wi-Fi off
//   - GPIOs to safe state
//
// Tolerant of double-calls and of subsystems that were never
// initialized (deep-sleep wake handlers don't bring up Wi-Fi/OLED/
// preview, so most no-ops on the wake path).
void power_manager_prep_for_sleep(bool vis_was_active,
                                   bool therm_was_active);

// Arm the wake timer and call esp_deep_sleep_start(). Does not
// return. `sleep_us` is clamped to [1ms, 1 hour] to catch obvious
// math bugs (passing 0 would loop forever; passing INT64_MAX would
// effectively be permanent).
void power_manager_enter_deep_sleep(uint64_t sleep_us) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
