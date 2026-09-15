// main/pet_app.c —— 桌宠应用的编排层：按键、计时、界面切换与状态保存。
//
// 线程模型（与仓库约定一致）：
//   * 按键回调运行在共享 esp_timer 任务里，只把事件丢进队列，立即返回；
//   * 每秒一次的 tick 也通过同一个队列投递；
//   * 所有模型读写与 LVGL 调用都发生在唯一的 app 任务里，因此无需为状态加锁。
//   LVGL 不是线程安全的，app 任务访问界面时仍会持有 bsp_lvgl_lock()。
#include "pet_app.h"

#include <string.h>

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "pet_audio.h"
#include "pet_model.h"
#include "pet_store.h"
#include "pet_view.h"

static const char *TAG = "pet_app";

#define PET_QUEUE_DEPTH 16
#define PET_TICK_PERIOD_US (1000 * 1000)
#define PET_SAVE_EVERY_TICKS 30
#define PET_BATTERY_EVERY_TICKS 10

typedef enum {
    PET_MSG_KEY = 0,
    PET_MSG_TICK,
} pet_msg_kind_t;

typedef struct {
    pet_msg_kind_t kind;
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} pet_msg_t;

typedef enum {
    PET_PAGE_MAIN = 0,
    PET_PAGE_STATUS,
    PET_PAGE_CLOCK,
} pet_page_t;

static pet_model_t s_model;
static pet_view_t *s_view;
static pet_page_t s_page;
static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_stopped;
static esp_timer_handle_t s_tick_timer;
static volatile bool s_running;
static uint32_t s_ticks;
static pet_view_battery_t s_battery;
static bool s_battery_ok;

/* ------------------------------------------------------------------ 电量 */

static void pet_read_battery(void)
{
    if (!s_battery_ok) {
        s_battery.available = false;
        s_battery.soc = -1;
        return;
    }
    const int soc = bsp_battery_soc();
    s_battery.soc = soc;
    s_battery.available = soc >= 0;
}

/* ------------------------------------------------------------------ 界面切换 */

static pet_view_t *pet_page_create(pet_page_t page)
{
    switch (page) {
    case PET_PAGE_STATUS:
        return pet_status_view_create(&s_model, &s_battery);
    case PET_PAGE_CLOCK: {
        pet_view_t *view = pet_clock_view_create(&s_model);
        if (view) pet_clock_view_sync(view, &s_model);
        return view;
    }
    case PET_PAGE_MAIN:
    default:
        return pet_view_create();
    }
}

static void pet_page_delete(pet_page_t page, pet_view_t *view)
{
    if (!view) return;
    switch (page) {
    case PET_PAGE_STATUS: pet_status_view_delete(view); break;
    case PET_PAGE_CLOCK: pet_clock_view_delete(view); break;
    case PET_PAGE_MAIN:
    default: pet_view_delete(view); break;
    }
}

static void pet_show_page(pet_page_t page)
{
    /* 两个约束互相拉扯：LVGL 的活动屏幕指针必须始终指向一块有效内存，而这台
       机器没有 PSRAM，24 KB 的 LVGL 堆只装得下一页。
       「先建新页、再拆旧页」满足了前者，代价是峰值变成「两页同时存在」；
       「先拆后建」省内存，中间却有一段时间活动屏幕已经被释放。
       折中办法是在中间垫一张只有一个对象的空屏：
       切到空屏 → 拆旧页 → 建新页 → 撤掉空屏。
       整段都在 LVGL 锁内完成，空屏不会被真正渲染，所以看不到闪屏。 */
    lv_obj_t *blank = lv_obj_create(NULL);
    if (blank) lv_screen_load(blank);

    pet_page_delete(s_page, s_view);
    s_view = NULL;

    pet_view_t *fresh = pet_page_create(page);
    if (!fresh && page != PET_PAGE_MAIN) {
        /* 新页建不出来时退回主界面：总比把用户留在空屏上强。 */
        ESP_LOGE(TAG, "页面创建失败，退回主界面");
        fresh = pet_page_create(PET_PAGE_MAIN);
        if (fresh) page = PET_PAGE_MAIN;
    }

    /* 新页在创建时已经接管活动屏幕，空屏可以撤了。新页没建出来时留着它，
       至少保证活动屏幕还指向一块有效内存。 */
    if (blank && fresh) lv_obj_delete(blank);

    if (!fresh) {
        ESP_LOGE(TAG, "主界面也创建失败：LVGL 堆已耗尽");
        return;
    }

    s_view = fresh;
    s_page = page;
    if (page == PET_PAGE_MAIN) pet_view_sync(s_view, &s_model, &s_battery);
}

static void pet_refresh(void)
{
    if (!s_view) return;
    switch (s_page) {
    case PET_PAGE_STATUS:
        pet_status_view_sync(s_view, &s_model, &s_battery);
        break;
    case PET_PAGE_CLOCK:
        pet_clock_view_sync(s_view, &s_model);
        break;
    case PET_PAGE_MAIN:
    default:
        pet_view_sync(s_view, &s_model, &s_battery);
        break;
    }
}

/* ------------------------------------------------------------------ 交互 */

static void pet_handle_action(pet_action_t action)
{
    uint32_t events = PET_EVENT_NONE;
    const pet_result_t result = pet_model_interact(&s_model, action, &events);
    if (result == PET_OK) {
        switch (action) {
        case PET_ACT_PET: pet_audio_play(PET_SOUND_PET); break;
        case PET_ACT_FEED: pet_audio_play(PET_SOUND_FEED); break;
        case PET_ACT_PLAY: pet_audio_play(PET_SOUND_PLAY); break;
        default: break;
        }
    } else {
        pet_audio_play(PET_SOUND_DENY);
    }

    if (s_view && s_page == PET_PAGE_MAIN) {
        pet_view_react(s_view, action, result);
        if (result != PET_OK) {
            pet_view_toast(s_view, pet_result_name(result));
        } else if (events & PET_EVENT_BOND_LEVEL_UP) {
            // 亲密度升级是陪伴类玩法的主要正反馈，单独提示一次。
            char notice[PET_STATUS_MAX];
            pet_model_bond_notice(&s_model, notice, sizeof(notice));
            pet_audio_play(PET_SOUND_CONFIRM);
            pet_view_toast(s_view, notice);
        }
        pet_view_sync(s_view, &s_model, &s_battery);
    }
    (void)pet_store_save_async(&s_model);
}

static void pet_handle_key(bsp_btn_t btn, bsp_btn_ev_t event)
{
    /* 界面还没建好时不该响应按键：此刻既没有可操作的对象，s_page 也还停在
       默认值上，处理了就会在空界面上翻页。 */
    if (!s_view) return;

    switch (s_page) {
    case PET_PAGE_MAIN:
        if (event == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            pet_audio_play(PET_SOUND_CONFIRM);
            pet_show_page(PET_PAGE_STATUS);
            return;
        }
        if (event != BSP_BTN_CLICK) return;
        if (btn == BSP_BTN_UP) pet_handle_action(PET_ACT_PET);
        else if (btn == BSP_BTN_DOWN) pet_handle_action(PET_ACT_FEED);
        else if (btn == BSP_BTN_OK) pet_handle_action(PET_ACT_PLAY);
        return;

    case PET_PAGE_STATUS:
        if (event == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            pet_audio_play(PET_SOUND_CONFIRM);
            pet_show_page(PET_PAGE_MAIN);
        } else if (event == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
            pet_audio_play(PET_SOUND_CONFIRM);
            pet_show_page(PET_PAGE_CLOCK);
        }
        return;

    case PET_PAGE_CLOCK:
        if (event == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            pet_audio_play(PET_SOUND_CONFIRM);
            (void)pet_store_save_async(&s_model);
            pet_show_page(PET_PAGE_MAIN);
        } else if (event == BSP_BTN_CLICK && btn == BSP_BTN_UP) {
            pet_model_adjust_clock(&s_model, 1);
            pet_audio_play(PET_SOUND_MOVE);
            pet_refresh();
        } else if (event == BSP_BTN_CLICK && btn == BSP_BTN_DOWN) {
            pet_model_adjust_clock(&s_model, -1);
            pet_audio_play(PET_SOUND_MOVE);
            pet_refresh();
        }
        return;

    default:
        return;
    }
}

/* ------------------------------------------------------------------ 计时 */

static void pet_handle_tick(void)
{
    const uint32_t events = pet_model_tick(&s_model, 1);
    s_ticks++;

    if (events & PET_EVENT_FELL_ASLEEP) pet_audio_play(PET_SOUND_SLEEP);
    if (events & PET_EVENT_WOKE) pet_audio_play(PET_SOUND_WAKE);
    if (events & PET_EVENT_BECAME_HUNGRY) pet_audio_play(PET_SOUND_HUNGRY);
    if (events & PET_EVENT_BOND_LEVEL_UP) pet_audio_play(PET_SOUND_CONFIRM);

    if (s_ticks % PET_BATTERY_EVERY_TICKS == 0) pet_read_battery();
    if (s_ticks % PET_SAVE_EVERY_TICKS == 0) (void)pet_store_save_async(&s_model);

    if (!bsp_lvgl_lock(200)) return;
    pet_refresh();
    bsp_lvgl_unlock();
}

/* ------------------------------------------------------------------ 任务 */

/* 退出时先给出信号量再自删：这样 pet_app_stop 能等到任务真正离开，
   不会在「任务正持有 LVGL 锁」的瞬间把它砍掉，把锁永久留在占用状态。 */
static void pet_task_exit(void)
{
    if (s_stopped) xSemaphoreGive(s_stopped);
    vTaskDelete(NULL);
}

static void pet_app_task(void *context)
{
    (void)context;
    pet_msg_t message;
    while (xQueueReceive(s_queue, &message, portMAX_DELAY) == pdTRUE) {
        if (!s_running) break;
        if (message.kind == PET_MSG_TICK) {
            pet_handle_tick();
            continue;
        }
        if (!bsp_lvgl_lock(300)) continue;
        pet_handle_key(message.btn, message.event);
        bsp_lvgl_unlock();
    }
    pet_task_exit();
}

static void pet_on_key(bsp_btn_t btn, bsp_btn_ev_t event, void *user)
{
    (void)user;
    if (!s_queue || !s_running) return;
    const pet_msg_t message = { .kind = PET_MSG_KEY, .btn = btn, .event = event };
    (void)xQueueSend(s_queue, &message, 0);
}

static void pet_on_tick(void *arg)
{
    (void)arg;
    if (!s_queue || !s_running) return;
    const pet_msg_t message = { .kind = PET_MSG_TICK };
    (void)xQueueSend(s_queue, &message, 0);
}

/* ------------------------------------------------------------------ 生命周期 */

esp_err_t pet_app_start(void)
{
    if (s_running) return ESP_OK;

    if (!pet_store_init()) {
        ESP_LOGW(TAG, "无法使用 NVS，本次运行不会保存状态");
    }
    if (!pet_store_load(&s_model)) {
        ESP_LOGI(TAG, "没有历史状态，领养一只新的桌宠");
        pet_model_init(&s_model, (uint32_t)esp_timer_get_time());
    }

    /* 音效与电量都是可选能力：失败只影响体验，不阻塞应用启动。 */
    if (!pet_audio_init()) {
        ESP_LOGW(TAG, "音频不可用，继续静音运行");
    } else {
        pet_audio_set_volume(s_model.volume);
    }
    s_battery_ok = bsp_battery_init() == ESP_OK;
    pet_read_battery();

    s_queue = xQueueCreate(PET_QUEUE_DEPTH, sizeof(pet_msg_t));
    if (!s_queue) return ESP_ERR_NO_MEM;
    s_stopped = xSemaphoreCreateBinary();
    if (!s_stopped) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_running = true;
    if (xTaskCreate(pet_app_task, "pet_app", 5120, NULL, 5, &s_task) != pdPASS) {
        s_running = false;
        vSemaphoreDelete(s_stopped);
        s_stopped = NULL;
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = pet_on_tick,
        .name = "pet_tick",
    };
    if (esp_timer_create(&timer_args, &s_tick_timer) == ESP_OK) {
        esp_timer_start_periodic(s_tick_timer, PET_TICK_PERIOD_US);
    } else {
        ESP_LOGW(TAG, "计时器创建失败，状态将不会随时间变化");
    }

    /* 先把界面建出来，再开始收按键。反过来做的话，从 bsp_button_init 到主界面
       就绪之间存在一段窗口：按键事件会被当成有效操作处理，而那时 s_view 还是
       NULL、s_page 还是默认的主界面。开机瞬间的一次电气抖动就足以让界面「自己」
       翻到状态页、时钟页，并顺手把时钟标成已校准。 */
    if (bsp_lvgl_lock(1000)) {
        pet_show_page(PET_PAGE_MAIN);
        if (s_view && !s_model.clock_set) {
            pet_view_toast(s_view, "长按确定可校准时钟");
        }
        bsp_lvgl_unlock();
    }

    if (bsp_button_init(pet_on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败，应用无法交互");
    }

    pet_audio_play(PET_SOUND_BOOT);
    ESP_LOGI(TAG, "桌宠已就绪：电量=%d 时钟=%s", s_battery.soc,
             s_model.clock_set ? "已校准" : "未校准");
    return ESP_OK;
}

void pet_app_stop(void)
{
    if (!s_running) return;
    s_running = false;

    if (s_tick_timer) {
        esp_timer_stop(s_tick_timer);
        esp_timer_delete(s_tick_timer);
        s_tick_timer = NULL;
    }

    /* 唤醒任务让它自己退出：先发一条消息解除 portMAX_DELAY 阻塞，
       再等它给出信号量。这样绝不会有「任务正持锁时被强杀」的窗口。 */
    if (s_queue) {
        const pet_msg_t wake = { .kind = PET_MSG_TICK };
        (void)xQueueSend(s_queue, &wake, 0);
    }
    if (s_stopped) (void)xSemaphoreTake(s_stopped, pdMS_TO_TICKS(1000));
    s_task = NULL;

    if (s_queue) {
        vQueueDelete(s_queue);
        s_queue = NULL;
    }
    if (s_stopped) {
        vSemaphoreDelete(s_stopped);
        s_stopped = NULL;
    }

    (void)pet_store_save_async(&s_model);

    if (bsp_lvgl_lock(500)) {
        pet_page_delete(s_page, s_view);
        s_view = NULL;
        bsp_lvgl_unlock();
    }
}
