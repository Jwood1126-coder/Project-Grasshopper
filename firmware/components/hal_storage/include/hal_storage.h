#pragma once

// Storage HAL — LittleFS for boot/UI assets, SD_MMC for capture sessions.
//
// Mounts:
//   /littlefs           ← built from `firmware/main/assets/` at flash time
//   /sdcard             ← user-inserted card (lazy-mounted; absence is OK)
//
// All SD writes/reads must hold sd_mutex (hal_storage_sd_lock /
// hal_storage_sd_unlock) so the timelapse writer + sync-capture handler
// + session indexer can't trample each other on the FATFS layer.

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// LittleFS — always present; partition is mandatory in partitions.csv.
esp_err_t hal_storage_littlefs_mount(void);

// SD_MMC — best-effort; returns ESP_FAIL if no card. Safe to call again
// later (e.g. after the user inserts a card).
esp_err_t hal_storage_sd_mount(void);
bool      hal_storage_sd_present(void);

// Cross-task mutex for SD operations. Always pair lock with unlock.
// Returns false on lock failure (timeout > 5s — caller should treat as
// "SD wedged" and surface an error).
bool hal_storage_sd_lock(uint32_t timeout_ms);
void hal_storage_sd_unlock(void);

// Live capacity. Zeroes the outputs if SD isn't mounted.
typedef struct {
    bool     mounted;
    uint64_t total_bytes;
    uint64_t used_bytes;
    uint64_t free_bytes;
} hal_storage_sd_stats_t;
void hal_storage_sd_stats(hal_storage_sd_stats_t *out);

// LittleFS capacity (assets partition, ~512 KB).
typedef struct {
    bool     mounted;
    size_t   total_bytes;
    size_t   used_bytes;
} hal_storage_lfs_stats_t;
void hal_storage_lfs_stats(hal_storage_lfs_stats_t *out);

// Atomic file write to SD: writes to <path>.tmp, fsyncs, then renames
// over <path>. Caller must hold sd_lock. Returns the bytes written or
// negative on error. Does NOT create parent dirs.
int hal_storage_sd_atomic_write(const char *path,
                                const void *data, size_t len);

// Recursive `mkdir -p` on SD. Caller must hold sd_lock.
esp_err_t hal_storage_sd_mkdir_p(const char *path);

#ifdef __cplusplus
}
#endif
