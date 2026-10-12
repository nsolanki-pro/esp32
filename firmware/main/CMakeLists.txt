set(srcs "main.c" "rxstats.c" "ota.c" "settings.c" "provision.c" "update.c")
set(embed "update.html" "setup.html" "settings.html")
if(CONFIG_APP_CSI_MOTION)
    list(APPEND srcs "motion.c")
    list(APPEND embed "motion.html")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES ${embed}
                       PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_http_server esp_driver_gpio
                                     esp_timer esp_pm lwip app_update esp_app_format esp_http_client mbedtls)
