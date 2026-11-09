set(srcs)
if(CONFIG_LAB_FP_EMU)
    list(APPEND srcs "fp_emu.c")
endif()
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES lab_common esp_driver_uart esp_driver_gpio nvs_flash esp_timer)
