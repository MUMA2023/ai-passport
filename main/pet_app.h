// main/pet_app.h —— 「家机桌宠」应用入口。
//
// 应用接管整块屏幕（不再经过演示菜单）：开机即进入桌宠主界面。
#pragma once

#include "bsp_button.h"
#include "esp_err.h"

// 启动应用：载入或新建宠物状态、启动界面与音效、开始计时。
// 必须在 bsp_display_init() 与 bsp_lvgl_init() 成功之后调用。
esp_err_t pet_app_start(void);

// 停止应用：停止定时器与任务、保存状态、释放界面。
void pet_app_stop(void);
