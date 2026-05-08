# Phase 3 — SOLVED via Arduino-as-component

## Resolution

The IDF v5.3 `spi_master` driver truncated VoSPI receive at 4 bytes per
164-byte transaction on these GPIO-matrix pins (SCK=41, MISO=42, CS=14).
Replaced the SPI HAL with Arduino-ESP32's `SPIClass(FSPI).transferBytes`,
wrapped in a small C-callable `lepton_spi.{h,cpp}`. Arduino's
implementation bypasses `spi_master` and hits the SPI peripheral
registers directly in 64-byte FIFO chunks.

## Live results

After Lepton finished converging (~7 minutes after several power-cycles
from debug, longer than usual):

```
I (413910) lep_vospi: first frame! seg ids: 1 2 3 4
I (439421) frames=224 total=900000 valid=668306 discard=231694
I (466871) frames=464 total=1080000 valid=796812 discard=283188
I (494346) frames=704 total=1260000 valid=925408 discard=334592
```

That's **8.7 fps**, exactly Lepton 3.1R's native rate. Diagnostics:

- `valid:total` = 76 % (was 0.8 % with broken spi_master)
- `lineMismatch` = 21 (rare bit-level glitch — well under 0.1 %)
- `spliceDetected` = 704, climbing ~1/s — splice detector is alive and
  catching cross-broadcast splices in real time, frames committed only
  when content-clean.
- `seg_not1` and `seg_zero` plateaued at 6609 / 17630 — accumulated
  during Lepton convergence, no longer increasing.

## How to keep this working in CI

Two non-obvious things must be in place:

1. **Pin `arduino-esp32` to 3.1.1.** 3.1.0 is yanked. 3.1.3+ uses
   `uart_config_t.flags` (added in IDF v5.3.1; we're on 5.3.0). 3.2+
   needs IDF 5.4 components.
2. **Patch `managed_components/.../esp32-hal-uart.c:514`** —
   comment out `uart_config.flags.backup_before_sleep = false;`. Brittle
   (lost on a clean clone), but unblocks the build immediately. Real
   fix: bump IDF to v5.3.1+ and unpin Arduino. Tracking this as a
   follow-up.

## Files

- `firmware/main/idf_component.yml` — `espressif/arduino-esp32: "3.1.1"`
- `firmware/sdkconfig.defaults` — `CONFIG_AUTOSTART_ARDUINO=n`,
  `CONFIG_FREERTOS_HZ=1000`
- `firmware/components/hal_lepton/src/lepton_spi.{h,cpp}` — wrapper
- `firmware/components/hal_lepton/src/lepton_vospi.c` — calls
  `lepton_spi_read_packet`; SPI-master init removed.

## What's next (still phase 3)

- Long-soak (10 min) with the relay watching, confirming `frames` keeps
  climbing at ~9 fps and `spliceDetected` stays roughly linear in time.
- Hook `hal_lepton_run_ffc` to a periodic timer or Tick command so the
  scene stays calibrated.
- Then phase 4: `hal_camera`, `hal_storage`, `hal_oled`.
