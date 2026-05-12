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

// ===== FLIR Lepton 3.5 VoSPI (FSPI / SPI2) =====
#define LEP_SPI_SCK     41
#define LEP_SPI_MISO    42
#define LEP_SPI_CS      14
// SPI clock — explicit diagnostic value, not a final choice.
//
//   Lepton 3.x segment period is ~9.4 ms. One segment is 60 packets ×
//   164 bytes × 8 bits = 78,720 bits, so 8 MHz takes ~9.84 ms before
//   software overhead — structurally too slow, host falls behind.
//   tCam-Mini (the reference design) runs 16 MHz on a controlled-
//   impedance PCB.
//
//   On this breadboarded rig:
//     - 8 MHz:  signal looks clean (bimodal byte0) but line_mismatch
//               accumulates (host can't keep up with segment rate)
//     - 12 MHz: current setting. Explicit diagnostic value while we
//               work on physical mitigations. NOT a final choice.
//     - 16 MHz: signal-integrity issues (byte0 lo-nibble spreads
//               across 1-14, no series resistor on SCK, no controlled
//               impedance, breadboard wiring)
//
//   Path forward: 16-20 MHz only after physical mitigations (22-33 Ω
//   series on SCK at the ESP32 end, twisted SCK+GND/MISO+GND, ≤10 cm
//   wires, decoupling near Lepton VIN, isolated 3V3 rail), OR switch
//   to IDF spi_master with DMA. See memory:lepton_breakout_damaged.md.
#define LEP_SPI_FREQ_HZ 12000000

// ===== FLIR Lepton 3.5 CCI (I2C) =====
#define LEP_I2C_SDA      1
#define LEP_I2C_SCL     21
#define LEP_I2C_ADDR  0x2A

// ===== Lepton control =====
#define LEP_EN_PIN      -1   // EN not wired (breakout pull-up keeps enabled)
#define LEP_VSYNC_PIN    2   // Lepton GPIO3 / PureThermal Pin 9 → ESP32 GPIO 2
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
