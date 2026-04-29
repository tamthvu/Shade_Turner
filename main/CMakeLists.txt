idf_component_register(SRCS "Shade_Turner.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi nvs_flash esp_event esp_netif lwip
                    PRIV_REQUIRES esp_driver_mcpwm)
