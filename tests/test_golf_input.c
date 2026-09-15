// tests/test_golf_input.c —— 按键翻译层的主机端测试。
//
// 这里覆盖的都是真机上很难靠肉眼发现的时序问题:button 组件在按下瞬间先给
// PRESS_DOWN,长按阈值到了才给 LONG_PRESS_START,抬起后还可能补一个 SINGLE_CLICK。
// 曾经的实现让一次长按变成 4 + 32 = 36 度,并且"击球后补的那个 CLICK"会顺手把
// 球推进下一洞。编译方式见 tools/validate.sh。
#include <assert.h>
#include <stdio.h>

#include "golf_input.h"
#include "golf_model.h"

#define TICK_MS 20

static void tick_until_settled(golf_model_t *model) {
    for (int i = 0; i < 600 && model->phase == GOLF_PHASE_ROLLING; i++) {
        golf_model_tick(model, TICK_MS);
    }
}

// 瞄准阶段确定键要等 CLICK:PRESS 只是按下瞬间的噪声,不能先把力度条点亮,
// 否则"长按确定返回菜单"会先闪出一格力度。
static void test_ok_press_in_aim_does_not_start_swing(void) {
    golf_model_t model;
    golf_key_state_t keys = {0};
    golf_model_init(&model);

    assert(golf_input_key(&model, &keys, GOLF_KEY_OK, GOLF_KEY_EV_PRESS, 1000) == GOLF_EVENT_NONE);
    assert(model.phase == GOLF_PHASE_AIM);
    assert(model.power_milli == 0);

    // 抬起后的 CLICK 才是真正的"进入力度"。
    assert(golf_input_key(&model, &keys, GOLF_KEY_OK, GOLF_KEY_EV_CLICK, 1300) ==
           GOLF_EVENT_POWER_START);
    assert(model.phase == GOLF_PHASE_POWER);
}

// 力度阶段必须按下即击球:摆动条单程 600ms,上果岭的窗口只有几十毫秒。
static void test_ok_press_in_power_strikes(void) {
    golf_model_t model;
    golf_key_state_t keys = {0};
    golf_model_init(&model);
    golf_input_key(&model, &keys, GOLF_KEY_OK, GOLF_KEY_EV_CLICK, 1000);
    assert(model.phase == GOLF_PHASE_POWER);
    model.power_milli = 1000;

    assert(golf_input_key(&model, &keys, GOLF_KEY_OK, GOLF_KEY_EV_PRESS, 1100) ==
           GOLF_EVENT_STRIKE);
    assert(model.phase == GOLF_PHASE_ROLLING);
    assert(model.strokes == 1);
}

// 击球后组件补的那个 CLICK 不能顺手把球推进下一洞。
static void test_trailing_click_does_not_advance_hole(void) {
    golf_model_t model;
    golf_key_state_t keys = {0};
    golf_model_init(&model);

    // 把球放到洞口正下方,轻推即可进洞。
    const golf_hole_t *hole = golf_model_hole(&model);
    model.x = (int32_t)hole->cup_x * GOLF_FIX;
    model.y = ((int32_t)hole->cup_y + 20) * GOLF_FIX;
    model.angle_deg = 90;
    model.power_milli = 150;
    model.phase = GOLF_PHASE_POWER;

    assert(golf_input_key(&model, &keys, GOLF_KEY_OK, GOLF_KEY_EV_PRESS, 1000) ==
           GOLF_EVENT_STRIKE);
    tick_until_settled(&model);
    assert(model.phase == GOLF_PHASE_SUNK);

    // 抬起后的 CLICK 落在同一个按压窗口内,必须被忽略。
    assert(golf_input_key(&model, &keys, GOLF_KEY_OK, GOLF_KEY_EV_CLICK, 1300) ==
           GOLF_EVENT_NONE);
    assert(model.hole_index == 0);
    assert(model.phase == GOLF_PHASE_SUNK);

    // 隔开足够久之后的一次点击才是"下一洞"。
    assert(golf_input_key(&model, &keys, GOLF_KEY_OK, GOLF_KEY_EV_CLICK, 5000) ==
           GOLF_EVENT_NONE);
    assert(model.hole_index == 1);
    assert(model.phase == GOLF_PHASE_AIM);
}

// 一次长按必须正好等于粗调步长,而不是"微调 + 粗调"。
static void test_long_aim_is_exactly_coarse_step(void) {
    golf_model_t model;
    golf_key_state_t keys = {0};
    golf_model_init(&model);
    const int16_t base = golf_model_hole(&model)->tee_angle;

    // PRESS_DOWN 先到,微调一格。
    assert(golf_input_key(&model, &keys, GOLF_KEY_UP, GOLF_KEY_EV_PRESS, 1000) ==
           GOLF_EVENT_AIM);
    assert(model.angle_deg == base + GOLF_AIM_STEP_DEG);

    // 长按阈值到达,补足差额。
    assert(golf_input_key(&model, &keys, GOLF_KEY_UP, GOLF_KEY_EV_LONG, 2500) ==
           GOLF_EVENT_AIM);
    assert(model.angle_deg == base + GOLF_AIM_COARSE_DEG);

    // 反向同理。
    golf_model_init(&model);
    assert(golf_input_key(&model, &keys, GOLF_KEY_DOWN, GOLF_KEY_EV_PRESS, 1000) ==
           GOLF_EVENT_AIM);
    assert(golf_input_key(&model, &keys, GOLF_KEY_DOWN, GOLF_KEY_EV_LONG, 2500) ==
           GOLF_EVENT_AIM);
    assert(model.angle_deg == base - GOLF_AIM_COARSE_DEG);
}

// 短按只走一格,抬起后的 CLICK 不会再补一格。
static void test_short_press_steps_once(void) {
    golf_model_t model;
    golf_key_state_t keys = {0};
    golf_model_init(&model);
    const int16_t base = golf_model_hole(&model)->tee_angle;

    golf_input_key(&model, &keys, GOLF_KEY_UP, GOLF_KEY_EV_PRESS, 1000);
    assert(model.angle_deg == base + GOLF_AIM_STEP_DEG);

    // 同一个按压窗口内。
    golf_input_key(&model, &keys, GOLF_KEY_UP, GOLF_KEY_EV_CLICK, 1300);
    assert(model.angle_deg == base + GOLF_AIM_STEP_DEG);

    // 就算隔了很久,只要这个键上报过 PRESS,CLICK 就不该再走一步。
    golf_input_key(&model, &keys, GOLF_KEY_UP, GOLF_KEY_EV_CLICK, 9000);
    assert(model.angle_deg == base + GOLF_AIM_STEP_DEG);
}

// 兜底:若某个组件版本只上报 CLICK,瞄准至少还能微调。
static void test_click_only_fallback_aims(void) {
    golf_model_t model;
    golf_key_state_t keys = {0};
    golf_model_init(&model);
    const int16_t base = golf_model_hole(&model)->tee_angle;

    assert(golf_input_key(&model, &keys, GOLF_KEY_UP, GOLF_KEY_EV_CLICK, 1000) ==
           GOLF_EVENT_AIM);
    assert(model.angle_deg == base + GOLF_AIM_STEP_DEG);
}

// 力度阶段长按上键等同于放弃本杆。
static void test_power_phase_up_long_cancels(void) {
    golf_model_t model;
    golf_key_state_t keys = {0};
    golf_model_init(&model);
    golf_input_key(&model, &keys, GOLF_KEY_OK, GOLF_KEY_EV_CLICK, 1000);
    assert(model.phase == GOLF_PHASE_POWER);

    // PRESS 先到:此时不在瞄准阶段,不能改角度。
    assert(golf_input_key(&model, &keys, GOLF_KEY_UP, GOLF_KEY_EV_PRESS, 1100) ==
           GOLF_EVENT_NONE);
    assert(model.angle_deg == golf_model_hole(&model)->tee_angle);

    assert(golf_input_key(&model, &keys, GOLF_KEY_UP, GOLF_KEY_EV_LONG, 2600) ==
           GOLF_EVENT_POWER_CANCEL);
    assert(model.phase == GOLF_PHASE_AIM);
    assert(model.power_milli == 0);
}

// 整轮结束后点击确定 = 重开,杆数与成绩全部归零。
static void test_round_done_click_restarts(void) {
    golf_model_t model;
    golf_key_state_t keys = {0};
    golf_model_init(&model);
    model.hole_index = (uint8_t)(GOLF_HOLE_COUNT - 1);
    model.phase = GOLF_PHASE_ROUND_DONE;
    model.scores[0] = 3;
    model.scores[1] = 4;
    model.scores[2] = 5;

    assert(golf_input_key(&model, &keys, GOLF_KEY_OK, GOLF_KEY_EV_CLICK, 1000) ==
           GOLF_EVENT_NONE);
    assert(model.hole_index == 0);
    assert(model.phase == GOLF_PHASE_AIM);
    assert(golf_model_total_strokes(&model) == 0);
    assert(model.scores[0] == 0 && model.scores[1] == 0 && model.scores[2] == 0);
}

// 双击与越界键值都不参与本页交互。
static void test_unrelated_events_are_ignored(void) {
    golf_model_t model;
    golf_key_state_t keys = {0};
    golf_model_init(&model);
    const int16_t base = golf_model_hole(&model)->tee_angle;

    assert(golf_input_key(&model, &keys, GOLF_KEY_OK, (golf_key_ev_t)99, 1000) ==
           GOLF_EVENT_NONE);
    assert(golf_input_key(&model, &keys, GOLF_KEY_OK, GOLF_KEY_EV_PRESS, 1000) ==
           GOLF_EVENT_NONE);
    assert(golf_input_key(&model, &keys, (golf_key_t)99, GOLF_KEY_EV_CLICK, 1000) ==
           GOLF_EVENT_NONE);
    assert(model.phase == GOLF_PHASE_AIM);
    assert(model.angle_deg == base);
}

int main(void) {
    test_ok_press_in_aim_does_not_start_swing();
    test_ok_press_in_power_strikes();
    test_trailing_click_does_not_advance_hole();
    test_long_aim_is_exactly_coarse_step();
    test_short_press_steps_once();
    test_click_only_fallback_aims();
    test_power_phase_up_long_cancels();
    test_round_done_click_restarts();
    test_unrelated_events_are_ignored();
    puts("golf_input: all tests passed");
    return 0;
}
