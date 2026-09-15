// main/pet_model.h —— 桌宠的纯逻辑状态机。
//
// 本文件不依赖 ESP-IDF、FreeRTOS 或 LVGL，可在主机上直接编译测试
// （tests/test_pet_model.c）。所有时间都以「秒」为单位传入，由调用方提供，
// 便于用固定步长复现行为。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PET_MODEL_VERSION 1U
#define PET_NAME_MAX 12U
#define PET_STATUS_MAX 48U

// 亲密度累计上限（bond_points 为 uint8）。
#define PET_BOND_MAX 255U

typedef enum {
    PET_MOOD_CONTENT = 0,  // 平静
    PET_MOOD_CURIOUS,      // 好奇
    PET_MOOD_HAPPY,        // 开心
    PET_MOOD_EXCITED,      // 兴奋
    PET_MOOD_HUNGRY,       // 饿了
    PET_MOOD_SLEEPY,       // 犯困
    PET_MOOD_LONELY,       // 寂寞
    PET_MOOD_SLEEPING,     // 睡着了
    PET_MOOD_COUNT,
} pet_mood_t;

typedef enum {
    PET_ACT_PET = 0,  // 撸一撸
    PET_ACT_FEED,     // 喂食
    PET_ACT_PLAY,     // 玩耍
    PET_ACT_COUNT,
} pet_action_t;

typedef enum {
    PET_OK = 0,
    PET_ERR_FULL,      // 不饿，吃不下
    PET_ERR_TIRED,     // 精力不足，玩不动
    PET_ERR_SLEEPING,  // 睡着了，先别打扰
    PET_ERR_COOLDOWN,  // 互动太频繁
    PET_ERR_INVALID,
} pet_result_t;

typedef enum {
    PET_BOND_STRANGER = 0,  // 陌生
    PET_BOND_FAMILIAR,      // 熟悉
    PET_BOND_CLOSE,         // 亲近
    PET_BOND_BEST,          // 挚友
} pet_bond_t;

// pet_model_tick 返回的事件位，供上层触发音效与动画。
enum {
    PET_EVENT_NONE = 0,
    PET_EVENT_WOKE = 1U << 0,
    PET_EVENT_FELL_ASLEEP = 1U << 1,
    PET_EVENT_BECAME_HUNGRY = 1U << 2,
    PET_EVENT_BOND_LEVEL_UP = 1U << 3,
};

typedef struct {
    uint32_t version;
    uint32_t rng;
    uint32_t age_seconds;   // 陪伴时长（累计）
    uint32_t clock_sod;     // 显示用时钟：当天秒数 0..86399
    uint16_t idle_seconds;  // 距上次互动的秒数
    uint16_t cheer_seconds; // 开心余韵，归零后回到平静
    uint16_t hunger_accum;  // 距下一点饱腹衰减的累计秒数
    uint16_t energy_accum;  // 距下一点精力衰减的累计秒数
    uint8_t hunger;         // 饱腹 0..100，100 = 很饱
    uint8_t energy;         // 精力 0..100
    uint8_t bond_points;    // 亲密度累计点数
    uint8_t care_count;     // 互动总次数
    uint8_t feed_count;
    uint8_t play_count;
    uint8_t pet_count;
    uint8_t asleep;         // 0/1
    uint8_t volume;         // 0..100
    uint8_t clock_set;      // 时钟是否已校准
    uint8_t cooldown;       // 「撸一撸」剩余冷却秒数
} pet_model_t;

// 时钟进位阈值与显示格式。
#define PET_SECONDS_PER_DAY 86400U

// 衰减与阈值（秒 / 百分比），集中在此便于测试引用。
#define PET_HUNGER_SECONDS_PER_POINT 90U
#define PET_ENERGY_SECONDS_PER_POINT 120U
#define PET_SLEEP_ENERGY_SECONDS_PER_POINT 20U
#define PET_LONELY_IDLE_SECONDS 600U
#define PET_HUNGRY_THRESHOLD 25U
#define PET_SLEEPY_THRESHOLD 30U
#define PET_SLEEP_ENTER_ENERGY 15U
#define PET_SLEEP_WAKE_ENERGY 90U
#define PET_FEED_HUNGER_GAIN 35U
#define PET_FEED_FULL_THRESHOLD 85U
#define PET_PLAY_ENERGY_COST 18U
#define PET_PLAY_HUNGER_COST 5U
#define PET_PLAY_MIN_ENERGY 25U
#define PET_PLAY_MIN_HUNGER 20U
#define PET_PET_COOLDOWN_SECONDS 5U
#define PET_CHEER_SECONDS 6U

// 亲密度点数到等级的边界。
#define PET_BOND_FAMILIAR_AT 20U
#define PET_BOND_CLOSE_AT 60U
#define PET_BOND_BEST_AT 120U

// 用给定种子创建一只全新状态的桌宠。
void pet_model_init(pet_model_t *model, uint32_t seed);

// 把读回的状态收敛到合法范围；数据不可用时返回 false。
bool pet_model_sanitize(pet_model_t *model);

// 推进 seconds 秒，返回 PET_EVENT_* 位掩码。
uint32_t pet_model_tick(pet_model_t *model, uint32_t seconds);

// 执行一次互动。成功时更新状态并返回 PET_OK。
// events 可为 NULL；不为 NULL 时写入本次互动产生的 PET_EVENT_* 位掩码
// （目前只有 PET_EVENT_BOND_LEVEL_UP），供上层播放提示音与动画。
pet_result_t pet_model_interact(pet_model_t *model, pet_action_t action,
                                uint32_t *events);

// 由当前状态推导心情（不修改状态）。
pet_mood_t pet_model_mood(const pet_model_t *model);

// 当前亲密度等级与到下一等级的进度（0..100，最高级恒为 100）。
pet_bond_t pet_model_bond(const pet_model_t *model);
uint8_t pet_model_bond_progress(const pet_model_t *model);

const char *pet_mood_name(pet_mood_t mood);
const char *pet_bond_name(pet_bond_t bond);

// 亲密度升级的一句话提示，例如「更亲近了：挚友」。
void pet_model_bond_notice(const pet_model_t *model, char *out, size_t size);
const char *pet_result_name(pet_result_t result);
const char *pet_action_name(pet_action_t action);

// 设置当天秒数（0..86399），并标记时钟已校准。
void pet_model_set_clock(pet_model_t *model, uint32_t seconds_of_day);
// 以分钟为单位微调时钟，delta 可正可负，内部按天回绕。
void pet_model_adjust_clock(pet_model_t *model, int32_t delta_minutes);

// 格式化为 "HH:MM"；未校准时写 "--:--" 并返回 false。
bool pet_model_format_clock(const pet_model_t *model, char *out, size_t size);

// 把陪伴时长格式化为「N天N小时」/「N小时N分」/「N分钟」；out 始终以 0 结尾。
void pet_model_format_age(uint32_t seconds, char *out, size_t size);

// 一行状态文案，例如「有点饿了」「陪你 3 天」。
void pet_model_status_text(const pet_model_t *model, char *out, size_t size);
