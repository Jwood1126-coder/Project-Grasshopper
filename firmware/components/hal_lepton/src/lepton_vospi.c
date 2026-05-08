// Lepton 3.1R VoSPI frame reader.
// IDF v5.x port of Fox's lepton_vospi.h. Algorithm + state machine +
// franken-frame guards (35→45 ms whole-frame deadline, 28 ms per-segment
// gap, content-based splice detection at row 30/60/90, abort_frame
// memset on every reset path) preserved verbatim.
//
// Platform changes:
//   SPIClass(FSPI)         →  lepton_spi.cpp (Arduino SPIClass — IDF
//                              spi_master truncates at byte 4 here)
//   digitalWrite           →  gpio_set_level (CS handled inside lepton_spi)
//   ps_malloc              →  heap_caps_malloc(MALLOC_CAP_SPIRAM)
//   millis()               →  esp_timer_get_time()/1000
//   Serial.printf          →  ESP_LOG*
//   xTaskCreatePinnedToCore + Semaphore — unchanged (FreeRTOS).

#include "lepton_internal.h"
#include "lepton_spi.h"
#include "board_pins.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "lep_vospi";

// ---- VoSPI constants ----
#define VOSPI_PACKETS_PER_SEG  60
#define VOSPI_PIXELS_PER_PKT   80
#define VOSPI_HEADER_BYTES      4
#define VOSPI_PENDING_LINES    20

// Buffers
static uint16_t *s_pending = NULL;          // PSRAM, 20 lines × 80 px × 2B = 3200B
static uint16_t *s_frame[2] = {NULL, NULL}; // PSRAM, 38400B each
static uint8_t  *s_pkt = NULL;              // 164B packet buffer (no special caps needed —
                                            // Arduino's SPIClass copies byte-by-byte from FIFO)
static volatile int s_write_idx = 0;

// Mutex protecting frame counter + write index
static SemaphoreHandle_t s_mutex = NULL;

// Reader task
static TaskHandle_t s_task = NULL;
static volatile bool s_start_reading = false;

// Stats
static volatile uint32_t s_frame_counter = 0;
static volatile uint32_t s_total_packets = 0;
static volatile uint32_t s_valid_packets = 0;
static volatile uint32_t s_discard_packets = 0;
static volatile uint32_t s_diag_sync_entries = 0;
static volatile uint32_t s_diag_line_mismatch = 0;
static volatile uint32_t s_diag_seg_not1 = 0;
static volatile uint32_t s_diag_seg_mismatch = 0;
static volatile uint32_t s_diag_frame_timeout = 0;
static volatile uint32_t s_diag_seg_zero = 0;
static volatile uint32_t s_diag_splice_detected = 0;
static volatile uint32_t s_hardware_resets = 0;
static volatile uint32_t s_last_frame_ms = 0;
static volatile uint8_t  s_state_code = 0;
static volatile uint8_t  s_last_seg_ids[4] = {0, 0, 0, 0};

// FFC timestamp (set by hal_lepton.c when an FFC runs)
volatile uint32_t lep_last_ffc_ms = 0;

static inline uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// SPI is owned by lepton_spi.cpp — see lepton_spi_init() below.

// ---- Power-cycle the Lepton via MOSFET (fox approach) ----

static void vospi_power_cycle_hardware(uint32_t boot_wait_ms) {
    s_hardware_resets++;
    gpio_set_level(LEP_SPI_CS, 1);

    if (LEP_POWER_PIN >= 0) {
        ESP_LOGW(TAG, "power-cycle Lepton via MOSFET (gpio %d)", LEP_POWER_PIN);
        gpio_config_t mos_cfg = {
            .pin_bit_mask = 1ULL << LEP_POWER_PIN,
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&mos_cfg);
        gpio_set_level(LEP_POWER_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
        gpio_set_level(LEP_POWER_PIN, 1);
    } else {
        ESP_LOGW(TAG, "no Lepton reset mechanism available");
        return;
    }

    ESP_LOGI(TAG, "waiting %u ms for Lepton boot...", (unsigned)boot_wait_ms);
    vTaskDelay(pdMS_TO_TICKS(boot_wait_ms));

    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_frame_counter = 0;
        s_last_frame_ms = 0;
        xSemaphoreGive(s_mutex);
    }
}

// ---- Read one VoSPI packet (164 B) ----

// DEBUG: histogram of byte0 values seen, dumped every N packets.
// Tells us what the SPI is actually reading — bus garbage looks like
// FF/00 dominant; Lepton-in-discard-mode looks like 0xF? / 0x?F mix;
// Lepton-streaming looks like a mix of 00..03 (line numbers), 14
// (line 20 segment-1), 24, 34, 44 (line 20 of segments 2-4), and
// 0xF? for inter-frame discards.
static uint32_t s_dbg_byte0_hist[16] = {0};
static uint32_t s_dbg_byte1_max = 0;

static bool vospi_read_packet(uint8_t *out_line, uint8_t *out_seg) {
    *out_seg = 0;
    *out_line = 0;

    if (!lepton_spi_read_packet(s_pkt, LEP_PKT_LEN)) return false;

    // DEBUG: tally low nibble of byte0.
    s_dbg_byte0_hist[s_pkt[0] & 0x0F]++;
    if (s_pkt[1] > s_dbg_byte1_max) s_dbg_byte1_max = s_pkt[1];

    // Discard packet?  byte0 low nibble == 0x0F.
    if ((s_pkt[0] & 0x0F) == 0x0F) return false;

    uint16_t pn = (((uint16_t)(s_pkt[0] & 0x0F)) << 8) | s_pkt[1];
    if (pn >= VOSPI_PACKETS_PER_SEG) return false;

    *out_line = (uint8_t)pn;
    if (*out_line == 20) *out_seg = (s_pkt[0] >> 4) & 0x07;
    return true;
}

// Called from the periodic-stats path so we get a histogram dump.
void lepton_vospi_dbg_dump(void) {
    char buf[200] = {0};
    int p = 0;
    for (int i = 0; i < 16; i++) {
        p += snprintf(buf + p, sizeof(buf) - p, "%lu ",
                      (unsigned long)s_dbg_byte0_hist[i]);
    }
    ESP_LOGI("lep_dbg", "byte0 lo-nib hist: %s | byte1_max=%lu",
             buf, (unsigned long)s_dbg_byte1_max);
}

// ---- Pixel storage ----

static void vospi_store_pixels(uint16_t *frame, int segment, int line) {
    int pkt_index = (segment - 1) * VOSPI_PACKETS_PER_SEG + line;
    int pixel_offset = pkt_index * VOSPI_PIXELS_PER_PKT;
    if (pixel_offset + VOSPI_PIXELS_PER_PKT > LEP_PIXELS) return;
    uint8_t *data = s_pkt + VOSPI_HEADER_BYTES;
    for (int i = 0; i < VOSPI_PIXELS_PER_PKT; i++) {
        frame[pixel_offset + i] = ((uint16_t)data[i * 2] << 8) | data[i * 2 + 1];
    }
}

static void vospi_store_pending(int line) {
    if (line >= VOSPI_PENDING_LINES) return;
    int off = line * VOSPI_PIXELS_PER_PKT;
    uint8_t *data = s_pkt + VOSPI_HEADER_BYTES;
    for (int i = 0; i < VOSPI_PIXELS_PER_PKT; i++) {
        s_pending[off + i] = ((uint16_t)data[i * 2] << 8) | data[i * 2 + 1];
    }
}

static void vospi_commit_pending(uint16_t *frame) {
    memcpy(frame, s_pending, VOSPI_PENDING_LINES * VOSPI_PIXELS_PER_PKT * sizeof(uint16_t));
}

// ---- Resync (CS high, wait >185ms per Lepton datasheet) ----

static void vospi_force_resync(void) {
    gpio_set_level(LEP_SPI_CS, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
}

// ---- Reset assembly state ----

static void vospi_reset_assembly(int *expect_seg, int *expect_line,
                                 uint32_t *frame_start, uint32_t *seg_start,
                                 uint8_t *seg_ids) {
    *expect_seg = 1;
    *expect_line = 0;
    *frame_start = 0;
    *seg_start = 0;
    seg_ids[0] = seg_ids[1] = seg_ids[2] = seg_ids[3] = 0xFF;
}

// Frankenframe fix from Fox's senior review: zero the in-flight buffer
// on every mid-frame reset path so subsequent partial assemblies can't
// commit a clean-looking 3-band splice.
static inline void vospi_abort_frame(uint16_t *frame,
                                     int *expect_seg, int *expect_line,
                                     uint32_t *frame_start, uint32_t *seg_start,
                                     uint8_t *seg_ids) {
    if (frame) memset(frame, 0, LEP_FRAME_BYTES);
    vospi_reset_assembly(expect_seg, expect_line, frame_start, seg_start, seg_ids);
}

// ---- Content-based splice detection ----
//
// Examines inter-row absolute diffs at the three segment boundaries
// (rows 30/60/90). Cross-broadcast splice → seam → boundary diff
// >> baseline diff. ~960 abs-diffs per frame. Verbatim from Fox.
static bool vospi_frame_has_splice(const uint16_t *frame) {
    static const int splice_rows[3] = {30, 60, 90};
    static const int baseline_y[3][2] = {
        {29, 32}, {59, 62}, {89, 92},
    };

    for (int i = 0; i < 3; i++) {
        int sy = splice_rows[i];

        bool any_zero_row = false;
        const int probe_ys[5] = { sy - 1, sy,
                                  baseline_y[i][0] - 1,
                                  baseline_y[i][1] - 1,
                                  baseline_y[i][1] };
        for (int p = 0; p < 5; p++) {
            int y = probe_ys[p];
            uint32_t row_or = 0;
            for (int x = 0; x < LEP_W; x++) row_or |= frame[y * LEP_W + x];
            if (row_or == 0) { any_zero_row = true; break; }
        }
        if (any_zero_row) continue;

        int32_t boundary_diff = 0;
        for (int x = 0; x < LEP_W; x++) {
            int32_t a = (int32_t)frame[sy * LEP_W + x];
            int32_t b = (int32_t)frame[(sy - 1) * LEP_W + x];
            int32_t d = a - b;
            boundary_diff += (d < 0) ? -d : d;
        }

        int32_t base_total = 0;
        for (int k = 0; k < 2; k++) {
            int by = baseline_y[i][k];
            int32_t s = 0;
            for (int x = 0; x < LEP_W; x++) {
                int32_t a = (int32_t)frame[by * LEP_W + x];
                int32_t b = (int32_t)frame[(by - 1) * LEP_W + x];
                int32_t d = a - b;
                s += (d < 0) ? -d : d;
            }
            base_total += s;
        }
        int32_t baseline_avg = base_total / 2;

        // boundary > 2.5 × baseline AND > 8000 absolute floor.
        if (boundary_diff > 8000 &&
            (int64_t)boundary_diff * 2 > (int64_t)baseline_avg * 5) {
            return true;
        }
    }
    return false;
}

// ---- Reader task on Core 1 ----

static void vospi_task(void *arg) {
    ESP_LOGI(TAG, "reader task on core %d, awaiting start signal",
             (int)xPortGetCoreID());

    while (!s_start_reading) {
        s_state_code = 0;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "start signal received, entering read loop");

    vospi_force_resync();

    int wi = s_write_idx;
    uint16_t *frame = s_frame[wi];
    bool logged_first = false;
    int sync_fail_count = 0;

    enum { SYNC, READING } state = SYNC;
    int expect_seg = 1, expect_line = 0;
    int discard_run = 0;
    uint8_t cur_seg_ids[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    const uint32_t FRAME_DEADLINE_MS = 45;
    const uint32_t SEGMENT_GAP_LIMIT_MS = 28;
    uint32_t frame_start_ms = 0;
    uint32_t last_segment_start_ms = 0;

    while (1) {
        s_state_code = (state == SYNC) ? 1 : 2;

        if (!s_start_reading) {
            s_state_code = 3;
            ESP_LOGW(TAG, "reader paused");
            while (!s_start_reading) vTaskDelay(pdMS_TO_TICKS(100));
            ESP_LOGI(TAG, "reader resumed");
            vospi_force_resync();
            state = SYNC;
            discard_run = 0;
            vospi_abort_frame(frame, &expect_seg, &expect_line,
                              &frame_start_ms, &last_segment_start_ms, cur_seg_ids);
        }

        uint8_t line, seg;
        bool valid = vospi_read_packet(&line, &seg);
        s_total_packets++;
        if (valid) s_valid_packets++; else s_discard_packets++;



        // Periodic stat dump (every ~10 k packets so we get fast feedback).
        if (s_total_packets % 10000 == 0) {
            extern void lepton_vospi_dbg_dump(void);
            lepton_vospi_dbg_dump();
        }
        if (s_total_packets % 180000 == 0) {
            ESP_LOGI(TAG, "frames=%lu total=%lu valid=%lu discard=%lu",
                     (unsigned long)s_frame_counter,
                     (unsigned long)s_total_packets,
                     (unsigned long)s_valid_packets,
                     (unsigned long)s_discard_packets);
            ESP_LOGI(TAG, "diag: sync=%lu line_mm=%lu seg_not1=%lu seg_mm=%lu fto=%lu sz=%lu splice=%lu hw=%lu",
                     (unsigned long)s_diag_sync_entries,
                     (unsigned long)s_diag_line_mismatch,
                     (unsigned long)s_diag_seg_not1,
                     (unsigned long)s_diag_seg_mismatch,
                     (unsigned long)s_diag_frame_timeout,
                     (unsigned long)s_diag_seg_zero,
                     (unsigned long)s_diag_splice_detected,
                     (unsigned long)s_hardware_resets);
        }

        switch (state) {
        case SYNC:
            if (!valid) {
                discard_run++;
                if (discard_run > 1000) {
                    discard_run = 0;
                    sync_fail_count++;
                    if (sync_fail_count % 5 == 0) {
                        ESP_LOGW(TAG, "sync attempt %d (valid=%lu discard=%lu)",
                                 sync_fail_count,
                                 (unsigned long)s_valid_packets,
                                 (unsigned long)s_discard_packets);
                    }
                    vospi_force_resync();

                    // Codex-improved recovery rule: track frames committed
                    // since the last reset, not cumulative valid packets.
                    // The old guard `s_valid_packets == 0` made recovery
                    // impossible once we'd seen ONE valid packet ever —
                    // exactly the half-alive state we got stuck in.
                    //
                    // New rule: if we've gone ≥600 sync attempts (~10 min
                    // at ~1 s each) without committing a single frame
                    // since the last reset attempt, the Lepton is
                    // genuinely stuck and a reset is worth trying.
                    static uint32_t s_frames_at_last_reset = 0;
                    bool no_progress = (s_frame_counter == s_frames_at_last_reset);
                    if (sync_fail_count >= 600 && no_progress) {
                        ESP_LOGW(TAG, "persistent failure — resetting Lepton hardware");
                        s_frames_at_last_reset = s_frame_counter;
                        vospi_power_cycle_hardware(5000);
                        vospi_force_resync();
                        vospi_abort_frame(frame, &expect_seg, &expect_line,
                                          &frame_start_ms, &last_segment_start_ms,
                                          cur_seg_ids);
                        sync_fail_count = 0;
                    }
                }
                break;
            }
            discard_run = 0;
            if (line == 0) {
                expect_seg = 0;
                expect_line = 1;
                vospi_store_pending(0);
                frame_start_ms = now_ms();
                state = READING;
            }
            break;

        case READING:
            if (!valid) break;  // discard packets allowed mid-stream

            if (line != expect_line) {
                if (expect_line == 0) break;  // between segments — ignore
                s_diag_line_mismatch++;
                s_diag_sync_entries++;
                vospi_abort_frame(frame, &expect_seg, &expect_line,
                                  &frame_start_ms, &last_segment_start_ms, cur_seg_ids);
                state = SYNC;
                break;
            }

            if (expect_seg == 1 && expect_line == 0 && frame_start_ms == 0) {
                frame_start_ms = now_ms();
            }

            if (line == 20) {
                if (expect_seg >= 1 && expect_seg <= 4)
                    cur_seg_ids[expect_seg - 1] = seg;

                if (expect_seg == 0) {
                    // Lepton 3.x emits a "segment 0" duplicate frame; wait
                    // for a real seg 1 before committing pending lines.
                    if (seg != 1) {
                        if (seg == 0) s_diag_seg_zero++; else s_diag_seg_not1++;
                        s_diag_sync_entries++;
                        vospi_abort_frame(frame, &expect_seg, &expect_line,
                                          &frame_start_ms, &last_segment_start_ms, cur_seg_ids);
                        state = SYNC;
                        break;
                    }
                    expect_seg = 1;
                    cur_seg_ids[0] = seg;
                    vospi_commit_pending(frame);
                } else if (seg != 0 && seg != expect_seg) {
                    s_diag_seg_mismatch++;
                    s_diag_sync_entries++;
                    vospi_abort_frame(frame, &expect_seg, &expect_line,
                                      &frame_start_ms, &last_segment_start_ms, cur_seg_ids);
                    state = SYNC;
                    break;
                } else if (seg == 0) {
                    // Some hardware emits all-zero seg IDs; lax accept
                    // (splice detector catches any mis-stitches).
                    s_diag_seg_zero++;
                }
            }

            if (expect_seg >= 1) vospi_store_pixels(frame, expect_seg, line);
            else                 vospi_store_pending(line);

            expect_line++;

            // Per-segment gap check.
            if (expect_line == 1) {
                uint32_t now = now_ms();
                if (expect_seg >= 2 && last_segment_start_ms != 0 &&
                    (now - last_segment_start_ms) > SEGMENT_GAP_LIMIT_MS) {
                    s_diag_frame_timeout++;
                    s_diag_sync_entries++;
                    state = SYNC;
                    vospi_abort_frame(frame, &expect_seg, &expect_line,
                                      &frame_start_ms, &last_segment_start_ms, cur_seg_ids);
                    break;
                }
                last_segment_start_ms = now;
            }

            if (expect_line >= VOSPI_PACKETS_PER_SEG) {
                // Whole-frame deadline check (catches splices that span
                // a Lepton broadcast period).
                if (frame_start_ms != 0 &&
                    (now_ms() - frame_start_ms) > FRAME_DEADLINE_MS) {
                    s_diag_frame_timeout++;
                    s_diag_sync_entries++;
                    state = SYNC;
                    vospi_abort_frame(frame, &expect_seg, &expect_line,
                                      &frame_start_ms, &last_segment_start_ms, cur_seg_ids);
                    break;
                }

                if (expect_seg == 4) {
                    // Strict seg-ID check (only when ALL segs reported nonzero).
                    bool all_nonzero = true;
                    for (int i = 0; i < 4; i++)
                        if (cur_seg_ids[i] == 0) { all_nonzero = false; break; }
                    if (all_nonzero) {
                        bool seq_ok = (cur_seg_ids[0] == 1 && cur_seg_ids[1] == 2 &&
                                       cur_seg_ids[2] == 3 && cur_seg_ids[3] == 4);
                        if (!seq_ok) {
                            s_diag_seg_mismatch++;
                            s_diag_sync_entries++;
                            vospi_abort_frame(frame, &expect_seg, &expect_line,
                                              &frame_start_ms, &last_segment_start_ms, cur_seg_ids);
                            state = SYNC;
                            break;
                        }
                    }

                    // Content-based splice detection.
                    if (vospi_frame_has_splice(frame)) {
                        s_diag_splice_detected++;
                        s_diag_sync_entries++;
                        vospi_abort_frame(frame, &expect_seg, &expect_line,
                                          &frame_start_ms, &last_segment_start_ms, cur_seg_ids);
                        state = SYNC;
                        break;
                    }

                    // Frame complete — flip buffers.
                    memcpy((void *)s_last_seg_ids, cur_seg_ids, 4);
                    xSemaphoreTake(s_mutex, portMAX_DELAY);
                    s_write_idx = 1 - wi;
                    s_frame_counter++;
                    s_last_frame_ms = now_ms();
                    xSemaphoreGive(s_mutex);

                    if (!logged_first) {
                        ESP_LOGI(TAG, "first frame! seg ids: %d %d %d %d",
                                 cur_seg_ids[0], cur_seg_ids[1],
                                 cur_seg_ids[2], cur_seg_ids[3]);
                        logged_first = true;
                    }

                    wi = s_write_idx;
                    frame = s_frame[wi];
                    sync_fail_count = 0;
                    cur_seg_ids[0] = cur_seg_ids[1] = cur_seg_ids[2] = cur_seg_ids[3] = 0xFF;
                    vTaskDelay(1);    // feed watchdog once per frame
                    expect_seg = 1;
                    expect_line = 0;
                    frame_start_ms = 0;
                } else {
                    expect_seg++;
                    expect_line = 0;
                }
            }
            break;
        }

        // Yield: SYNC sleeps to feed watchdog; READING just yields.
        if (state == SYNC) vTaskDelay(1);
        else if (expect_line % 30 == 0) taskYIELD();
    }
}

// ---- Public init ----

esp_err_t lepton_vospi_init(void) {
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) return ESP_ERR_NO_MEM;
    }

    if (!s_frame[0]) s_frame[0] = heap_caps_malloc(LEP_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_frame[1]) s_frame[1] = heap_caps_malloc(LEP_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_frame[0] || !s_frame[1]) {
        ESP_LOGE(TAG, "frame buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }
    memset(s_frame[0], 0, LEP_FRAME_BYTES);
    memset(s_frame[1], 0, LEP_FRAME_BYTES);

    if (!s_pending) {
        s_pending = heap_caps_malloc(VOSPI_PENDING_LINES * VOSPI_PIXELS_PER_PKT * sizeof(uint16_t),
                                     MALLOC_CAP_SPIRAM);
    }
    if (!s_pending) {
        ESP_LOGE(TAG, "pending buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }
    memset(s_pending, 0, VOSPI_PENDING_LINES * VOSPI_PIXELS_PER_PKT * sizeof(uint16_t));

    if (!s_pkt) {
        s_pkt = heap_caps_malloc(LEP_PKT_LEN, MALLOC_CAP_INTERNAL);
    }
    if (!s_pkt) {
        ESP_LOGE(TAG, "packet buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = lepton_spi_init();
    if (err != ESP_OK) return err;

    if (!s_task) {
        BaseType_t r = xTaskCreatePinnedToCore(vospi_task, "vospi", 4096, NULL,
                                               10, &s_task, 1);
        if (r != pdPASS) {
            ESP_LOGE(TAG, "reader task create failed");
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "VoSPI ready");
    return ESP_OK;
}

void lepton_vospi_start(void) {
    ESP_LOGI(TAG, "start signal");
    s_start_reading = true;
}

bool lepton_vospi_get_frame(uint16_t *dst) {
    if (s_frame_counter == 0) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int read_idx = 1 - s_write_idx;
    memcpy(dst, s_frame[read_idx], LEP_FRAME_BYTES);
    xSemaphoreGive(s_mutex);
    return true;
}

uint32_t lepton_vospi_frame_count(void) { return s_frame_counter; }

const char *lepton_vospi_state_name(uint8_t state) {
    switch (state) {
    case 0: return "waiting";
    case 1: return "sync";
    case 2: return "reading";
    case 3: return "paused";
    default: return "unknown";
    }
}

void lepton_vospi_get_stats(hal_lepton_stats_t *out) {
    if (!out) return;
    out->frames = s_frame_counter;
    out->totalPackets = s_total_packets;
    out->validPackets = s_valid_packets;
    out->discardPackets = s_discard_packets;
    out->syncEntries = s_diag_sync_entries;
    out->lineMismatch = s_diag_line_mismatch;
    out->segNot1 = s_diag_seg_not1;
    out->segMismatch = s_diag_seg_mismatch;
    out->frameTimeout = s_diag_frame_timeout;
    out->segZero = s_diag_seg_zero;
    out->spliceDetected = s_diag_splice_detected;
    out->hardwareResets = s_hardware_resets;
    out->lastFrameMs = s_last_frame_ms;
    out->lastFFCMs = lep_last_ffc_ms;
    out->state = s_state_code;
    out->started = s_start_reading;
    out->ready = s_frame_counter > 0;
    for (int i = 0; i < 4; i++) out->lastSegIds[i] = s_last_seg_ids[i];
}
