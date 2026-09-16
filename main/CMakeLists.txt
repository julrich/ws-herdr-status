idf_component_register(
    SRCS "main.c" "wifi_sta.c" "herdr_client.c" "ui_companion.c" "mood_face.c"
         "ui_device.c"
    INCLUDE_DIRS "."
    REQUIRES "esp_bsp" "esp_lcd" "driver" "nvs_flash"
             "esp_wifi" "esp_netif" "esp_event" "esp_http_client" "json"
             "espressif__esp_lvgl_port" "lvgl__lvgl" "lvgl_kawaii_face"
    PRIV_REQUIRES "esp_timer")   # wifi_sta.c and herdr_client.c both include it

# ui_companion.c stays free of esp_* headers so the host harness (tools/ui_host_test)
# can compile it unmodified (AGENTS.md §9a); this is what turns UI_LOGI into real
# ESP_LOGI. mood_face.c is the one file that reaches into the kawaii component and
# its esp_err.h, and that harness compiles it too, with shims.
target_compile_definitions(${COMPONENT_LIB} PRIVATE UI_USE_ESP_LOG=1)