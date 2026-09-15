// main/main.c —— 迷你高尔夫固件的应用入口。
//
// 本固件是单一玩法固件:上电后直接进入球场,没有 BSP 参考示例的菜单,
// 因此也没有"长按确定返回菜单"这条路。三个键全部交给玩法页:
//   上/下 短按 = 瞄准微调 4 度;上/下 长按 = 粗调 32 度
//   确定 短按 = 瞄准阶段进入力度 / 力度阶段击球 / 进洞后下一洞 / 结算后重开
//   确定 长按 = 玩法页不使用(没有可返回的上级页面)
//
// 屏幕由 ui_pixel 主题搭建(天空、草地、标题板、墨线面板),视觉身份与
// 仓库其余页面一致。
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"      // 显示初始化失败时要打印 BSP_LCD_* 引脚号
#include "demo.h"
#include "esp_log.h"
#include "esp_sleep.h"

static const char *TAG = "main";

// 按键回调运行在 button 组件的任务里。玩法页只把事件塞进无阻塞队列,
// 由 LVGL 定时器在下一帧消费,所以这里既不拿 LVGL 锁,也不会因为
// 等锁而丢掉"按住不放"的按下事件。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    demo_golf_key(btn, ev);
}

void app_main(void) {
    ESP_LOGI(TAG, "Mini Golf 启动");

    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "休眠唤醒原因: %d", wakeup);
    }

    bsp_i2c_init();
    bsp_i2c_scan();

    // 屏幕是玩法的唯一 UI 载体,失败就没有可玩的东西 —— 打清楚日志后退出,
    // 不做"串口版玩法"降级。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,玩法无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    // 按键是唯一输入,失败也要把球场画出来:屏幕能亮、日志有报错,
    // 现场才好判断是按键坏了还是显示坏了。
    // 音频与电量是可选增强,失败不阻塞 —— 电量读不到时页面画占位符,
    // 音效不可用时静默出杆。
    const bool button_ok = (bsp_button_init(on_key, NULL) == ESP_OK);
    const bool audio_ok = (bsp_audio_init() == ESP_OK);
    const bool battery_ok = (bsp_battery_init() == ESP_OK);
    if (!button_ok) {
        ESP_LOGE(TAG, "按键初始化失败:球场会正常显示,但无法操作。");
    }

    if (bsp_lvgl_lock(1000)) {
        demo_golf_enter();
        bsp_lvgl_unlock();
    }

    ESP_LOGI(TAG, "就绪:Button=%d Audio=%d Battery=%d",
             button_ok, audio_ok, battery_ok);
}
