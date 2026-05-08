// Storage HAL — see hal_storage.h.

#include "hal_storage.h"
#include "board_pins.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "driver/sdmmc_host.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"

static const char *TAG = "hal_storage";

#define LFS_MOUNT  "/littlefs"
#define SD_MOUNT   "/sdcard"

static bool s_lfs_mounted = false;
static bool s_sd_mounted  = false;
static sdmmc_card_t *s_sd_card = NULL;
static SemaphoreHandle_t s_sd_mutex = NULL;

static void ensure_mutex(void) {
    if (!s_sd_mutex) s_sd_mutex = xSemaphoreCreateMutex();
}

esp_err_t hal_storage_littlefs_mount(void) {
    if (s_lfs_mounted) return ESP_OK;

    esp_vfs_littlefs_conf_t conf = {
        .base_path        = LFS_MOUNT,
        .partition_label  = "littlefs",
        .format_if_mount_failed = true,
        .dont_mount       = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "littlefs mount failed: %s", esp_err_to_name(err));
        return err;
    }
    s_lfs_mounted = true;

    size_t total = 0, used = 0;
    if (esp_littlefs_info(conf.partition_label, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "littlefs mounted at %s — %u/%u bytes used",
                 LFS_MOUNT, (unsigned)used, (unsigned)total);
    }
    return ESP_OK;
}

esp_err_t hal_storage_sd_mount(void) {
    if (s_sd_mounted) return ESP_OK;
    ensure_mutex();

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = SD_MMC_CLK;
    slot.cmd = SD_MMC_CMD;
    slot.d0  = SD_MMC_D0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };

    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT, &host, &slot, &mount,
                                             &s_sd_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed (%s) — card absent or unformatted",
                 esp_err_to_name(err));
        return err;
    }
    s_sd_mounted = true;
    ESP_LOGI(TAG, "SD mounted at %s — %llu MB", SD_MOUNT,
             ((uint64_t)s_sd_card->csd.capacity *
              (uint64_t)s_sd_card->csd.sector_size) / (1024 * 1024));
    return ESP_OK;
}

bool hal_storage_sd_present(void) { return s_sd_mounted; }

bool hal_storage_sd_lock(uint32_t timeout_ms) {
    ensure_mutex();
    if (!s_sd_mutex) return false;
    return xSemaphoreTake(s_sd_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void hal_storage_sd_unlock(void) {
    if (s_sd_mutex) xSemaphoreGive(s_sd_mutex);
}

void hal_storage_sd_stats(hal_storage_sd_stats_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!s_sd_mounted || !s_sd_card) return;

    FATFS *fs = NULL;
    DWORD free_clusters = 0;
    if (f_getfree("0:", &free_clusters, &fs) == FR_OK && fs) {
        uint64_t cluster_bytes = (uint64_t)fs->csize * 512;
        uint64_t total_clusters = (fs->n_fatent - 2);
        out->mounted     = true;
        out->total_bytes = total_clusters * cluster_bytes;
        out->free_bytes  = (uint64_t)free_clusters * cluster_bytes;
        out->used_bytes  = out->total_bytes - out->free_bytes;
    }
}

void hal_storage_lfs_stats(hal_storage_lfs_stats_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!s_lfs_mounted) return;
    size_t total = 0, used = 0;
    if (esp_littlefs_info("littlefs", &total, &used) == ESP_OK) {
        out->mounted     = true;
        out->total_bytes = total;
        out->used_bytes  = used;
    }
}

int hal_storage_sd_atomic_write(const char *path,
                                 const void *data, size_t len) {
    if (!s_sd_mounted || !path || !data) return -1;

    char tmp[256];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n <= 0 || (size_t)n >= sizeof(tmp)) return -1;

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        ESP_LOGE(TAG, "open(%s) failed: errno=%d", tmp, errno);
        return -1;
    }
    ssize_t w = write(fd, data, len);
    fsync(fd);
    close(fd);
    if (w < 0 || (size_t)w != len) {
        ESP_LOGE(TAG, "short write to %s (%d/%u)", tmp, (int)w, (unsigned)len);
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        // FATFS rename can fail if dest exists — fall back to delete+rename.
        ESP_LOGW(TAG, "rename(%s→%s) failed (errno=%d); retrying", tmp, path, errno);
        unlink(path);
        if (rename(tmp, path) != 0) {
            ESP_LOGE(TAG, "rename retry failed: errno=%d", errno);
            unlink(tmp);
            return -1;
        }
    }
    return (int)len;
}

esp_err_t hal_storage_sd_mkdir_p(const char *path) {
    if (!s_sd_mounted || !path) return ESP_ERR_INVALID_STATE;

    char tmp[256];
    size_t n = strlen(path);
    if (n >= sizeof(tmp)) return ESP_ERR_INVALID_ARG;
    memcpy(tmp, path, n + 1);

    for (size_t i = 1; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                ESP_LOGE(TAG, "mkdir(%s) failed: errno=%d", tmp, errno);
                return ESP_FAIL;
            }
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "mkdir(%s) failed: errno=%d", tmp, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}
