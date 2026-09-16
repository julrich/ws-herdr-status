idf_component_register(
    SRCS "lvgl_kawaii_face.c"
    INCLUDE_DIRS "include"
    REQUIRES
        lvgl__lvgl
    PRIV_REQUIRES
        log
        esp_timer
        heap
)

# esp_lvgl_port is optional (compile-time, via __has_include).
# Link it only when it is actually present in the build.
# Link the *target* that is there: esp_lvgl_port names its CMake library
# lvgl_port_lib, so asking for it by component name produces a -l the linker cannot
# resolve. (Vendoring fix; upstream builds against a version whose names matched.)
if(TARGET __idf_espressif__esp_lvgl_port)
    target_link_libraries(${COMPONENT_LIB} PRIVATE __idf_espressif__esp_lvgl_port)
elseif(TARGET espressif__esp_lvgl_port)
    target_link_libraries(${COMPONENT_LIB} PRIVATE espressif__esp_lvgl_port)
endif()
