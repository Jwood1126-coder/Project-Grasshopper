// SPI HAL — Arduino-ESP32's SPIClass, wrapped as C callable.
// See lepton_spi.h for why we don't use the IDF spi_master driver here.

#include "lepton_spi.h"
#include "board_pins.h"

#include <Arduino.h>
#include <SPI.h>

#include "esp_log.h"

static const char *TAG = "lep_spi";

static SPIClass lepSPI(FSPI);
static bool s_inited = false;

extern "C" esp_err_t lepton_spi_init(void) {
    if (s_inited) return ESP_OK;

    lepSPI.begin(LEP_SPI_SCK, LEP_SPI_MISO, -1, -1);
    pinMode(LEP_SPI_CS, OUTPUT);
    digitalWrite(LEP_SPI_CS, HIGH);
    lepSPI.beginTransaction(SPISettings(LEP_SPI_FREQ_HZ, MSBFIRST, SPI_MODE3));

    s_inited = true;
    ESP_LOGI(TAG, "Arduino SPIClass(FSPI) ready (sck=%d miso=%d cs=%d, %d Hz, mode 3)",
             LEP_SPI_SCK, LEP_SPI_MISO, LEP_SPI_CS, LEP_SPI_FREQ_HZ);
    return ESP_OK;
}

extern "C" bool lepton_spi_read_packet(uint8_t *buf, size_t len) {
    if (!s_inited) return false;
    digitalWrite(LEP_SPI_CS, LOW);
    lepSPI.transferBytes(nullptr, buf, len);
    digitalWrite(LEP_SPI_CS, HIGH);
    return true;
}
