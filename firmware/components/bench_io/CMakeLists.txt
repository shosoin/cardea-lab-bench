set(srcs)
if(CONFIG_LAB_BENCH_IO)
    list(APPEND srcs "bench_io.c")
endif()
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES lab_common esp_driver_gpio)
