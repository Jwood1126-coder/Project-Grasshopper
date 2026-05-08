idf_component_register(
    SRCS "src/hal_storage.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "../../main/include"
    REQUIRES vfs sdmmc
    PRIV_REQUIRES esp_driver_sdmmc esp_driver_gpio fatfs joltwallet__littlefs
)
