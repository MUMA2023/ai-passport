// main/golf_model.h —— 迷你高尔夫纯逻辑模型。
//
// 本文件不依赖 ESP-IDF 与 LVGL:全部状态、碰撞与像素判定都用整数定点运算,
// 因此可以在主机上直接编译测试。ESP32-C3 没有硬件 FPU,浮点会走软件模拟,
// 所以位置、速度、方向一律以 1/GOLF_FIX 世界单位表示。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 定点比例:1 个世界单位 = GOLF_FIX 个定点子单位。
#define GOLF_FIX 1024

// 球场尺寸(世界单位)。渲染时 1 世界单位 = 1 画布像素。
#define GOLF_WORLD_W 224
#define GOLF_WORLD_H 152

#define GOLF_HOLE_COUNT 3

// 球半径(世界单位)。球心被限制在 [R, W-R] x [R, H-R] 内。
#define GOLF_BALL_RADIUS 3
// 洞口半径:球心进入这个范围且速度足够慢才算进洞。
#define GOLF_CUP_RADIUS 6
// 高于这个速度经过洞口会"溜边"弹出,不进洞。
#define GOLF_CAPTURE_SPEED 95
// 低于这个速度视为停球。
#define GOLF_STOP_SPEED 7
// 力度 0..1000 对应的出球速度(世界单位/秒)。
#define GOLF_MIN_SPEED 55
#define GOLF_MAX_SPEED 395
// 瞄准辅助线最长长度(世界单位)。
#define GOLF_AIM_LENGTH 30
// 球杆摆动的单程时间(毫秒),即 0% -> 100% 所需时间。
#define GOLF_SWING_SWEEP_MS 600

// 地形。摩擦系数以"每秒衰减率"的 1024 倍存储,数值越大球停得越快。
typedef enum {
    GOLF_TERRAIN_ROUGH = 0,
    GOLF_TERRAIN_FAIRWAY,
    GOLF_TERRAIN_GREEN,
    GOLF_TERRAIN_SAND,
    GOLF_TERRAIN_WATER,
} golf_terrain_t;

// 地形区域。radius > 0 时为圆角矩形,渲染与判定共用同一套点测试,
// 保证"看起来在果岭上"和"判定为果岭"永远一致。
typedef struct {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
    uint8_t radius;
    uint8_t terrain;
} golf_zone_t;

// 障碍墙(轴对齐矩形),球会以固定弹性系数反弹。
typedef struct {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
} golf_wall_t;

typedef struct {
    const char *name;
    uint8_t par;
    int16_t tee_x;
    int16_t tee_y;
    int16_t cup_x;
    int16_t cup_y;
    int16_t tee_angle;          // 开局时的初始瞄准角(度),避免为此实现 atan2
    const golf_zone_t *zones;   // 后出现的区域覆盖先出现的
    uint8_t zone_count;
    const golf_wall_t *walls;
    uint8_t wall_count;
} golf_hole_t;

typedef enum {
    GOLF_PHASE_AIM = 0,   // 上/下调整角度,确定进入力度
    GOLF_PHASE_POWER,     // 力度条自动来回摆动,确定击球
    GOLF_PHASE_ROLLING,   // 球在滚动
    GOLF_PHASE_SUNK,      // 本洞已进洞,等待进入下一洞
    GOLF_PHASE_ROUND_DONE,// 整轮结束
} golf_phase_t;

typedef enum {
    GOLF_EVENT_NONE = 0,
    GOLF_EVENT_AIM,
    GOLF_EVENT_POWER_START,
    GOLF_EVENT_POWER_CANCEL,
    GOLF_EVENT_STRIKE,
    GOLF_EVENT_BOUNCE,
    GOLF_EVENT_SAND,
    GOLF_EVENT_WATER,
    GOLF_EVENT_SUNK,
    GOLF_EVENT_ROUND_DONE,
} golf_event_t;

// 场景像素语义。页面把每种语义映射到一个调色板索引,
// 模型本身不认识颜色,便于主机测试逐像素校验。
typedef enum {
    GOLF_PX_NONE = 0,
    GOLF_PX_ROUGH,
    GOLF_PX_FAIRWAY,
    GOLF_PX_GREEN,
    GOLF_PX_SAND,
    GOLF_PX_WATER,
    GOLF_PX_WALL,
    GOLF_PX_WALL_SHADOW,
    GOLF_PX_CUP,
    GOLF_PX_POLE,
    GOLF_PX_FLAG,
    GOLF_PX_BALL,
    GOLF_PX_BALL_EDGE,
    GOLF_PX_BALL_SHADOW,
    GOLF_PX_AIM,
} golf_px_t;

typedef struct {
    uint8_t hole_index;
    uint8_t strokes;                       // 本洞已用杆数
    uint8_t scores[GOLF_HOLE_COUNT];       // 已完成各洞杆数,0 = 未完成
    golf_phase_t phase;
    int16_t angle_deg;                     // 0 = 右,90 = 上,逆时针增大
    int16_t power_milli;                   // 0..1000
    int8_t power_dir;                      // 摆动方向 +1 / -1
    int32_t x, y;                          // 球心,定点
    int32_t vx, vy;                        // 速度,定点/秒
    int32_t shot_x, shot_y;                // 本杆起点,落水后回到这里
    uint8_t terrain;                       // 球当前位置的地形
    uint8_t sunk_this_hole;
    golf_event_t last_event;
} golf_model_t;

// 初始化到第一洞的瞄准状态。
void golf_model_init(golf_model_t *m);
// 重置整轮成绩并回到第一洞。
void golf_model_restart(golf_model_t *m);
// 载入指定洞:球回到发球台,角度对准洞口,杆数清零。
void golf_model_start_hole(golf_model_t *m, uint8_t index);

// 上/下键:调整瞄准角度(自动取模到 0..359)。非瞄准阶段返回 NONE。
golf_event_t golf_model_aim(golf_model_t *m, int delta_deg);
// 确定键(瞄准阶段):进入力度摆动。
golf_event_t golf_model_power_begin(golf_model_t *m);
// 上键(力度阶段):放弃本杆,退回瞄准。
golf_event_t golf_model_power_cancel(golf_model_t *m);
// 确定键(力度阶段):按当前力度击球。
golf_event_t golf_model_strike(golf_model_t *m);
// 推进 dt_ms 毫秒:力度摆动或球体物理。
golf_event_t golf_model_tick(golf_model_t *m, uint32_t dt_ms);
// 进洞后推进:下一洞或整轮结束。
golf_event_t golf_model_advance(golf_model_t *m);

const golf_hole_t *golf_model_hole(const golf_model_t *m);
// 世界坐标(整数)处的地形。
uint8_t golf_model_terrain_at(const golf_model_t *m, int wx, int wy);
// 静态场景像素(地形/墙/洞/旗)。
golf_px_t golf_model_scene_pixel(const golf_model_t *m, int px, int py);
// 动态像素(球/影子/瞄准线),画在场景之上。
golf_px_t golf_model_overlay_pixel(const golf_model_t *m, int px, int py);

int golf_model_power_percent(const golf_model_t *m);
int golf_model_total_strokes(const golf_model_t *m);
int golf_model_total_par(void);
int golf_model_ball_pixel_x(const golf_model_t *m);
int golf_model_ball_pixel_y(const golf_model_t *m);
// 瞄准辅助线的末端像素(球心沿瞄准方向前进 GOLF_AIM_LENGTH)。页面用它计算重绘脏区。
int golf_model_aim_tip_pixel_x(const golf_model_t *m);
int golf_model_aim_tip_pixel_y(const golf_model_t *m);
