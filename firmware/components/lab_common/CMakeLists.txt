idf_component_register(SRCS "lab_led.c" "lab_console.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_uart
                       PRIV_REQUIRES vfs)
