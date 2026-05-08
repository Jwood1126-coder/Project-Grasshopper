idf_component_register(
    SRCS "src/hal_oled.c" "src/oled_font.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src" "../../main/include"
    REQUIRES driver freertos
    PRIV_REQUIRES esp_timer hal_lepton
)
