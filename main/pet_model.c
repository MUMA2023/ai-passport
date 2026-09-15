// main/pet_model.c —— 桌宠状态机实现。只做纯计算，不碰硬件。
#include "pet_model.h"

#include <stdio.h>
#include <string.h>

static const char *const PET_MOOD_NAMES[PET_MOOD_COUNT] = {
    "平静", "好奇", "开心", "兴奋", "饿了", "犯困", "寂寞", "睡着了",
};

static const char *const PET_BOND_NAMES[] = {
    "陌生", "熟悉", "亲近", "挚友",
};

static const char *const PET_RESULT_NAMES[] = {
    "好的", "吃不下啦", "玩不动了", "它在睡觉", "等等再试", "不行",
};

static const char *const PET_ACTION_NAMES[] = {
    "撸一撸", "喂食", "玩耍",
};

static uint8_t pet_clamp_percent(int value)
{
    if (value < 0) return 0;
    if (value > 100) return 100;
    return (uint8_t)value;
}

static uint32_t pet_rng_next(pet_model_t *model)
{
    uint32_t x = model->rng ? model->rng : 0x2545F491U;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    model->rng = x ? x : 0x2545F491U;
    return model->rng;
}

void pet_model_init(pet_model_t *model, uint32_t seed)
{
    if (!model) return;
    memset(model, 0, sizeof(*model));
    model->version = PET_MODEL_VERSION;
    model->rng = seed ? seed : 0x2545F491U;
    model->hunger = 70;
    model->energy = 80;
    model->volume = 60;
    model->clock_sod = 9U * 3600U;  // 未校准时先摆一个 09:00，界面显示 --:--
    model->clock_set = 0;
    (void)pet_rng_next(model);
}

bool pet_model_sanitize(pet_model_t *model)
{
    if (!model) return false;
    if (model->version != PET_MODEL_VERSION) return false;
    if (model->rng == 0) model->rng = 0x2545F491U;

    model->hunger = pet_clamp_percent(model->hunger);
    model->energy = pet_clamp_percent(model->energy);
    model->volume = pet_clamp_percent(model->volume);
    model->asleep = model->asleep ? 1U : 0U;
    model->clock_set = model->clock_set ? 1U : 0U;
    model->clock_sod %= PET_SECONDS_PER_DAY;

    // 累加器只需保留「未满一点」的余数，超范围一律归零，避免坏数据放大衰减。
    if (model->hunger_accum >= PET_HUNGER_SECONDS_PER_POINT) model->hunger_accum = 0;
    if (model->energy_accum >= PET_ENERGY_SECONDS_PER_POINT) model->energy_accum = 0;
    if (model->cheer_seconds > PET_CHEER_SECONDS) model->cheer_seconds = 0;
    if (model->cooldown > PET_PET_COOLDOWN_SECONDS) model->cooldown = 0;
    return true;
}

uint32_t pet_model_tick(pet_model_t *model, uint32_t seconds)
{
    if (!model || seconds == 0) return PET_EVENT_NONE;

    uint32_t events = PET_EVENT_NONE;
    const bool was_asleep = model->asleep != 0;
    const bool was_hungry = model->hunger <= PET_HUNGRY_THRESHOLD;

    model->clock_sod = (model->clock_sod + seconds) % PET_SECONDS_PER_DAY;
    model->age_seconds += seconds;

    const uint32_t idle = (uint32_t)model->idle_seconds + seconds;
    model->idle_seconds = idle > 0xFFFFU ? 0xFFFFU : (uint16_t)idle;

    model->cheer_seconds = model->cheer_seconds > seconds
                               ? (uint16_t)(model->cheer_seconds - seconds)
                               : 0;
    model->cooldown = model->cooldown > seconds
                          ? (uint8_t)(model->cooldown - seconds)
                          : 0;

    if (was_asleep) {
        uint32_t accum = (uint32_t)model->energy_accum + seconds;
        const uint32_t gain = accum / PET_SLEEP_ENERGY_SECONDS_PER_POINT;
        accum %= PET_SLEEP_ENERGY_SECONDS_PER_POINT;
        model->energy = (uint8_t)((gain >= 100U - model->energy)
                                      ? 100U
                                      : (uint32_t)model->energy + gain);
        model->energy_accum = (uint16_t)accum;
        if (model->energy >= PET_SLEEP_WAKE_ENERGY) {
            model->asleep = 0;
            model->energy_accum = 0;
            events |= PET_EVENT_WOKE;
        }
    } else {
        uint32_t accum = (uint32_t)model->hunger_accum + seconds;
        const uint32_t loss = accum / PET_HUNGER_SECONDS_PER_POINT;
        accum %= PET_HUNGER_SECONDS_PER_POINT;
        model->hunger = (uint8_t)(loss >= model->hunger ? 0U : model->hunger - loss);
        model->hunger_accum = (uint16_t)accum;

        accum = (uint32_t)model->energy_accum + seconds;
        const uint32_t drain = accum / PET_ENERGY_SECONDS_PER_POINT;
        accum %= PET_ENERGY_SECONDS_PER_POINT;
        model->energy = (uint8_t)(drain >= model->energy ? 0U : model->energy - drain);
        model->energy_accum = (uint16_t)accum;

        if (model->energy <= PET_SLEEP_ENTER_ENERGY) {
            model->asleep = 1;
            model->energy_accum = 0;
            events |= PET_EVENT_FELL_ASLEEP;
        }
    }

    if (!was_hungry && model->hunger <= PET_HUNGRY_THRESHOLD) {
        events |= PET_EVENT_BECAME_HUNGRY;
    }
    // 亲密度只在 interact 里变化，因此升级事件由那里产生，不在这里判断。
    return events;
}

static void pet_award_bond(pet_model_t *model, uint8_t points)
{
    const uint32_t total = (uint32_t)model->bond_points + points;
    model->bond_points = (uint8_t)(total > PET_BOND_MAX ? PET_BOND_MAX : total);
    if (model->care_count < 0xFFU) model->care_count++;
    model->idle_seconds = 0;
    model->cheer_seconds = PET_CHEER_SECONDS;
}

pet_result_t pet_model_interact(pet_model_t *model, pet_action_t action, uint32_t *events)
{
    if (events) *events = PET_EVENT_NONE;
    if (!model || action >= PET_ACT_COUNT) return PET_ERR_INVALID;
    if (model->asleep) return PET_ERR_SLEEPING;

    const pet_bond_t bond_before = pet_model_bond(model);

    switch (action) {
    case PET_ACT_PET:
        if (model->cooldown > 0) return PET_ERR_COOLDOWN;
        pet_award_bond(model, 3);
        model->cooldown = PET_PET_COOLDOWN_SECONDS;
        if (model->pet_count < 0xFFU) model->pet_count++;
        break;

    case PET_ACT_FEED:
        if (model->hunger >= PET_FEED_FULL_THRESHOLD) return PET_ERR_FULL;
        model->hunger = pet_clamp_percent(model->hunger + PET_FEED_HUNGER_GAIN);
        pet_award_bond(model, 2);
        if (model->feed_count < 0xFFU) model->feed_count++;
        break;

    case PET_ACT_PLAY:
        if (model->energy < PET_PLAY_MIN_ENERGY || model->hunger < PET_PLAY_MIN_HUNGER) {
            return PET_ERR_TIRED;
        }
        model->energy = pet_clamp_percent(model->energy - PET_PLAY_ENERGY_COST);
        model->hunger = pet_clamp_percent(model->hunger - PET_PLAY_HUNGER_COST);
        pet_award_bond(model, 5);
        if (model->play_count < 0xFFU) model->play_count++;
        break;

    default:
        return PET_ERR_INVALID;
    }

    if (events && pet_model_bond(model) != bond_before) {
        *events |= PET_EVENT_BOND_LEVEL_UP;
    }
    return PET_OK;
}

pet_mood_t pet_model_mood(const pet_model_t *model)
{
    if (!model) return PET_MOOD_CONTENT;
    if (model->asleep) return PET_MOOD_SLEEPING;
    if (model->hunger <= PET_HUNGRY_THRESHOLD) return PET_MOOD_HUNGRY;
    if (model->energy <= PET_SLEEPY_THRESHOLD) return PET_MOOD_SLEEPY;
    if (model->cheer_seconds > 0) {
        return model->cheer_seconds > PET_CHEER_SECONDS / 2U ? PET_MOOD_EXCITED
                                                            : PET_MOOD_HAPPY;
    }
    if (model->idle_seconds >= PET_LONELY_IDLE_SECONDS) return PET_MOOD_LONELY;
    // 剩余情况用固定的奇偶交替代替随机，行为可复现、便于测试。
    return (model->rng & 1U) ? PET_MOOD_CURIOUS : PET_MOOD_CONTENT;
}

pet_bond_t pet_model_bond(const pet_model_t *model)
{
    if (!model) return PET_BOND_STRANGER;
    if (model->bond_points >= PET_BOND_BEST_AT) return PET_BOND_BEST;
    if (model->bond_points >= PET_BOND_CLOSE_AT) return PET_BOND_CLOSE;
    if (model->bond_points >= PET_BOND_FAMILIAR_AT) return PET_BOND_FAMILIAR;
    return PET_BOND_STRANGER;
}

uint8_t pet_model_bond_progress(const pet_model_t *model)
{
    if (!model) return 0;
    const uint32_t points = model->bond_points;
    if (points >= PET_BOND_BEST_AT) return 100;
    // 进度条从「当前等级的起点」走到「下一等级的阈值」，因此分母必须是下一级阈值本身，
    // 否则进度条会在换级的那一刻跳变。
    const uint32_t floor_points =
        points >= PET_BOND_CLOSE_AT ? PET_BOND_CLOSE_AT
        : points >= PET_BOND_FAMILIAR_AT ? PET_BOND_FAMILIAR_AT
                                         : 0U;
    const uint32_t ceiling_points =
        points >= PET_BOND_CLOSE_AT ? PET_BOND_BEST_AT
        : points >= PET_BOND_FAMILIAR_AT ? PET_BOND_CLOSE_AT
                                         : PET_BOND_FAMILIAR_AT;
    return (uint8_t)((points - floor_points) * 100U / (ceiling_points - floor_points));
}

const char *pet_mood_name(pet_mood_t mood)
{
    if (mood < 0 || mood >= PET_MOOD_COUNT) return "?";
    return PET_MOOD_NAMES[mood];
}

const char *pet_bond_name(pet_bond_t bond)
{
    if (bond < 0 || bond > PET_BOND_BEST) return "?";
    return PET_BOND_NAMES[bond];
}

void pet_model_bond_notice(const pet_model_t *model, char *out, size_t size)
{
    if (!out || size == 0) return;
    snprintf(out, size, "更亲近了：%s", pet_bond_name(pet_model_bond(model)));
}

const char *pet_result_name(pet_result_t result)
{
    if (result < 0 || result > PET_ERR_INVALID) return "?";
    return PET_RESULT_NAMES[result];
}

const char *pet_action_name(pet_action_t action)
{
    if (action < 0 || action >= PET_ACT_COUNT) return "?";
    return PET_ACTION_NAMES[action];
}

void pet_model_set_clock(pet_model_t *model, uint32_t seconds_of_day)
{
    if (!model) return;
    model->clock_sod = seconds_of_day % PET_SECONDS_PER_DAY;
    model->clock_set = 1;
}

void pet_model_adjust_clock(pet_model_t *model, int32_t delta_minutes)
{
    if (!model) return;
    int64_t total = (int64_t)model->clock_sod + (int64_t)delta_minutes * 60;
    const int64_t day = PET_SECONDS_PER_DAY;
    total %= day;
    if (total < 0) total += day;
    model->clock_sod = (uint32_t)total;
    model->clock_set = 1;
}

bool pet_model_format_clock(const pet_model_t *model, char *out, size_t size)
{
    if (!out || size == 0) return false;
    if (!model || !model->clock_set) {
        snprintf(out, size, "--:--");
        return false;
    }
    snprintf(out, size, "%02u:%02u",
             (unsigned)(model->clock_sod / 3600U),
             (unsigned)((model->clock_sod % 3600U) / 60U));
    return true;
}

void pet_model_format_age(uint32_t seconds, char *out, size_t size)
{
    if (!out || size == 0) return;
    const uint32_t days = seconds / 86400U;
    const uint32_t hours = (seconds % 86400U) / 3600U;
    const uint32_t minutes = (seconds % 3600U) / 60U;
    if (days > 0) {
        snprintf(out, size, "%u天%u小时", (unsigned)days, (unsigned)hours);
    } else if (hours > 0) {
        snprintf(out, size, "%u小时%u分", (unsigned)hours, (unsigned)minutes);
    } else {
        snprintf(out, size, "%u分钟", (unsigned)minutes);
    }
}

void pet_model_status_text(const pet_model_t *model, char *out, size_t size)
{
    if (!out || size == 0) return;
    if (!model) {
        snprintf(out, size, " ");
        return;
    }
    char age[24];
    pet_model_format_age(model->age_seconds, age, sizeof(age));
    switch (pet_model_mood(model)) {
    case PET_MOOD_SLEEPING:
        snprintf(out, size, "嘘…它在打盹");
        break;
    case PET_MOOD_HUNGRY:
        snprintf(out, size, "肚子空空，想吃点东西");
        break;
    case PET_MOOD_SLEEPY:
        snprintf(out, size, "眼皮打架了");
        break;
    case PET_MOOD_LONELY:
        snprintf(out, size, "有点想你，陪你%s了", age);
        break;
    case PET_MOOD_EXCITED:
        snprintf(out, size, "开心得转圈圈");
        break;
    case PET_MOOD_HAPPY:
        snprintf(out, size, "心情不错");
        break;
    case PET_MOOD_CURIOUS:
        snprintf(out, size, "东张西望中");
        break;
    case PET_MOOD_CONTENT:
    default:
        snprintf(out, size, "安静地待在你身边");
        break;
    }
}
