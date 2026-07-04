idf_component_register(
    SRCS "main.c" "relay.c" "net.c" "link.c" "web.c" "captive.c" "update.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_server esp_timer json driver app_update esp_https_ota esp_http_client esp-tls)
