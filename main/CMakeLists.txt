idf_component_register(
    SRCS "main.c" "wifi_sta.c" "herdr_client.c" "ui_companion.c" "mood_face.c"
         "imu_qmi8658.c" "rotation_logic.c" "ui_rotation.c" "ui_device.c"
    INCLUDE_DIRS "."
    REQUIRES "esp_bsp" "esp_lcd" "driver" "nvs_flash"
             "esp_wifi" "esp_netif" "esp_event" "esp_http_client" "json"
             "espressif__esp_lvgl_port" "lvgl__lvgl" "lvgl_kawaii_face"
    PRIV_REQUIRES "esp_timer")   # imu_qmi8658.c; it arrived transitively under LVGL 8

# ui_companion.c and rotation_logic.c stay free of esp_* headers so the host
# harnesses (tools/ui_host_test, tools/rotation_test) can compile them
# unmodified (AGENTS.md §9a); this is what turns UI_LOGI into real ESP_LOGI.
target_compile_definitions(${COMPONENT_LIB} PRIVATE UI_USE_ESP_LOG=1)