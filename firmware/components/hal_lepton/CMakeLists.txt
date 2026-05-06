idf_component_register(
    SRCS "src/lepton_cci.c" "src/lepton_vospi.c" "src/hal_lepton.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src" "../../main/include"
    REQUIRES driver esp_timer freertos
    PRIV_REQUIRES nvs_flash esp_driver_i2c esp_driver_spi esp_driver_gpio
)
