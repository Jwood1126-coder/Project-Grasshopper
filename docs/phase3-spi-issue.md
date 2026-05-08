# Phase 3 — VoSPI receive on IDF v5.3 spi_master

## Symptom

VoSPI reader runs, packets clock in, but frames never assemble. With
the chunked-polling workaround in place, the device reaches
`validPackets ≈ 74 / 4366` over ~30 s and `lineMismatch ≈ 26`. It does
sometimes pull complete-looking packets (raw byte dump showed real
pixel data at byte 160 of a `line=30` packet), but most reads land
during Lepton's idle/discard state and the reader's per-packet read
time (~246 μs) is too slow to keep up with Lepton's ~9000 pps stream
once in READING mode.

## What we ruled out (with raw byte dumps)

Single 164-byte transactions via `spi_device_transmit` with DMA
(`SPI_DMA_CH_AUTO`) on these GPIO-matrix pins (SCK=41, MISO=42)
deliver only the first 4 bytes of MISO data; bytes 4..163 read as
zero. Verified with 0xAA pre-fill — the SPI driver IS writing zeros
to those bytes (not just leaving them untouched). SPI clock measured
running for 132 μs total / ~80 μs of actual SCK, which matches 1312
bits at 16 MHz — so SCK runs the full duration but data isn't
captured into rx_buffer past byte 4.

Same 4-byte truncation regardless of:

- `spi_device_transmit` (DMA queued) vs `spi_device_polling_transmit`
- `SPI_DMA_CH_AUTO` vs `SPI_DMA_DISABLED`
- Full-duplex (length=1312, dummy zero TX) vs half-duplex (length=0,
  rxlength=1312, `SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_NO_DUMMY`)
- `SPI2_HOST` (FSPI) vs `SPI3_HOST` (HSPI)
- Manual CS (`spics_io_num=-1`) vs driver-managed CS (driver-managed
  gave reliably all-zero — driver wasn't asserting CS the way we
  expected).
- MISO pull-up — tail bytes still 0x00, so the line is actively
  driven low somewhere, not floating.
- MOSFET power-cycle (1 s OFF, then ON, 2 s settle).
- CCI fully configured vs CCI skipped.
- Buffers in DMA-capable internal SRAM (`MALLOC_CAP_DMA |
  MALLOC_CAP_INTERNAL`).

## Workaround in place: chunked polling

Three back-to-back `spi_device_polling_transmit` calls of 64 + 64 + 36
bytes with manual CS held low across all three. Lepton stalls when
SCK pauses between chunks and resumes from the next bit, so we don't
lose bytes between chunks.

Effects:

- `validPackets` roughly doubled (42 → 74 over ~30 s windows).
- One captured packet (`line=30`, bytes [160..163] = `05 00 80 19`)
  shows real pixel data extending past byte 4, confirming chunked
  polling does deliver full packets sometimes.
- Single-DMA tests after several reflashes started showing all-zero
  headers (Lepton may have entered a degraded state from repeated
  CCI command sequences + power cycles during debug). Chunked
  polling pulled occasional real packets even in that state.

Cost: per-packet read time is ~246 μs (3 × ~82 μs per chunk, polling
overhead included). Lepton's master-clocked rate is ~9000 pps, so
~4000 effective pps from us means we miss roughly 55 % of packets
during a Lepton broadcast. Frames never fully assemble — every
`line_mismatch` aborts the in-flight frame.

## Likely fixes (next session)

1. **Arduino-as-component for the SPI HAL only.** Add
   `espressif/arduino-esp32` to `firmware/main/idf_component.yml`,
   have `lepton_vospi.cpp` use Arduino's `SPIClass(FSPI).transferBytes`
   (which Fox uses successfully on identical wiring). Arduino's path
   chunks at 64 bytes too, but its per-call overhead is much lower
   than `spi_device_polling_transmit`. Pragmatic, works.
2. **Direct register-level read** via `spi_ll_*`. Tried this in-session
   — added `spi_ll_set_miso_bitlen` + `spi_ll_user_start` + busy-wait
   on `spi_ll_get_running_cmd`. Reader hung on the first chunk; spi_ll
   alone doesn't fully replace what `spi_master` does in
   `spi_format_hw_data` (cmd/addr/dummy bitlen, polarity, bit order,
   `apply_config`, etc.), and the `spi_master`-initialized hardware
   state isn't in the configuration this raw path expects. Either
   bypass spi_master entirely (do hardware init via `spi_ll_master_*`
   / direct registers) or call into IDF's lower-level `spi_hal_*`
   helpers. ~3-4 hours of careful work.
3. **Re-flash Fox** to confirm wiring + Lepton are still healthy. If
   Fox doesn't pull frames either, the Lepton may be stuck — hard
   power-cycle (unplug USB ≥ 15 s) before further VoSPI debug.

## Code state

`firmware/components/hal_lepton/src/lepton_vospi.c` — chunked polling
(64+64+36) with manual CS, full-duplex, DMA disabled (chunks fit in
the 64-byte FIFO so DMA isn't needed). Builds and runs; `frames`
stays at 0; relay shows live diagnostic counters via `Tick.thermal`.
