// main/golf_model.c —— 迷你高尔夫的球场、物理与逐像素渲染语义。
//
// 全部计算使用定点整数:ESP32-C3 是 RV32IMC,没有硬件浮点,浮点会退化成软件模拟。
// 位置与速度以 1/GOLF_FIX 世界单位表示,方向以 Q12(单位向量 = 4096)表示。
#include "golf_model.h"

#include <string.h>

// 摩擦系数,单位是"每秒衰减率 x 1024"。数值越大,球停得越快。
#define GOLF_FRICTION_ROUGH_Q10   3277   // 3.2 /s
#define GOLF_FRICTION_FAIRWAY_Q10 2048   // 2.0 /s
#define GOLF_FRICTION_GREEN_Q10   2765   // 2.7 /s
#define GOLF_FRICTION_SAND_Q10    5120   // 5.0 /s

// 碰墙弹性,以 1024 为 1.0。
#define GOLF_RESTITUTION_Q10 563   // 0.55

// 每个子步最多推进的世界单位,用来避免高速穿墙/穿洞。
#define GOLF_SUBSTEP_UNITS 2
#define GOLF_MAX_SUBSTEPS 16

// 方向向量的定点精度。
#define GOLF_DIR_Q12 4096

// 球杆摆动一次 tick 的最大时长,防止掉帧时力度条跳变。
#define GOLF_MAX_TICK_MS 100

// 瞄准辅助线的半宽(定点),约 0.68 世界单位。
#define GOLF_AIM_HALF_WIDTH_FIX 700

// cos(k°),k = 0..90,放大 4096 倍。sin 用 cos(90 - k) 得到。
static const int16_t GOLF_COS_Q12[91] = {
    4096, 4095, 4094, 4090, 4086, 4080, 4074, 4065, 4056, 4046, 4034, 4021, 4006,
    3991, 3974, 3956, 3937, 3917, 3896, 3873, 3849, 3824, 3798, 3770, 3742, 3712,
    3681, 3650, 3617, 3582, 3547, 3511, 3474, 3435, 3396, 3355, 3314, 3271, 3228,
    3183, 3138, 3091, 3044, 2996, 2946, 2896, 2845, 2793, 2741, 2687, 2633, 2578,
    2522, 2465, 2408, 2349, 2290, 2231, 2171, 2110, 2048, 1986, 1923, 1860, 1796,
    1731, 1666, 1600, 1534, 1468, 1401, 1334, 1266, 1198, 1129, 1060,  991,  921,
     852,  782,  711,  641,  570,  499,  428,  357,  286,  214,  143,   71,    0,
};

// ---------------------------------------------------------------------------
// 球场定义。世界尺寸 224 x 152;球心可达 x[3,221] y[3,149]。
// ---------------------------------------------------------------------------

// 第 1 洞:开局洞。左上角一道沙坑惩罚瞄得太高的球,右下角一根柱子惩罚拉平的球,
// 中间留出干净的通道——默认瞄准角正对着这条通道。2 杆洞。
static const golf_zone_t GOLF_ZONES_1[] = {
    { 10, 10, 200, 130, 18, GOLF_TERRAIN_FAIRWAY },
    { 28, 48,  44,  24, 11, GOLF_TERRAIN_SAND    },
    {164,  4,  56,  46, 20, GOLF_TERRAIN_GREEN   },
};
static const golf_wall_t GOLF_WALLS_1[] = {
    {150, 92, 18, 28 },
};

// 第 2 洞:狗腿弯。上沿垂下一段矮墙,专挡"贴顶抄近路"的高球;右上角的水塘则
// 惩罚过早转平的低球。两者之间留出宽阔通道,开局瞄准线正好从通道中间穿过,
// 所以第一杆不会撞墙——难点是别把球打高。球道向下延伸到 y=146。3 杆洞。
static const golf_zone_t GOLF_ZONES_2[] = {
    { 10, 10, 204, 136, 16, GOLF_TERRAIN_FAIRWAY },
    {132, 14,  62,  42, 12, GOLF_TERRAIN_WATER   },
    {166, 98,  52,  48, 20, GOLF_TERRAIN_GREEN   },
};
static const golf_wall_t GOLF_WALLS_2[] = {
    { 94,  2, 14, 42 },
};

// 第 3 洞:右侧一大片水塘守着果岭正下方,拉平往右走必下水;开局瞄准线从水塘
// 左上角擦过,所以正确打法是先沿左边上去、再从上方切进果岭。难点是别贪心
// 打平——越平越靠右,越容易下水。3 杆洞。
static const golf_zone_t GOLF_ZONES_3[] = {
    { 10, 10, 204, 132, 24, GOLF_TERRAIN_FAIRWAY },
    {150, 70,  74,  40,  0, GOLF_TERRAIN_WATER   },
    {166,  4,  54,  46, 20, GOLF_TERRAIN_GREEN   },
};

static const golf_hole_t GOLF_HOLES[GOLF_HOLE_COUNT] = {
    { "Side Step", 2,  26, 128, 190,  26, 32, GOLF_ZONES_1, 3, GOLF_WALLS_1, 1 },
    { "Dogleg",    3,  26,  26, 192, 122, 330, GOLF_ZONES_2, 3, GOLF_WALLS_2, 1 },
    { "Pond Side", 3,  30, 136, 192,  26, 34, GOLF_ZONES_3, 3, NULL,         0 },
};

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

static int32_t golf_abs32(int32_t value) {
    return value < 0 ? -value : value;
}

static int32_t golf_clamp32(int32_t value, int32_t low, int32_t high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

static int32_t golf_friction_q10(uint8_t terrain) {
    switch (terrain) {
        case GOLF_TERRAIN_GREEN:   return GOLF_FRICTION_GREEN_Q10;
        case GOLF_TERRAIN_SAND:    return GOLF_FRICTION_SAND_Q10;
        case GOLF_TERRAIN_FAIRWAY: return GOLF_FRICTION_FAIRWAY_Q10;
        case GOLF_TERRAIN_ROUGH:   return GOLF_FRICTION_ROUGH_Q10;
        default:                   return GOLF_FRICTION_ROUGH_Q10;
    }
}

// 角度 -> 屏幕方向向量(Q12)。0 度向右,90 度向上(屏幕 y 轴向下,故取负)。
static void golf_direction_q12(int angle_deg, int32_t *out_x, int32_t *out_y) {
    int angle = angle_deg % 360;
    if (angle < 0) angle += 360;
    const int quadrant = angle / 90;
    const int rest = angle % 90;
    int32_t cos_a;
    int32_t sin_a;
    switch (quadrant) {
        case 0:  cos_a =  GOLF_COS_Q12[rest];      sin_a =  GOLF_COS_Q12[90 - rest]; break;
        case 1:  cos_a = -GOLF_COS_Q12[90 - rest]; sin_a =  GOLF_COS_Q12[rest];      break;
        case 2:  cos_a = -GOLF_COS_Q12[rest];      sin_a = -GOLF_COS_Q12[90 - rest]; break;
        default: cos_a =  GOLF_COS_Q12[90 - rest]; sin_a = -GOLF_COS_Q12[rest];      break;
    }
    *out_x = cos_a;
    *out_y = -sin_a;
}

// 圆角矩形点测试。渲染与地形判定共用,保证所见即所判。
static bool golf_zone_contains(const golf_zone_t *zone, int x, int y) {
    if (x < zone->x || y < zone->y) return false;
    if (x >= zone->x + zone->w || y >= zone->y + zone->h) return false;
    const int radius = zone->radius;
    if (radius <= 0) return true;

    const int local_x = x - zone->x;
    const int local_y = y - zone->y;
    const int corner_x = golf_clamp32(local_x, radius, zone->w - 1 - radius);
    const int corner_y = golf_clamp32(local_y, radius, zone->h - 1 - radius);
    const int dx = local_x - corner_x;
    const int dy = local_y - corner_y;
    return dx * dx + dy * dy <= radius * radius;
}

static bool golf_wall_contains(const golf_hole_t *hole, int x, int y) {
    for (uint8_t i = 0; i < hole->wall_count; i++) {
        const golf_wall_t *wall = &hole->walls[i];
        if (x >= wall->x && x < wall->x + wall->w &&
            y >= wall->y && y < wall->y + wall->h) {
            return true;
        }
    }
    return false;
}

// 球心所在的世界整数坐标(四舍五入)。
static int golf_ball_world_x(const golf_model_t *model) {
    return (int)((model->x + GOLF_FIX / 2) >> 10);
}

static int golf_ball_world_y(const golf_model_t *model) {
    return (int)((model->y + GOLF_FIX / 2) >> 10);
}

static bool golf_inside_cup(const golf_model_t *model) {
    const golf_hole_t *hole = golf_model_hole(model);
    const int64_t dx = model->x - (int64_t)hole->cup_x * GOLF_FIX;
    const int64_t dy = model->y - (int64_t)hole->cup_y * GOLF_FIX;
    const int64_t radius = (int64_t)GOLF_CUP_RADIUS * GOLF_FIX;
    return dx * dx + dy * dy <= radius * radius;
}

static int64_t golf_speed_squared(const golf_model_t *model) {
    return (int64_t)model->vx * model->vx + (int64_t)model->vy * model->vy;
}

// ---------------------------------------------------------------------------
// 对外查询
// ---------------------------------------------------------------------------

const golf_hole_t *golf_model_hole(const golf_model_t *model) {
    uint8_t index = model->hole_index;
    if (index >= GOLF_HOLE_COUNT) index = GOLF_HOLE_COUNT - 1;
    return &GOLF_HOLES[index];
}

uint8_t golf_model_terrain_at(const golf_model_t *model, int wx, int wy) {
    const golf_hole_t *hole = golf_model_hole(model);
    uint8_t terrain = GOLF_TERRAIN_ROUGH;
    for (uint8_t i = 0; i < hole->zone_count; i++) {
        if (golf_zone_contains(&hole->zones[i], wx, wy)) {
            terrain = hole->zones[i].terrain;
        }
    }
    return terrain;
}

static golf_px_t golf_terrain_pixel(uint8_t terrain) {
    switch (terrain) {
        case GOLF_TERRAIN_FAIRWAY: return GOLF_PX_FAIRWAY;
        case GOLF_TERRAIN_GREEN:   return GOLF_PX_GREEN;
        case GOLF_TERRAIN_SAND:    return GOLF_PX_SAND;
        case GOLF_TERRAIN_WATER:   return GOLF_PX_WATER;
        default:                   return GOLF_PX_ROUGH;
    }
}

golf_px_t golf_model_scene_pixel(const golf_model_t *model, int px, int py) {
    if (px < 0 || py < 0 || px >= GOLF_WORLD_W || py >= GOLF_WORLD_H) return GOLF_PX_NONE;
    const golf_hole_t *hole = golf_model_hole(model);

    if (golf_wall_contains(hole, px, py)) return GOLF_PX_WALL;
    // 墙体投影:同一面墙向右下偏移 3 像素。
    if (golf_wall_contains(hole, px - 3, py - 3)) return GOLF_PX_WALL_SHADOW;

    const int cup_dx = px - hole->cup_x;
    const int cup_dy = py - hole->cup_y;
    if (cup_dx * cup_dx + cup_dy * cup_dy <= GOLF_CUP_RADIUS * GOLF_CUP_RADIUS) {
        return GOLF_PX_CUP;
    }

    // 旗面:以旗杆顶端为起点的三角旗,中间最宽。
    const int flag_row = py - (hole->cup_y - 16);
    if (flag_row >= 0 && flag_row < 11) {
        const int half = (flag_row < 6) ? flag_row : (10 - flag_row);
        if (px >= hole->cup_x + 1 && px <= hole->cup_x + 1 + half) return GOLF_PX_FLAG;
    }
    if ((px == hole->cup_x - 1 || px == hole->cup_x) &&
        py >= hole->cup_y - 16 && py < hole->cup_y) {
        return GOLF_PX_POLE;
    }

    return golf_terrain_pixel(golf_model_terrain_at(model, px, py));
}

golf_px_t golf_model_overlay_pixel(const golf_model_t *model, int px, int py) {
    if (px < 0 || py < 0 || px >= GOLF_WORLD_W || py >= GOLF_WORLD_H) return GOLF_PX_NONE;
    const int ball_x = golf_model_ball_pixel_x(model);
    const int ball_y = golf_model_ball_pixel_y(model);

    const int dx = px - ball_x;
    const int dy = py - ball_y;
    const int distance = dx * dx + dy * dy;
    const int inner = GOLF_BALL_RADIUS - 1;
    if (distance <= inner * inner) return GOLF_PX_BALL;
    if (distance <= GOLF_BALL_RADIUS * GOLF_BALL_RADIUS) return GOLF_PX_BALL_EDGE;

    const int shadow_dx = px - (ball_x + 1);
    const int shadow_dy = py - (ball_y + 1);
    if (shadow_dx * shadow_dx + shadow_dy * shadow_dy <= GOLF_BALL_RADIUS * GOLF_BALL_RADIUS) {
        return GOLF_PX_BALL_SHADOW;
    }

    if (model->phase != GOLF_PHASE_AIM) return GOLF_PX_NONE;

    int32_t dir_x;
    int32_t dir_y;
    golf_direction_q12(model->angle_deg, &dir_x, &dir_y);
    const int64_t rel_x = (int64_t)px * GOLF_FIX + GOLF_FIX / 2 - model->x;
    const int64_t rel_y = (int64_t)py * GOLF_FIX + GOLF_FIX / 2 - model->y;
    const int64_t along = (rel_x * dir_x + rel_y * dir_y) / GOLF_DIR_Q12;
    if (along < (int64_t)GOLF_BALL_RADIUS * GOLF_FIX) return GOLF_PX_NONE;
    if (along > (int64_t)GOLF_AIM_LENGTH * GOLF_FIX) return GOLF_PX_NONE;
    int64_t across = (rel_x * dir_y - rel_y * dir_x) / GOLF_DIR_Q12;
    if (across < 0) across = -across;
    if (across > GOLF_AIM_HALF_WIDTH_FIX) return GOLF_PX_NONE;
    // 虚线:每 3 个世界单位交替一次。
    if (((along >> 10) / 3) % 2) return GOLF_PX_NONE;
    return GOLF_PX_AIM;
}

int golf_model_power_percent(const golf_model_t *model) {
    int percent = (model->power_milli + 5) / 10;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    return percent;
}

int golf_model_total_strokes(const golf_model_t *model) {
    int total = 0;
    for (int i = 0; i < GOLF_HOLE_COUNT; i++) total += model->scores[i];
    if (model->phase != GOLF_PHASE_SUNK && model->phase != GOLF_PHASE_ROUND_DONE) {
        total += model->strokes;
    }
    return total;
}

int golf_model_total_par(void) {
    int total = 0;
    for (int i = 0; i < GOLF_HOLE_COUNT; i++) total += GOLF_HOLES[i].par;
    return total;
}

int golf_model_ball_pixel_x(const golf_model_t *model) {
    return golf_ball_world_x(model);
}

int golf_model_ball_pixel_y(const golf_model_t *model) {
    return golf_ball_world_y(model);
}

// 瞄准线末端。方向是 Q12 单位向量,这里换算成世界单位后叠加到球心。
static int golf_aim_tip_axis(const golf_model_t *model, bool horizontal) {
    int32_t dir_x;
    int32_t dir_y;
    golf_direction_q12(model->angle_deg, &dir_x, &dir_y);
    const int32_t dir = horizontal ? dir_x : dir_y;
    const int32_t origin = horizontal ? model->x : model->y;
    return (int)(origin + ((int64_t)GOLF_AIM_LENGTH * GOLF_FIX * dir) / GOLF_DIR_Q12) / GOLF_FIX;
}

int golf_model_aim_tip_pixel_x(const golf_model_t *model) {
    return golf_aim_tip_axis(model, true);
}

int golf_model_aim_tip_pixel_y(const golf_model_t *model) {
    return golf_aim_tip_axis(model, false);
}

// ---------------------------------------------------------------------------
// 状态机
// ---------------------------------------------------------------------------

void golf_model_start_hole(golf_model_t *model, uint8_t index) {
    if (index >= GOLF_HOLE_COUNT) index = GOLF_HOLE_COUNT - 1;
    const golf_hole_t *hole = &GOLF_HOLES[index];
    model->hole_index = index;
    model->strokes = 0;
    model->sunk_this_hole = 0;
    model->phase = GOLF_PHASE_AIM;
    model->power_milli = 0;
    model->power_dir = 1;
    model->x = (int32_t)hole->tee_x * GOLF_FIX;
    model->y = (int32_t)hole->tee_y * GOLF_FIX;
    model->vx = 0;
    model->vy = 0;
    model->shot_x = model->x;
    model->shot_y = model->y;
    model->angle_deg = hole->tee_angle;
    model->terrain = golf_model_terrain_at(model, hole->tee_x, hole->tee_y);
    model->last_event = GOLF_EVENT_NONE;
}

void golf_model_restart(golf_model_t *model) {
    for (int i = 0; i < GOLF_HOLE_COUNT; i++) model->scores[i] = 0;
    golf_model_start_hole(model, 0);
}

void golf_model_init(golf_model_t *model) {
    memset(model, 0, sizeof(*model));
    golf_model_restart(model);
}

golf_event_t golf_model_aim(golf_model_t *model, int delta_deg) {
    if (model->phase != GOLF_PHASE_AIM) return GOLF_EVENT_NONE;
    if (delta_deg == 0) return GOLF_EVENT_NONE;
    int angle = model->angle_deg + delta_deg;
    angle %= 360;
    if (angle < 0) angle += 360;
    model->angle_deg = (int16_t)angle;
    return GOLF_EVENT_AIM;
}

golf_event_t golf_model_power_begin(golf_model_t *model) {
    if (model->phase != GOLF_PHASE_AIM) return GOLF_EVENT_NONE;
    model->phase = GOLF_PHASE_POWER;
    model->power_milli = 0;
    model->power_dir = 1;
    return GOLF_EVENT_POWER_START;
}

golf_event_t golf_model_power_cancel(golf_model_t *model) {
    if (model->phase != GOLF_PHASE_POWER) return GOLF_EVENT_NONE;
    model->phase = GOLF_PHASE_AIM;
    model->power_milli = 0;
    model->power_dir = 1;
    return GOLF_EVENT_POWER_CANCEL;
}

golf_event_t golf_model_strike(golf_model_t *model) {
    if (model->phase != GOLF_PHASE_POWER) return GOLF_EVENT_NONE;

    int32_t dir_x;
    int32_t dir_y;
    golf_direction_q12(model->angle_deg, &dir_x, &dir_y);
    const int32_t speed = GOLF_MIN_SPEED +
        (GOLF_MAX_SPEED - GOLF_MIN_SPEED) * model->power_milli / 1000;
    model->vx = (int32_t)(((int64_t)speed * dir_x) / GOLF_DIR_Q12) * GOLF_FIX;
    model->vy = (int32_t)(((int64_t)speed * dir_y) / GOLF_DIR_Q12) * GOLF_FIX;

    model->shot_x = model->x;
    model->shot_y = model->y;
    if (model->strokes < 255) model->strokes++;
    model->phase = GOLF_PHASE_ROLLING;
    model->power_milli = 0;
    model->power_dir = 1;
    return GOLF_EVENT_STRIKE;
}

golf_event_t golf_model_advance(golf_model_t *model) {
    if (model->phase != GOLF_PHASE_SUNK) return GOLF_EVENT_NONE;
    if (model->hole_index + 1 < GOLF_HOLE_COUNT) {
        golf_model_start_hole(model, (uint8_t)(model->hole_index + 1));
        return GOLF_EVENT_NONE;
    }
    model->phase = GOLF_PHASE_ROUND_DONE;
    return GOLF_EVENT_ROUND_DONE;
}

// ---------------------------------------------------------------------------
// 物理
// ---------------------------------------------------------------------------

static int32_t golf_apply_restitution(int32_t velocity) {
    return (int32_t)(((int64_t)velocity * GOLF_RESTITUTION_Q10) / 1024);
}

// 边界与障碍墙反弹。返回本子步是否发生过碰撞。
static bool golf_resolve_collisions(golf_model_t *model) {
    bool bounced = false;
    const int32_t radius = GOLF_BALL_RADIUS * GOLF_FIX;
    const int32_t min_x = radius;
    const int32_t max_x = (int32_t)GOLF_WORLD_W * GOLF_FIX - radius;
    const int32_t min_y = radius;
    const int32_t max_y = (int32_t)GOLF_WORLD_H * GOLF_FIX - radius;

    if (model->x < min_x) {
        model->x = min_x;
        if (model->vx < 0) { model->vx = -golf_apply_restitution(model->vx); bounced = true; }
    } else if (model->x > max_x) {
        model->x = max_x;
        if (model->vx > 0) { model->vx = -golf_apply_restitution(model->vx); bounced = true; }
    }
    if (model->y < min_y) {
        model->y = min_y;
        if (model->vy < 0) { model->vy = -golf_apply_restitution(model->vy); bounced = true; }
    } else if (model->y > max_y) {
        model->y = max_y;
        if (model->vy > 0) { model->vy = -golf_apply_restitution(model->vy); bounced = true; }
    }

    const golf_hole_t *hole = golf_model_hole(model);
    for (uint8_t i = 0; i < hole->wall_count; i++) {
        const golf_wall_t *wall = &hole->walls[i];
        const int32_t left = (int32_t)wall->x * GOLF_FIX;
        const int32_t top = (int32_t)wall->y * GOLF_FIX;
        const int32_t right = left + (int32_t)wall->w * GOLF_FIX;
        const int32_t bottom = top + (int32_t)wall->h * GOLF_FIX;
        const int32_t near_x = golf_clamp32(model->x, left, right);
        const int32_t near_y = golf_clamp32(model->y, top, bottom);
        const int32_t dx = model->x - near_x;
        const int32_t dy = model->y - near_y;

        if (dx == 0 && dy == 0) {
            // 球心已在墙内:沿穿透最浅的一侧推出,再翻转该轴速度。
            const int32_t to_left = model->x - left;
            const int32_t to_right = right - model->x;
            const int32_t to_top = model->y - top;
            const int32_t to_bottom = bottom - model->y;
            if (to_left <= to_right && to_left <= to_top && to_left <= to_bottom) {
                model->x = left - radius;
                if (model->vx > 0) { model->vx = -golf_apply_restitution(model->vx); bounced = true; }
            } else if (to_right <= to_top && to_right <= to_bottom) {
                model->x = right + radius;
                if (model->vx < 0) { model->vx = -golf_apply_restitution(model->vx); bounced = true; }
            } else if (to_top <= to_bottom) {
                model->y = top - radius;
                if (model->vy > 0) { model->vy = -golf_apply_restitution(model->vy); bounced = true; }
            } else {
                model->y = bottom + radius;
                if (model->vy < 0) { model->vy = -golf_apply_restitution(model->vy); bounced = true; }
            }
            continue;
        }

        if ((int64_t)dx * dx + (int64_t)dy * dy >= (int64_t)radius * radius) continue;

        // 轴对齐的墙面:沿偏移较大的那一轴推出并反弹。
        if (golf_abs32(dx) >= golf_abs32(dy)) {
            if (dx > 0) {
                model->x = near_x + radius;
                if (model->vx < 0) { model->vx = -golf_apply_restitution(model->vx); bounced = true; }
            } else {
                model->x = near_x - radius;
                if (model->vx > 0) { model->vx = -golf_apply_restitution(model->vx); bounced = true; }
            }
        } else {
            if (dy > 0) {
                model->y = near_y + radius;
                if (model->vy < 0) { model->vy = -golf_apply_restitution(model->vy); bounced = true; }
            } else {
                model->y = near_y - radius;
                if (model->vy > 0) { model->vy = -golf_apply_restitution(model->vy); bounced = true; }
            }
        }
    }
    return bounced;
}

static golf_event_t golf_sink_ball(golf_model_t *model) {
    const golf_hole_t *hole = golf_model_hole(model);
    model->vx = 0;
    model->vy = 0;
    model->x = (int32_t)hole->cup_x * GOLF_FIX;
    model->y = (int32_t)hole->cup_y * GOLF_FIX;
    model->phase = GOLF_PHASE_SUNK;
    model->sunk_this_hole = 1;
    model->scores[model->hole_index] = model->strokes;
    model->last_event = GOLF_EVENT_SUNK;
    return GOLF_EVENT_SUNK;
}

static golf_event_t golf_water_penalty(golf_model_t *model) {
    if (model->strokes < 255) model->strokes++;   // 落水罚杆
    model->x = model->shot_x;
    model->y = model->shot_y;
    model->vx = 0;
    model->vy = 0;
    model->phase = GOLF_PHASE_AIM;
    model->power_milli = 0;
    model->power_dir = 1;
    model->terrain = golf_model_terrain_at(model, golf_ball_world_x(model), golf_ball_world_y(model));
    model->last_event = GOLF_EVENT_WATER;
    return GOLF_EVENT_WATER;
}

golf_event_t golf_model_tick(golf_model_t *model, uint32_t dt_ms) {
    if (dt_ms == 0) return GOLF_EVENT_NONE;
    if (dt_ms > GOLF_MAX_TICK_MS) dt_ms = GOLF_MAX_TICK_MS;
    const int32_t dt = (int32_t)dt_ms;

    if (model->phase == GOLF_PHASE_POWER) {
        const int32_t step = (int32_t)(((int64_t)1000 * dt) / GOLF_SWING_SWEEP_MS);
        int32_t power = model->power_milli + (int32_t)model->power_dir * step;
        if (power >= 1000) {
            power = 1000 - (power - 1000);
            model->power_dir = -1;
        }
        if (power <= 0) {
            power = -power;
            model->power_dir = 1;
        }
        model->power_milli = (int16_t)golf_clamp32(power, 0, 1000);
        return GOLF_EVENT_NONE;
    }

    if (model->phase != GOLF_PHASE_ROLLING) return GOLF_EVENT_NONE;

    const uint8_t start_terrain = (uint8_t)golf_model_terrain_at(
        model, golf_ball_world_x(model), golf_ball_world_y(model));

    // 摩擦按整个 tick 折算一次,再分子步推进位置。
    int32_t decay = 1024 - (int32_t)(((int64_t)golf_friction_q10(start_terrain) * dt) / 1000);
    if (decay < 64) decay = 64;
    model->vx = (int32_t)(((int64_t)model->vx * decay) / 1024);
    model->vy = (int32_t)(((int64_t)model->vy * decay) / 1024);

    const int64_t span = ((int64_t)(golf_abs32(model->vx) + golf_abs32(model->vy)) * dt) / 1000;
    int steps = (int)(span / (GOLF_SUBSTEP_UNITS * GOLF_FIX)) + 1;
    if (steps > GOLF_MAX_SUBSTEPS) steps = GOLF_MAX_SUBSTEPS;

    bool bounced = false;
    for (int i = 0; i < steps; i++) {
        model->x += (int32_t)(((int64_t)model->vx * dt) / (1000 * steps));
        model->y += (int32_t)(((int64_t)model->vy * dt) / (1000 * steps));
        if (golf_resolve_collisions(model)) bounced = true;

        if (golf_model_terrain_at(model, golf_ball_world_x(model), golf_ball_world_y(model))
                == GOLF_TERRAIN_WATER) {
            return golf_water_penalty(model);
        }

        const int64_t speed_squared = golf_speed_squared(model);
        const int64_t capture = (int64_t)GOLF_CAPTURE_SPEED * GOLF_FIX;
        if (golf_inside_cup(model) && speed_squared <= capture * capture) {
            return golf_sink_ball(model);
        }
    }

    model->terrain = (uint8_t)golf_model_terrain_at(
        model, golf_ball_world_x(model), golf_ball_world_y(model));

    const int64_t stop = (int64_t)GOLF_STOP_SPEED * GOLF_FIX;
    if (golf_speed_squared(model) <= stop * stop) {
        model->vx = 0;
        model->vy = 0;
        if (golf_inside_cup(model)) return golf_sink_ball(model);
        model->phase = GOLF_PHASE_AIM;
        model->last_event = GOLF_EVENT_NONE;
        return GOLF_EVENT_NONE;
    }

    if (bounced) return GOLF_EVENT_BOUNCE;
    if (start_terrain != GOLF_TERRAIN_SAND && model->terrain == GOLF_TERRAIN_SAND) {
        return GOLF_EVENT_SAND;
    }
    return GOLF_EVENT_NONE;
}
