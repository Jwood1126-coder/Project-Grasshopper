idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "." "include"
    REQUIRES nvs_flash esp_event esp_timer net_wifi net_relay proto hal_lepton hal_camera hal_storage hal_oled
)

# Compile-time version string. Override at build time:
#   idf.py build -DGRASSHOPPER_FW_VERSION="\"0.2.0\""
if(NOT DEFINED GRASSHOPPER_FW_VERSION)
    set(GRASSHOPPER_FW_VERSION "\"0.1.0-dev\"")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    GRASSHOPPER_FW_VERSION=${GRASSHOPPER_FW_VERSION}
)
