# IDF SPI DMA retest — design note

## Context

Earlier in this project the conclusion was that ESP-IDF `spi_master`
"truncates 164-byte VoSPI reads to 4 bytes" on the ESP32-S3 GPIO-matrix
combination we use (SCK=41, MISO=42, CS=14). That conclusion drove us
to Arduino's `SPIClass.transferBytes()`, which has its own problems
(no DMA — `esp32-hal-spi.c` ~line 1017 chunks transfers larger than
64 bytes into multiple hardware transfers under one CS assertion).

Codex's review during the unified-diagnosis session flagged this as
worth re-investigating: "164-byte reads truncate to 4 bytes" smells
like a known IDF gotcha with `SPI_TRANS_USE_RXDATA` or `rxlength`
misconfiguration, not a real driver bug.

This note specifies what to verify and how, before we either commit
to an IDF DMA SPI rewrite or stay on Arduino's SPIClass.

## What to check (in order)

### 1. The `SPI_TRANS_USE_RXDATA` flag

When `SPI_TRANS_USE_RXDATA` is set in the `flags` field of an
`spi_transaction_t`, the driver writes received data into the inline
`rx_data[4]` buffer in the transaction struct **and ignores
`rx_buffer`**. Maximum 4 bytes. If our previous test set this flag (or
inherited it from a copied transaction), every read would truncate to
4 bytes regardless of `rxlength`.

**Verify**: The transaction struct must have `flags = 0` (or at least
must NOT include `SPI_TRANS_USE_RXDATA`), and `rx_buffer` must point to
the DMA-capable buffer.

### 2. The `length` and `rxlength` fields

For half-duplex receive on IDF spi_master:
- `length` is the **total transaction length in bits** (including any
  TX phase). If we're RX-only, this can be `0`.
- `rxlength` is the **receive length in bits**. Must be set explicitly
  to `164 * 8 = 1312`. If left at 0, the driver assumes `rxlength = length`.

Common mistake: setting `length = 164` (interpreted as 164 BITS, not
164 bytes — that's only 20.5 bytes received). Or setting both `length`
and `rxlength` to the byte count instead of bit count.

**Verify**: `length` and `rxlength` are in BITS. `rxlength` is set to 1312.

### 3. DMA-capable buffer

The receive buffer must be allocated with both `MALLOC_CAP_DMA` and
`MALLOC_CAP_INTERNAL` (PSRAM is not DMA-capable on ESP32-S3 for SPI).
Word-aligned. tCam-Mini does:

```c
lepPacketP = heap_caps_malloc(LEP_PKT_LENGTH, MALLOC_CAP_DMA);
```

Note: `MALLOC_CAP_DMA` already implies internal SRAM on ESP32-S3, but
being explicit doesn't hurt.

**Verify**: `heap_caps_malloc(164, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)`.
Confirm `esp_ptr_dma_capable()` returns true on the result.

### 4. Half-duplex device flag

Lepton VoSPI is RX-only. The device config needs:

```c
.flags = SPI_DEVICE_HALFDUPLEX
```

Without this, the driver expects full-duplex framing and may handle
the transaction differently, including how it interprets `length` vs
`rxlength`.

### 5. `cs_ena_pretrans`

tCam-Mini uses `cs_ena_pretrans = 10` — meaning hold CS low for 10 SPI
clock cycles before the first data clock. Lepton spec requires ≥1 SCK
of CS-low setup before MISO is valid. Without setup time, the first
MISO bit can be sampled before the Lepton drives it.

**Use**: `cs_ena_pretrans = 10` in `spi_device_interface_config_t`.

### 6. `spi_device_polling_transmit` vs `spi_device_transmit`

tCam-Mini uses `spi_device_polling_transmit()` (busy-wait) rather than
`spi_device_transmit()` (interrupt-driven). For 164 bytes at 16 MHz
(~82 µs), polling is faster — interrupt overhead dominates the actual
transfer time. Comment in tCam's source confirms they benchmarked and
chose polling.

**Use**: `spi_device_polling_transmit()`.

## Reference configuration to test

```c
// Bus init
spi_bus_config_t bus = {
    .mosi_io_num     = -1,           // RX-only
    .miso_io_num     = LEP_SPI_MISO, // GPIO 42
    .sclk_io_num     = LEP_SPI_SCK,  // GPIO 41
    .quadwp_io_num   = -1,
    .quadhd_io_num   = -1,
    .max_transfer_sz = 164,
};
ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

// Device
spi_device_interface_config_t dev = {
    .clock_speed_hz = 16000000,             // or 12 MHz, see board_pins.h
    .mode           = 3,
    .spics_io_num   = LEP_SPI_CS,           // GPIO 14
    .queue_size     = 1,
    .flags          = SPI_DEVICE_HALFDUPLEX,
    .cs_ena_pretrans= 10,
};
spi_device_handle_t spi;
ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &spi));

// Buffer
uint8_t *pkt = heap_caps_malloc(164, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
assert(pkt && esp_ptr_dma_capable(pkt));

// Per-packet read
spi_transaction_t t = {0};
t.length    = 0;          // RX-only
t.rxlength  = 164 * 8;    // 1312 bits
t.rx_buffer = pkt;        // do NOT set SPI_TRANS_USE_RXDATA
// flags = 0
ESP_ERROR_CHECK(spi_device_polling_transmit(spi, &t));
```

## Validation tests (before integrating into the reader task)

1. **Read 10 packets, log the bytes received per call.** Should be
   exactly 164 each. If any are 4, the `SPI_TRANS_USE_RXDATA` /
   `rxlength` issue is still present.
2. **Read 1000 packets in a tight loop with the Lepton powered.**
   Histogram byte0 lo-nibble. Should be bimodal at 0x0 (line packets)
   and 0xF (discard packets); spread across 1-14 means the byte
   alignment is still drifting (signal integrity, not driver).
3. **Compare timing**: measure microseconds per `spi_device_polling_transmit()`
   call. Should be ~82 µs at 16 MHz, ~110 µs at 12 MHz, plus ~5-10 µs
   overhead. If it's significantly more, something is blocking
   (other transactions, ISRs, or the device isn't actually configured
   for polling).
4. **CS waveform check** (if a logic analyzer is available): CS should
   stay LOW for the entire 1312 SCK cycles of a packet, with the
   `cs_ena_pretrans` setup time visible. No mid-packet glitches.

## When this is worth doing

- **Not yet.** The right order is: physical mitigations first (series
  resistor on SCK, twisted GND returns, VSYNC wired, isolated power),
  *then* this driver work. A clean physical link with Arduino's chunked
  reads is more likely to work than a fancy IDF driver with a noisy
  link.
- After the physical work, if Arduino SPIClass still shows
  intermittent valid:discard ratios, this is the next move.

## File locations affected (when implemented)

- `/home/jwood/Project-Grasshopper/firmware/components/hal_lepton/src/lepton_spi.cpp`
  — replace Arduino `SPIClass(FSPI).transferBytes()` with the IDF
  config above. Move from C++ to C if convenient.
- `/home/jwood/Project-Grasshopper/firmware/components/hal_lepton/src/lepton_vospi.c`
  — `vospi_read_packet()` calls into the SPI HAL; should be source-
  compatible with the new implementation.
- `board_pins.h` — `LEP_SPI_FREQ_HZ` may move to 16 MHz once the link
  is confirmed clean.

## Reference

- tCam-Mini SPI bus init: `tCam-Mini/firmware/components/sys/sys_utilities.c` ~line 132
- tCam-Mini SPI device + read: `tCam-Mini/firmware/components/lepton/vospi.c` ~line 92
- ESP-IDF SPI master docs: https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32s3/api-reference/peripherals/spi_master.html
