idf_component_register(
    SRCS "main.c"
         # --- 「家机桌宠」应用本体 ---
         "pet_app.c"
         "pet_model.c"
         "pet_store.c"
         "pet_audio.c"
         "pet_view.c"
         "lv_font_pet_16.c"
         # --- BSP 参考演示 ---
         # 开机不再进入演示菜单（见 main.c），但这些文件仍参与编译：
         # 这样它们不会在无人察觉的情况下被改坏，链接时未被引用的目标文件也不会进固件。
         "demo_navigation.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "demo_display.c"
         "demo_button.c"
         "demo_audio.c"
         "demo_battery.c"
         "demo_radio.c"
         "demo_wifi.c"
         "demo_ble.c"
         "demo_low_power.c"
    INCLUDE_DIRS "."
    REQUIRES bsp bt esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash
)
