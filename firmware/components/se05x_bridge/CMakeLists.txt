set(srcs)
if(CONFIG_LAB_SE05X_BRIDGE)
    list(APPEND srcs "se05x_bridge.c")
endif()
# config.local.h (Wi-Fi, simulator address) lives in the project directory, outside git.
idf_build_get_property(project_dir PROJECT_DIR)
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "${project_dir}"
                       PRIV_REQUIRES lab_common esp_wifi esp_netif esp_event lwip esp_driver_i2c esp_driver_gpio)
