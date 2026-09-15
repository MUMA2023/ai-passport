// tests/test_golf_model.c —— golf_model 的主机端测试。
// 编译方式见 tools/validate.sh:cc -std=c11 -Wall -Wextra -Werror -Imain
#include <assert.h>
#include <stdio.h>

#include "golf_model.h"

#define TICK_MS 20
#define MAX_TICKS 600

static int32_t to_fix(int world) {
    return (int32_t)world * GOLF_FIX;
}

static int count_pixels(const golf_model_t *model, golf_px_t want,
                        int x0, int y0, int x1, int y1) {
    int count = 0;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            if (golf_model_overlay_pixel(model, x, y) == want) count++;
        }
    }
    return count;
}

static bool inside_bounds(const golf_model_t *model) {
    const int x = golf_model_ball_pixel_x(model);
    const int y = golf_model_ball_pixel_y(model);
    // 球心被夹在 [R, W-R] 闭区间内,所以边界值本身是合法的。
    return x >= GOLF_BALL_RADIUS && x <= GOLF_WORLD_W - GOLF_BALL_RADIUS &&
           y >= GOLF_BALL_RADIUS && y <= GOLF_WORLD_H - GOLF_BALL_RADIUS;
}

// 推进到球停下或出现结果事件。返回 SUNK / WATER / NONE。
static golf_event_t run_until_settled(golf_model_t *model, int *ticks,
                                      bool *saw_bounce, bool *saw_sand) {
    for (int i = 0; i < MAX_TICKS; i++) {
        const golf_event_t event = golf_model_tick(model, TICK_MS);
        if (event == GOLF_EVENT_BOUNCE && saw_bounce) *saw_bounce = true;
        if (event == GOLF_EVENT_SAND && saw_sand) *saw_sand = true;
        if (event == GOLF_EVENT_SUNK || event == GOLF_EVENT_WATER) {
            if (ticks) *ticks = i;
            return event;
        }
        assert(inside_bounds(model));
        if (model->phase != GOLF_PHASE_ROLLING) {
            if (ticks) *ticks = i;
            return GOLF_EVENT_NONE;
        }
    }
    if (ticks) *ticks = MAX_TICKS;
    return GOLF_EVENT_NONE;
}

static void test_initial_state(void) {
    golf_model_t model;
    golf_model_init(&model);

    assert(model.hole_index == 0);
    assert(model.phase == GOLF_PHASE_AIM);
    assert(model.strokes == 0);
    assert(model.power_milli == 0);
    assert(model.scores[0] == 0 && model.scores[1] == 0 && model.scores[2] == 0);

    const golf_hole_t *hole = golf_model_hole(&model);
    assert(hole->par == 2);
    assert(golf_model_ball_pixel_x(&model) == hole->tee_x);
    assert(golf_model_ball_pixel_y(&model) == hole->tee_y);
    assert(model.angle_deg == hole->tee_angle);
    assert(golf_model_total_strokes(&model) == 0);
    assert(golf_model_total_par() == 8);
}

static void test_terrain_lookup(void) {
    golf_model_t model;

    golf_model_init(&model);
    assert(golf_model_terrain_at(&model, 150, 20) == GOLF_TERRAIN_FAIRWAY);
    assert(golf_model_terrain_at(&model, 190, 26) == GOLF_TERRAIN_GREEN);
    assert(golf_model_terrain_at(&model, 46, 58) == GOLF_TERRAIN_SAND);
    // 圆角之外必须是粗草,否则"看起来在果岭上"就不成立了。
    assert(golf_model_terrain_at(&model, 12, 12) == GOLF_TERRAIN_ROUGH);
    assert(golf_model_terrain_at(&model, 60, 130) == GOLF_TERRAIN_FAIRWAY);

    golf_model_start_hole(&model, 1);
    assert(golf_model_terrain_at(&model, 160, 30) == GOLF_TERRAIN_WATER);
    assert(golf_model_terrain_at(&model, 192, 122) == GOLF_TERRAIN_GREEN);
    assert(golf_model_terrain_at(&model, 30, 20) == GOLF_TERRAIN_FAIRWAY);

    golf_model_start_hole(&model, 2);
    assert(golf_model_terrain_at(&model, 180, 90) == GOLF_TERRAIN_WATER);
    assert(golf_model_terrain_at(&model, 140, 90) == GOLF_TERRAIN_FAIRWAY);
    assert(golf_model_terrain_at(&model, 192, 26) == GOLF_TERRAIN_GREEN);
}

static void test_scene_pixels(void) {
    golf_model_t model;
    golf_model_init(&model);
    const golf_hole_t *hole = golf_model_hole(&model);

    assert(golf_model_scene_pixel(&model, hole->cup_x, hole->cup_y) == GOLF_PX_CUP);
    assert(golf_model_scene_pixel(&model, hole->cup_x - 1, hole->cup_y - 8) == GOLF_PX_POLE);
    assert(golf_model_scene_pixel(&model, hole->cup_x + 3, hole->cup_y - 11) == GOLF_PX_FLAG);
    assert(golf_model_scene_pixel(&model, 46, 58) == GOLF_PX_SAND);
    assert(golf_model_scene_pixel(&model, 159, 105) == GOLF_PX_WALL);
    // 墙体投影落在墙右下方 3 像素处,且该点本身不在墙内。
    assert(golf_model_scene_pixel(&model, 169, 100) == GOLF_PX_WALL_SHADOW);
    assert(golf_model_scene_pixel(&model, 150, 20) == GOLF_PX_FAIRWAY);
    assert(golf_model_scene_pixel(&model, 2, 2) == GOLF_PX_ROUGH);
    assert(golf_model_scene_pixel(&model, -1, 5) == GOLF_PX_NONE);
    assert(golf_model_scene_pixel(&model, GOLF_WORLD_W, 5) == GOLF_PX_NONE);

    golf_model_start_hole(&model, 1);
    assert(golf_model_scene_pixel(&model, 160, 30) == GOLF_PX_WATER);
    assert(golf_model_scene_pixel(&model, 100, 20) == GOLF_PX_WALL);

    golf_model_start_hole(&model, 2);
    assert(golf_model_scene_pixel(&model, 180, 90) == GOLF_PX_WATER);
    assert(golf_model_scene_pixel(&model, 140, 90) == GOLF_PX_FAIRWAY);
}

// 开局瞄准线必须干净:模型给玩家的默认角度正对洞口,如果这条线上压着水塘或
// 墙壁,第一杆就是必输的设计陷阱。三洞都必须满足。
static void test_default_aim_line_is_clear(void) {
    golf_model_t model;
    golf_model_init(&model);

    for (uint8_t hole = 0; hole < GOLF_HOLE_COUNT; hole++) {
        golf_model_start_hole(&model, hole);
        const golf_hole_t *h = golf_model_hole(&model);
        for (int step = 0; step <= 240; step++) {
            const int x = h->tee_x + (h->cup_x - h->tee_x) * step / 240;
            const int y = h->tee_y + (h->cup_y - h->tee_y) * step / 240;
            assert(golf_model_terrain_at(&model, x, y) != GOLF_TERRAIN_WATER);
            // 碰撞会把墙按球半径外扩,所以球心要离墙至少 GOLF_BALL_RADIUS。
            for (int oy = -GOLF_BALL_RADIUS; oy <= GOLF_BALL_RADIUS; oy++) {
                for (int ox = -GOLF_BALL_RADIUS; ox <= GOLF_BALL_RADIUS; ox++) {
                    assert(golf_model_scene_pixel(&model, x + ox, y + oy) != GOLF_PX_WALL);
                }
            }
        }
    }
}

static void test_aim_wraps_and_phase_gate(void) {
    golf_model_t model;
    golf_model_init(&model);

    model.angle_deg = 350;
    assert(golf_model_aim(&model, 20) == GOLF_EVENT_AIM);
    assert(model.angle_deg == 10);

    model.angle_deg = 5;
    assert(golf_model_aim(&model, -20) == GOLF_EVENT_AIM);
    assert(model.angle_deg == 345);

    // 瞄准只在瞄准阶段生效。
    assert(golf_model_power_begin(&model) == GOLF_EVENT_POWER_START);
    const int16_t frozen = model.angle_deg;
    assert(golf_model_aim(&model, 10) == GOLF_EVENT_NONE);
    assert(model.angle_deg == frozen);
}

static void test_swing_meter_sweeps_and_bounces(void) {
    golf_model_t model;
    golf_model_init(&model);
    assert(golf_model_power_begin(&model) == GOLF_EVENT_POWER_START);
    assert(model.power_milli == 0);

    // 摆动步长是 1000 * dt / GOLF_SWING_SWEEP_MS,不一定正好落在 1000 上,
    // 因此只断言峰值足够高、并且随后折返。
    int peak = 0;
    bool returned_low = false;
    for (int i = 0; i < 200; i++) {
        golf_model_tick(&model, TICK_MS);
        assert(model.power_milli >= 0 && model.power_milli <= 1000);
        if (model.power_milli > peak) peak = model.power_milli;
        if (peak >= 960 && model.power_milli < 100) returned_low = true;
    }
    assert(peak >= 960);
    assert(returned_low);
    assert(model.phase == GOLF_PHASE_POWER);

    // 力度阶段可以退回瞄准。
    assert(golf_model_power_cancel(&model) == GOLF_EVENT_POWER_CANCEL);
    assert(model.phase == GOLF_PHASE_AIM);
    assert(model.power_milli == 0);
}

static void test_strike_direction_and_speed(void) {
    golf_model_t model;
    golf_model_init(&model);

    model.angle_deg = 0;
    model.power_milli = 1000;
    model.phase = GOLF_PHASE_POWER;
    assert(golf_model_strike(&model) == GOLF_EVENT_STRIKE);
    assert(model.phase == GOLF_PHASE_ROLLING);
    assert(model.strokes == 1);
    assert(model.vx > 0 && model.vy == 0);
    assert(model.vx == to_fix(GOLF_MAX_SPEED));

    golf_model_start_hole(&model, 0);
    model.angle_deg = 90;
    model.power_milli = 1000;
    model.phase = GOLF_PHASE_POWER;
    golf_model_strike(&model);
    // 屏幕 y 轴向下:90 度必须朝上。
    assert(model.vy < 0 && model.vx == 0);
    assert(model.vy == -to_fix(GOLF_MAX_SPEED));

    // 0 力度仍给最小出球速度,不会原地不动。
    golf_model_start_hole(&model, 0);
    model.angle_deg = 0;
    model.power_milli = 0;
    model.phase = GOLF_PHASE_POWER;
    golf_model_strike(&model);
    assert(model.vx == to_fix(GOLF_MIN_SPEED));

    // 非力度阶段击球无效。
    assert(golf_model_strike(&model) == GOLF_EVENT_NONE);
}

static void test_wall_bounce_keeps_ball_in_bounds(void) {
    golf_model_t model;
    golf_model_init(&model);
    // 第 1 洞右下角的柱子正对着这个角度。
    model.angle_deg = 10;
    model.power_milli = 1000;
    model.phase = GOLF_PHASE_POWER;
    golf_model_strike(&model);

    bool saw_bounce = false;
    int ticks = 0;
    const golf_event_t event = run_until_settled(&model, &ticks, &saw_bounce, NULL);
    assert(event == GOLF_EVENT_NONE);
    assert(saw_bounce);
    assert(ticks > 0);
    assert(inside_bounds(&model));
    assert(model.phase == GOLF_PHASE_AIM);
    assert(model.strokes == 1);
}

static void test_water_penalty_resets_to_shot_origin(void) {
    golf_model_t model;
    golf_model_start_hole(&model, 2);
    const int32_t origin_x = model.x;
    const int32_t origin_y = model.y;

    // 第 3 洞右前方就是水塘:拉平往右打必然下水。
    model.angle_deg = 20;
    model.power_milli = 1000;
    model.phase = GOLF_PHASE_POWER;
    golf_model_strike(&model);
    assert(model.strokes == 1);

    int ticks = 0;
    const golf_event_t event = run_until_settled(&model, &ticks, NULL, NULL);
    assert(event == GOLF_EVENT_WATER);
    assert(model.x == origin_x && model.y == origin_y);
    assert(model.phase == GOLF_PHASE_AIM);
    assert(model.vx == 0 && model.vy == 0);
    // 击球 1 杆 + 落水罚 1 杆。
    assert(model.strokes == 2);
}

static void test_slow_ball_drops_and_fast_ball_lips_out(void) {
    golf_model_t model;
    golf_model_start_hole(&model, 2);
    const golf_hole_t *hole = golf_model_hole(&model);
    const int32_t cup_y = to_fix(hole->cup_y);

    // 球停在洞口正下方 20 单位,轻轻一推必须进洞。
    model.x = to_fix(hole->cup_x);
    model.y = cup_y + to_fix(20);
    model.angle_deg = 90;
    model.power_milli = 150;
    model.phase = GOLF_PHASE_POWER;
    golf_model_strike(&model);
    const golf_event_t event = run_until_settled(&model, NULL, NULL, NULL);
    assert(event == GOLF_EVENT_SUNK);
    assert(model.phase == GOLF_PHASE_SUNK);
    assert(model.scores[2] == 1);
    assert(golf_model_ball_pixel_x(&model) == hole->cup_x);
    assert(golf_model_ball_pixel_y(&model) == hole->cup_y);

    // 同样的位置满力推杆,高速越过洞口不进洞。
    golf_model_start_hole(&model, 2);
    model.x = to_fix(hole->cup_x);
    model.y = cup_y + to_fix(20);
    model.angle_deg = 90;
    model.power_milli = 1000;
    model.phase = GOLF_PHASE_POWER;
    golf_model_strike(&model);
    for (int i = 0; i < 200 && model.y > cup_y; i++) {
        golf_model_tick(&model, TICK_MS);
    }
    assert(model.y <= cup_y);
    assert(model.phase == GOLF_PHASE_ROLLING);
}

static void test_round_progression_and_scoring(void) {
    golf_model_t model;
    golf_model_init(&model);
    assert(golf_model_total_par() == 8);

    for (uint8_t hole = 0; hole < GOLF_HOLE_COUNT; hole++) {
        assert(model.hole_index == hole);
        assert(model.phase == GOLF_PHASE_AIM);

        // 直接把球放到洞口下方再推入,验证计分与流程。
        model.x = to_fix(golf_model_hole(&model)->cup_x);
        model.y = to_fix(golf_model_hole(&model)->cup_y) + to_fix(20);
        model.angle_deg = 90;
        model.power_milli = 150;
        model.phase = GOLF_PHASE_POWER;
        golf_model_strike(&model);
        assert(run_until_settled(&model, NULL, NULL, NULL) == GOLF_EVENT_SUNK);
        assert(model.scores[hole] == 1);
        assert(golf_model_total_strokes(&model) == hole + 1);

        const golf_event_t event = golf_model_advance(&model);
        if (hole + 1 < GOLF_HOLE_COUNT) {
            assert(event == GOLF_EVENT_NONE);
        } else {
            assert(event == GOLF_EVENT_ROUND_DONE);
        }
    }

    assert(model.phase == GOLF_PHASE_ROUND_DONE);
    assert(golf_model_total_strokes(&model) == GOLF_HOLE_COUNT);

    golf_model_restart(&model);
    assert(model.hole_index == 0);
    assert(model.phase == GOLF_PHASE_AIM);
    assert(golf_model_total_strokes(&model) == 0);
    assert(model.scores[0] == 0 && model.scores[1] == 0 && model.scores[2] == 0);
}

static void test_overlay_ball_and_aim_guide(void) {
    golf_model_t model;
    golf_model_init(&model);
    model.angle_deg = 0;

    const int ball_x = golf_model_ball_pixel_x(&model);
    const int ball_y = golf_model_ball_pixel_y(&model);
    assert(golf_model_overlay_pixel(&model, ball_x, ball_y) == GOLF_PX_BALL);
    assert(count_pixels(&model, GOLF_PX_BALL, ball_x - 4, ball_y - 4,
                        ball_x + 4, ball_y + 4) > 0);

    // 瞄准辅助线只出现在球的前方。
    const int forward = count_pixels(&model, GOLF_PX_AIM, ball_x + 5, ball_y - 2,
                                     ball_x + GOLF_AIM_LENGTH - 1, ball_y + 2);
    const int backward = count_pixels(&model, GOLF_PX_AIM, ball_x - GOLF_AIM_LENGTH,
                                      ball_y - 2, ball_x - 5, ball_y + 2);
    assert(forward > 0);
    assert(backward == 0);

    // 球滚动时不再显示辅助线。
    model.power_milli = 500;
    model.phase = GOLF_PHASE_POWER;
    golf_model_strike(&model);
    assert(model.phase == GOLF_PHASE_ROLLING);
    assert(count_pixels(&model, GOLF_PX_AIM, 0, 0, GOLF_WORLD_W - 1, GOLF_WORLD_H - 1) == 0);
}

static void test_power_percent_mapping(void) {
    golf_model_t model;
    golf_model_init(&model);
    model.power_milli = 0;
    assert(golf_model_power_percent(&model) == 0);
    model.power_milli = 1000;
    assert(golf_model_power_percent(&model) == 100);
    model.power_milli = 505;
    assert(golf_model_power_percent(&model) == 51);
}

static void test_long_play_stays_in_bounds(void) {
    golf_model_t model;
    golf_model_init(&model);
    // 固定序列的角度与力度,覆盖全场跑一遍,球必须始终留在场地内。
    for (int shot = 0; shot < 120; shot++) {
        if (model.phase == GOLF_PHASE_SUNK) {
            golf_model_advance(&model);
            continue;
        }
        if (model.phase == GOLF_PHASE_ROUND_DONE) break;
        model.angle_deg = (int16_t)((shot * 47) % 360);
        model.power_milli = (int16_t)(100 + (shot * 137) % 900);
        model.phase = GOLF_PHASE_POWER;
        golf_model_strike(&model);
        run_until_settled(&model, NULL, NULL, NULL);
        assert(inside_bounds(&model));
        assert(model.phase == GOLF_PHASE_AIM || model.phase == GOLF_PHASE_SUNK);
    }
}

int main(void) {
    test_initial_state();
    test_terrain_lookup();
    test_scene_pixels();
    test_default_aim_line_is_clear();
    test_aim_wraps_and_phase_gate();
    test_swing_meter_sweeps_and_bounces();
    test_strike_direction_and_speed();
    test_wall_bounce_keeps_ball_in_bounds();
    test_water_penalty_resets_to_shot_origin();
    test_slow_ball_drops_and_fast_ball_lips_out();
    test_round_progression_and_scoring();
    test_overlay_ball_and_aim_guide();
    test_power_percent_mapping();
    test_long_play_stays_in_bounds();
    puts("golf_model: all tests passed");
    return 0;
}
