idf_component_register(
    SRCS "bsp_display.c" "bsp_touch.c" "bsp_i2c.c" "bsp_spi.c"
    INCLUDE_DIRS "."
    REQUIRES "esp_lcd" "driver" "esp_lcd_jd9853" "esp_lcd_touch_axs5106")
