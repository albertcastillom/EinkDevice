idf_component_register(
    SRCS "network_service.c"
    INCLUDE_DIRS "."
    REQUIRES esp_event esp_http_server esp_netif esp_wifi nvs_flash photo_store ui
)
