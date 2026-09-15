// main/pet_view.h —— 桌宠界面：场景、宠物本体、状态气泡与信息条。
//
// 所有函数都必须在持有 bsp_lvgl_lock() 的前提下调用；LVGL 不是线程安全的。
// 界面只负责「把状态画出来」和「播放一次反应动画」，不修改模型。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#include "pet_model.h"

// 「家机桌宠」自有配色：暖色小屋，与仓库的像素演示界面刻意区分开。
#define PET_C_WALL_TOP 0xBFE3F5
#define PET_C_WALL_LOW 0xE8F4FA
#define PET_C_DESK 0xF3D6A8
#define PET_C_DESK_EDGE 0xD9B183
#define PET_C_INK 0x2B2B33
#define PET_C_MUTED 0x6B7A90
#define PET_C_PANEL 0xFFFDF7
#define PET_C_BODY 0xFFF3E0
#define PET_C_BODY_SHADE 0xEAD9C0
#define PET_C_ACCENT 0xFFB86B
#define PET_C_CHEEK 0xFFA8A8
#define PET_C_HEART 0xE8536B
#define PET_C_GOOD 0x4CAF7D
#define PET_C_WARN 0xE0A33E
#define PET_C_BAD 0xD4553F

typedef struct pet_view pet_view_t;

// 电量读数（-1 表示不可用），由调用方从 bsp_battery_soc() 取得。
typedef struct {
    int soc;
    bool available;
} pet_view_battery_t;

// 创建并载入主界面。失败返回 NULL。
pet_view_t *pet_view_create(void);

// 按当前模型状态刷新文字、表情、亲密度与电量。播放中的动画不被打断。
void pet_view_sync(pet_view_t *view, const pet_model_t *model,
                   const pet_view_battery_t *battery);

// 播放一次互动反馈；result 为 PET_OK 时表现「开心」，否则表现「摇头」。
void pet_view_react(pet_view_t *view, pet_action_t action, pet_result_t result);

// 在屏幕上浮出一条短提示（例如「吃不下啦」）。
void pet_view_toast(pet_view_t *view, const char *text);

// 删除界面并停止其动画、定时器。
void pet_view_delete(pet_view_t *view);

// ---------------------------------------------------------------------------
// 状态页：亲密度明细与陪伴时长。
// ---------------------------------------------------------------------------
pet_view_t *pet_status_view_create(const pet_model_t *model,
                                   const pet_view_battery_t *battery);
// 刷新状态页上的时长与计数（它们会随时间变化）。
void pet_status_view_sync(pet_view_t *view, const pet_model_t *model,
                          const pet_view_battery_t *battery);
void pet_status_view_delete(pet_view_t *view);

// 时钟设置页：用上下键调分钟、确定键确认。
pet_view_t *pet_clock_view_create(const pet_model_t *model);
// 刷新时钟设置页显示的 HH:MM。
void pet_clock_view_sync(pet_view_t *view, const pet_model_t *model);
void pet_clock_view_delete(pet_view_t *view);

// 通用：屏幕顶部的时间与电量条，供各页面复用。
lv_obj_t *pet_hud_create(lv_obj_t *screen);
void pet_hud_sync(lv_obj_t *hud, const pet_model_t *model,
                  const pet_view_battery_t *battery);
