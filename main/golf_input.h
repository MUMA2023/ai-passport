// main/golf_input.h —— 迷你高尔夫的按键翻译层。
//
// 硬件按键的事件模型有个陷阱:按下瞬间先上报 PRESS_DOWN,长按阈值到达后才上报
// LONG_PRESS_START,抬起后还可能补一个 SINGLE_CLICK。而"按下即响应"与"区分短按/
// 长按"对同一个键是互斥的,所以翻译规则必须显式写清楚,并且能被主机测试覆盖:
//
//   上/下  PRESS = 微调 GOLF_AIM_STEP_DEG
//           LONG  = 再补 (GOLF_AIM_COARSE_DEG - GOLF_AIM_STEP_DEG),
//                   这样一次长按合计正好是粗调步长,而不是 微调 + 粗调
//   确定   POWER 阶段用 PRESS(摆动条有效窗口只有几十毫秒)
//           其余阶段用 CLICK,这样"长按确定返回菜单"不会先闪出一格力度
//
// 本文件不依赖 ESP-IDF 与 LVGL,只有整数状态,可直接在主机上测试。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "golf_model.h"

// 同一次按压里,PRESS 与随后 CLICK 的最大间隔。超过就视为两次独立操作。
#define GOLF_PRESS_CLICK_WINDOW_MS 400

// 瞄准步长:短按微调、长按粗调。
#define GOLF_AIM_STEP_DEG 4
#define GOLF_AIM_COARSE_DEG 32

typedef enum {
    GOLF_KEY_UP = 0,
    GOLF_KEY_DOWN,
    GOLF_KEY_OK,
    GOLF_KEY_COUNT,
} golf_key_t;

typedef enum {
    GOLF_KEY_EV_PRESS = 0,
    GOLF_KEY_EV_CLICK,
    GOLF_KEY_EV_LONG,
} golf_key_ev_t;

typedef struct {
    uint64_t last_press_ms[GOLF_KEY_COUNT];
    bool press_seen[GOLF_KEY_COUNT];
} golf_key_state_t;

// 把一次按键事件翻译成模型操作,返回模型事件(调用方据此播音效、刷新界面)。
golf_event_t golf_input_key(golf_model_t *model, golf_key_state_t *state,
                            golf_key_t key, golf_key_ev_t ev, uint64_t now_ms);
