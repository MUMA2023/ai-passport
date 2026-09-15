// main/main.c —— 「家机桌宠」固件入口：初始化板级外设后直接进入桌宠。
//
// 本固件不再提供演示菜单，开机即是一只住在屏幕里的小宠物。
// 按键语义（桌宠主界面）：
//   上   短按   撸一撸
//   下   短按   喂食
//   确定 短按   玩耍
//   确定 长按   进入状态页（状态页里再长按返回）
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"      // 错误日志里要打印 BSP_LCD_* 引脚号
#include "esp_log.h"
#include "esp_sleep.h"

#include "pet_app.h"

static const char *TAG = "main";

void app_main(void) {
    ESP_LOGI(TAG, "FoloToy AI Passport —— 家机桌宠 启动");
    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "休眠唤醒原因: %d", wakeup);
    }

    bsp_i2c_init();
    bsp_i2c_scan();

    // 屏幕是桌宠唯一的输出载体,失败就没有可用的应用形态 —— 打清楚日志后退出,
    // 不做"串口降级"(那会让本文件复杂一倍,也违背桌宠这个形态本身)。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,桌宠无法启动。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    esp_err_t err = pet_app_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "桌宠启动失败: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "就绪:长按确定进入状态页");
}
