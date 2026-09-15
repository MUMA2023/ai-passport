// main/golf_idle.h —— 闲置调光的纯逻辑。
//
// 这是电池供电的小设备,页面长时间没人操作时应当把背光压低,避免"亮着屏干等"
// (见 docs/development/engineering/coding-conventions.md 的功耗约定)。
//
// 判定本身只是时间比较,但"只在状态翻转的那一帧上报"这一点很容易写错,而且两个
// 方向都是真 bug:每帧都上报会让背光被反复重设,从不恢复则会让屏幕再也亮不起来。
// 所以抽成不依赖 ESP-IDF/LVGL 的状态机,由主机测试钉住。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 无操作多久开始调暗。取 60 秒,与已发布的 Pokédex 玩法保持一致。
#define GOLF_IDLE_DIM_MS 60000ULL

typedef struct {
    uint64_t last_activity_ms;   // 最近一次按键的时间戳
    bool     dimmed;             // 当前是否处于调暗状态
} golf_idle_state_t;

// 进入页面时调用:从"刚有过操作、正常亮度"开始。
void golf_idle_init(golf_idle_state_t *state, uint64_t now_ms);

// 有按键事件时调用。任何按键、任何事件都算"有人在场",包括玩法本身不响应的那种。
// 返回 true 表示这次是从调暗状态被唤醒,调用方需要把背光调回正常亮度,
// 并且**应当把这次按压吞掉不再转给玩法**(否则在力度阶段按一下唤醒就会出杆)。
bool golf_idle_note_activity(golf_idle_state_t *state, uint64_t now_ms);

// 每个游戏节拍调用一次。
// 返回 true 表示本次应当把背光调暗 —— 只在状态翻转的那一次返回 true。
bool golf_idle_tick(golf_idle_state_t *state, uint64_t now_ms);
