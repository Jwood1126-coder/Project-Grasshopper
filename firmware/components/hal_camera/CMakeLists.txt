idf_component_register(
    SRCS "src/hal_camera.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "../../main/include"
    REQUIRES espressif__esp32-camera
    PRIV_REQUIRES esp_timer nvs_flash
)
