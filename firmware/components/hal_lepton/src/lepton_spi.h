#pragma once

// Thin C-callable shim around Arduino-ESP32's SPIClass for the Lepton
// VoSPI receive path. Implementation in lepton_spi.cpp uses
// SPIClass(FSPI).transferBytes(), which is known to work on the
// SCK=41/MISO=42/CS=14 wiring (Fox uses it). The IDF spi_master driver
// truncates the 164-byte read at byte 4 on this same wiring; Arduino's
// implementation bypasses spi_master and writes the FIFO registers
// directly in 64-byte chunks.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Init SPI bus + device, configure CS line. Idempotent.
esp_err_t lepton_spi_init(void);

// Read `len` bytes from MISO into `buf` while holding CS low. Used at
// ~9 kpps in the VoSPI reader, so kept hot-path lean (no logging).
bool lepton_spi_read_packet(uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
