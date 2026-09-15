// main/golf_idle.c —— 闲置调光状态机的实现。规则见 golf_idle.h。
#include "golf_idle.h"

void golf_idle_init(golf_idle_state_t *state, uint64_t now_ms) {
    state->last_activity_ms = now_ms;
    state->dimmed = false;
}

bool golf_idle_note_activity(golf_idle_state_t *state, uint64_t now_ms) {
    state->last_activity_ms = now_ms;
    if (!state->dimmed) return false;   // 本来就亮着,没有状态要翻转
    state->dimmed = false;
    return true;                        // 从暗到亮,这一帧要重设背光
}

bool golf_idle_tick(golf_idle_state_t *state, uint64_t now_ms) {
    if (state->dimmed) return false;    // 已经在暗着,不重复上报
    // 无符号相减:只要间隔不超过 2^64 毫秒就不会因为回绕出错。
    if (now_ms - state->last_activity_ms < GOLF_IDLE_DIM_MS) return false;
    state->dimmed = true;
    return true;
}
