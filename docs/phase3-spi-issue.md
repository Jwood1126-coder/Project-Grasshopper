# Phase 3 — VoSPI receive truncated to 4 bytes

## Symptom

Reader reads packets at 100 pps (SYNC mode bound by `vTaskDelay(1)` at
the 100 Hz tick rate, as designed). Each packet is supposed to be 164
bytes; the SPI clock runs for the full duration (measured 132 μs total,
~80 μs of clocking which matches 1312 bits at 16 MHz), but only the
first 4 bytes read back as real Lepton data. Bytes 4..163 come back as
`0x00`.

Result: `validPackets / totalPackets` ≈ 0.8 % (we mostly catch real
discard packets), and every "valid" packet decodes as `line == 0`
because byte 1 of the rx buffer is always zero, so `packet_number =
((byte0 & 0x0F) << 8) | byte1 = 0`. State machine advances
SYNC → READING, then aborts on the very next packet because expected
line 1 is also read as line 0 → `line_mismatch`. Frames never commit.

## What's been ruled out

Same 4-byte read regardless of:

- `spi_device_transmit` (DMA queued) vs `spi_device_polling_transmit`
- `SPI_DMA_CH_AUTO` vs `SPI_DMA_DISABLED`
  (with `SPI_DMA_DISABLED`, polling errors out at ">host max" as
  expected — confirms DMA is engaged in the working path)
- Full-duplex (`length = 1312`, dummy zero TX buffer) vs half-duplex
  (`length = 0`, `rxlength = 1312`, `SPI_DEVICE_HALFDUPLEX |
  SPI_DEVICE_NO_DUMMY`)
- `SPI2_HOST` (FSPI) vs `SPI3_HOST` (HSPI)
- Manual CS (`spics_io_num = -1` + `gpio_set_level`) vs driver-managed
  CS (`spics_io_num = LEP_SPI_CS`). Driver-managed CS gave all-zero rx
  reliably — Lepton never sees CS asserted. So manual CS works in
  principle.
- MISO `gpio_set_pull_mode(GPIO_PULLUP_ONLY)` — tail bytes still 0x00,
  which means the line is actively driven low, not floating.
- MOSFET power-cycle (1 s OFF, then ON, 2 s settle) before init.
- CCI fully configured vs CCI skipped — same data either way, so the
  truncation is independent of Lepton state.
- `0xAA` prefill on the rx buffer survives bytes 4..163 are
  overwritten — proves the SPI driver IS writing to those bytes,
  writing zeros.
- Buffers are DMA-capable internal SRAM (`MALLOC_CAP_DMA |
  MALLOC_CAP_INTERNAL`).

## Hypotheses

1. **IDF v5.3 spi_master quirk on non-IOMUX pins** routed via GPIO
   matrix. SCK=41 and MISO=42 are not the FSPI native IOMUX pins
   (those are 12, 13, 11, 10). Routing through GPIO matrix is allowed
   but has different timing. Fox uses identical pins via
   Arduino-ESP32's `SPIClass(FSPI)` and works — but Arduino-ESP32 may
   call lower-level APIs differently than `spi_master`.

2. **DMA descriptor truncation**: the descriptor may be limited to 4
   bytes by some misinterpretation of `length` / `rxlength` in
   half-duplex mode. The fact that timing is correct (full clock
   duration) but data is missing past byte 4 hints at an internal
   state-machine that stops latching MISO into the descriptor after
   the first word.

3. **Some driver path expects an explicit cmd / addr phase** (4 bytes)
   that's eating the first 32 bits in our buffer, and then either
   never starts the data phase or starts it but doesn't write to our
   rx_buffer. We've explicitly set `command_bits = 0`,
   `address_bits = 0`, `dummy_bits = 0`, but maybe an internal
   default in v5.3 overrides.

## Path forward (next session)

In rough preference order:

1. **Arduino-as-component for hal_lepton's SPI only.** Add
   `espressif/arduino-esp32` to the firmware's
   `idf_component.yml`, then have `lepton_vospi.c` call into Arduino's
   `SPIClass` to do the transfer. Keep CCI on the IDF
   `i2c_master_*` API. Known to work on this exact wiring (Fox is the
   reference).
2. **Direct `spi_ll_*` register-level read.** Bypass the driver,
   manually program SPI registers, poll FIFO. Mirrors what
   Arduino-ESP32's `spiTransferBytesNL` does on ESP32-original.
3. **Move SCK/MISO to FSPI IOMUX pins** (re-wire the breakout to
   GPIO 12 / 13 / 11). Costs hardware change; only worth it if the
   driver path gets unblocked by IOMUX.

## Code state

`firmware/components/hal_lepton/src/lepton_vospi.c` left in a working
build state but not producing frames. The component compiles, boots,
and reports stats to the relay — `frames` stays at 0, `lineMismatch`
counter increments. Boot completes the rest of the sketch normally
(Wi-Fi, relay, periodic Tick).
