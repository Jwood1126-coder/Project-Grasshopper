#pragma once

// Grasshopper hardware pin map — same as Scout Fox.
// Board: GOOUUU ESP32-S3 (Freenove WROOM N8R8 clone).

// ===== Visible camera (OV2640/OV5640 parallel) — phase 4 =====
#define CAM_PIN_PWDN    -1
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    15
#define CAM_PIN_SIOD     4
#define CAM_PIN_SIOC     5
#define CAM_PIN_D7      16
#define CAM_PIN_D6      17
#define CAM_PIN_D5      18
#define CAM_PIN_D4      12
#define CAM_PIN_D3      10
#define CAM_PIN_D2       8
#define CAM_PIN_D1       9
#define CAM_PIN_D0      11
#define CAM_PIN_VSYNC    6
#define CAM_PIN_HREF     7
#define CAM_PIN_PCLK    13

// ===== FLIR Lepton 3.1R VoSPI (FSPI / SPI2) =====
#define LEP_SPI_SCK     41
#define LEP_SPI_MISO    42
#define LEP_SPI_CS      14
// 8 MHz instead of 16 MHz — Codex's signal-integrity separator test.
// If thermal frames immediately stabilize at 8 MHz, the breakout has
// a margin issue at 16 MHz (loose wires, long jumpers, no termination).
// If 8 MHz fails the same way, the bug is in the VoSPI state machine.
#define LEP_SPI_FREQ_HZ 8000000

// ===== FLIR Lepton 3.1R CCI (I2C) =====
#define LEP_I2C_SDA      1
#define LEP_I2C_SCL     21
#define LEP_I2C_ADDR  0x2A

// ===== Lepton control =====
#define LEP_EN_PIN      -1   // EN not wired (breakout pull-up keeps enabled)
#define LEP_VSYNC_PIN   -1   // VSYNC not wired
#define LEP_POWER_PIN   46   // N-channel MOSFET gate, 10K pulldown

// ===== SD card (1-bit MMC, onboard slot) — phase 4 =====
#define SD_MMC_CLK      39
#define SD_MMC_CMD      38
#define SD_MMC_D0       40

// ===== SSD1306 OLED (shares Lepton I2C bus) =====
#define OLED_I2C_ADDR  0x3C
#define OLED_W         128
#define OLED_H          64

// ===== Lepton frame geometry =====
#define LEP_W            160
#define LEP_H            120
#define LEP_PIXELS       (LEP_W * LEP_H)         // 19200
#define LEP_FRAME_BYTES  (LEP_PIXELS * 2)        // 38400
#define LEP_PKT_LEN      164                     // 4 header + 160 data bytes

// Gain modes are public CCI API — see hal_lepton.h.
