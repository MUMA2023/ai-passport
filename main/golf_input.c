// main/golf_input.c —— 按键翻译层的纯逻辑实现。见 golf_input.h 的规则说明。
#include "golf_input.h"

// 一次按压会产生 PRESS,抬起后可能再产生 CLICK。把两者收敛成"每次按压只算一次":
// PRESS 先到并记账,紧随其后的 CLICK 丢弃;距离上次 PRESS 已超过窗口的 CLICK
// 当作一次独立按压(兜底只上报 CLICK 的组件版本)。
static bool accept_once(golf_key_state_t *state, golf_key_t key,
                        golf_key_ev_t ev, uint64_t now_ms) {
    if (ev == GOLF_KEY_EV_PRESS) {
        state->press_seen[key] = true;
        state->last_press_ms[key] = now_ms;
        return true;
    }
    if (ev != GOLF_KEY_EV_CLICK) return false;
    return !state->press_seen[key] ||
           now_ms - state->last_press_ms[key] > GOLF_PRESS_CLICK_WINDOW_MS;
}

// 这个 CLICK 是不是刚才那次 PRESS 的尾巴?
static bool click_is_trailing(const golf_key_state_t *state, golf_key_t key,
                              uint64_t now_ms) {
    return state->press_seen[key] &&
           now_ms - state->last_press_ms[key] <= GOLF_PRESS_CLICK_WINDOW_MS;
}

static golf_event_t handle_aim_key(golf_model_t *model, golf_key_state_t *state,
                                   golf_key_t key, golf_key_ev_t ev, uint64_t now_ms) {
    const int dir = (key == GOLF_KEY_UP) ? 1 : -1;

    if (ev == GOLF_KEY_EV_PRESS) {
        // 记账,否则抬起后的 CLICK 会被误当成"没有 PRESS 的兜底点击"而多走一格。
        state->press_seen[key] = true;
        state->last_press_ms[key] = now_ms;
        return golf_model_aim(model, dir * GOLF_AIM_STEP_DEG);
    }
    if (ev == GOLF_KEY_EV_LONG) {
        // PRESS_DOWN 已经先走了一格微调,这里补足到粗调步长。
        const golf_event_t event =
            golf_model_aim(model, dir * (GOLF_AIM_COARSE_DEG - GOLF_AIM_STEP_DEG));
        if (event != GOLF_EVENT_NONE) return event;
        // 非瞄准阶段(例如力度阶段):长按上键等同于放弃本杆。
        if (key == GOLF_KEY_UP) return golf_model_power_cancel(model);
        return GOLF_EVENT_NONE;
    }
    // 兜底:若某个组件版本只上报 CLICK,至少还能微调。
    if (!state->press_seen[key]) {
        return golf_model_aim(model, dir * GOLF_AIM_STEP_DEG);
    }
    return GOLF_EVENT_NONE;
}

golf_event_t golf_input_key(golf_model_t *model, golf_key_state_t *state,
                            golf_key_t key, golf_key_ev_t ev, uint64_t now_ms) {
    if ((unsigned)key >= (unsigned)GOLF_KEY_COUNT) return GOLF_EVENT_NONE;

    if (key == GOLF_KEY_UP || key == GOLF_KEY_DOWN) {
        return handle_aim_key(model, state, key, ev, now_ms);
    }

    // 力度阶段必须按下即击球:摆动条单程 600ms,上果岭的窗口只有几十毫秒。
    // 其余阶段等 CLICK,这样"长按确定返回菜单"不会先闪出一格力度。
    if (model->phase == GOLF_PHASE_POWER) {
        if (!accept_once(state, key, ev, now_ms)) return GOLF_EVENT_NONE;
    } else if (ev == GOLF_KEY_EV_CLICK) {
        // 击球后组件还会补一个 CLICK,别让它顺手把球推进下一洞。
        if (click_is_trailing(state, key, now_ms)) return GOLF_EVENT_NONE;
    } else {
        return GOLF_EVENT_NONE;
    }

    switch (model->phase) {
        case GOLF_PHASE_AIM:
            return golf_model_power_begin(model);
        case GOLF_PHASE_POWER:
            return golf_model_strike(model);
        case GOLF_PHASE_SUNK:
            return golf_model_advance(model);
        case GOLF_PHASE_ROUND_DONE:
            golf_model_restart(model);
            return GOLF_EVENT_NONE;
        default:
            return GOLF_EVENT_NONE;
    }
}
