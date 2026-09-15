idf_component_register(
    SRCS "main.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "demo_golf.c"
         "golf_model.c"
         "golf_input.c"
         "golf_idle.c"
         "rtttl_player.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_hw_support esp_timer
)
