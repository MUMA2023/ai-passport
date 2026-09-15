// tests/test_pet_model.c —— 「家机桌宠」状态机的主机侧测试。
//
// 状态机刻意不依赖 ESP-IDF / LVGL,所以能直接用宿主编译器跑:
//   cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_pet_model.c main/pet_model.c
// 时间都以秒为单位由调用方传入,因此这里可以用固定步长精确复现行为。
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "pet_model.h"

static pet_model_t make_pet(void)
{
    pet_model_t model;
    pet_model_init(&model, 0x12345678U);
    return model;
}

static void test_new_pet(void)
{
    pet_model_t m = make_pet();
    assert(m.version == PET_MODEL_VERSION);
    assert(m.hunger == 70);
    assert(m.energy == 80);
    assert(m.volume == 60);
    assert(m.asleep == 0);
    assert(m.clock_set == 0);
    assert(m.bond_points == 0);
    assert(m.care_count == 0);
    assert(m.age_seconds == 0);
    assert(pet_model_bond(&m) == PET_BOND_STRANGER);
    assert(pet_model_bond_progress(&m) == 0);

    // 刚领养时既不饿也不困,心情只可能是平静或好奇。
    const pet_mood_t mood = pet_model_mood(&m);
    assert(mood == PET_MOOD_CONTENT || mood == PET_MOOD_CURIOUS);
}

static void test_clock(void)
{
    pet_model_t m = make_pet();
    char buf[16];

    // 未校准时显示占位符,且明确返回 false。
    assert(pet_model_format_clock(&m, buf, sizeof(buf)) == false);
    assert(strcmp(buf, "--:--") == 0);

    pet_model_set_clock(&m, 3661U);
    assert(m.clock_set == 1);
    assert(pet_model_format_clock(&m, buf, sizeof(buf)) == true);
    assert(strcmp(buf, "01:01") == 0);

    pet_model_adjust_clock(&m, -2);
    (void)pet_model_format_clock(&m, buf, sizeof(buf));
    assert(strcmp(buf, "00:59") == 0);

    // 向前跨过零点。
    pet_model_set_clock(&m, 0U);
    pet_model_adjust_clock(&m, -1);
    (void)pet_model_format_clock(&m, buf, sizeof(buf));
    assert(strcmp(buf, "23:59") == 0);

    // 向后跨过零点。
    pet_model_set_clock(&m, 86340U);
    pet_model_adjust_clock(&m, 2);
    (void)pet_model_format_clock(&m, buf, sizeof(buf));
    assert(strcmp(buf, "00:01") == 0);

    // 越界输入按一天取模,不会写坏状态。
    pet_model_set_clock(&m, PET_SECONDS_PER_DAY + 60U);
    (void)pet_model_format_clock(&m, buf, sizeof(buf));
    assert(strcmp(buf, "00:01") == 0);
}

static void test_age_format(void)
{
    char age[24];

    pet_model_format_age(0U, age, sizeof(age));
    assert(strcmp(age, "0分钟") == 0);

    pet_model_format_age(3600U, age, sizeof(age));
    assert(strcmp(age, "1小时0分") == 0);

    pet_model_format_age(90000U, age, sizeof(age));
    assert(strcmp(age, "1天1小时") == 0);

    // 缓冲区很小时也必须以 0 结尾。
    char tiny[3];
    pet_model_format_age(90000U, tiny, sizeof(tiny));
    assert(strlen(tiny) < sizeof(tiny));
}

static void test_tick_decay(void)
{
    pet_model_t m = make_pet();

    // 90 秒掉一点饱腹,但还不够掉一点精力。
    uint32_t events = pet_model_tick(&m, PET_HUNGER_SECONDS_PER_POINT);
    assert(events == PET_EVENT_NONE);
    assert(m.hunger == 69);
    assert(m.energy == 80);
    assert(m.age_seconds == 90U);
    assert(m.idle_seconds == 90U);

    // 累加器跨调用保留:再 30 秒刚好凑满 120 秒,精力掉一点。
    events = pet_model_tick(&m, 30U);
    assert(events == PET_EVENT_NONE);
    assert(m.hunger == 69);
    assert(m.energy == 79);
    assert(m.age_seconds == 120U);

    // 非法输入不改变状态。
    assert(pet_model_tick(&m, 0U) == PET_EVENT_NONE);
    assert(pet_model_tick(NULL, 10U) == PET_EVENT_NONE);
    assert(m.age_seconds == 120U);
}

static void test_becomes_hungry(void)
{
    pet_model_t m = make_pet();
    m.hunger = PET_HUNGRY_THRESHOLD + 1U;
    m.hunger_accum = 0;

    const uint32_t events = pet_model_tick(&m, PET_HUNGER_SECONDS_PER_POINT);
    assert(m.hunger == PET_HUNGRY_THRESHOLD);
    assert((events & PET_EVENT_BECAME_HUNGRY) != 0);
    assert(pet_model_mood(&m) == PET_MOOD_HUNGRY);

    // 已经饿了之后不再重复触发事件。
    const uint32_t again = pet_model_tick(&m, PET_HUNGER_SECONDS_PER_POINT);
    assert((again & PET_EVENT_BECAME_HUNGRY) == 0);
}

static void test_sleep_cycle(void)
{
    pet_model_t m = make_pet();
    m.energy = PET_SLEEP_ENTER_ENERGY + 1U;
    m.energy_accum = 0;
    m.hunger_accum = 0;

    uint32_t events = pet_model_tick(&m, PET_ENERGY_SECONDS_PER_POINT);
    assert((events & PET_EVENT_FELL_ASLEEP) != 0);
    assert(m.asleep == 1);
    assert(m.energy == PET_SLEEP_ENTER_ENERGY);
    assert(pet_model_mood(&m) == PET_MOOD_SLEEPING);

    // 睡着时不再消耗饱腹,精力按更快的速度回充。
    const uint8_t hunger_before = m.hunger;
    events = pet_model_tick(&m, 20U);
    assert(events == PET_EVENT_NONE);
    assert(m.hunger == hunger_before);
    assert(m.energy == PET_SLEEP_ENTER_ENERGY + 1U);
    assert(m.asleep == 1);

    // 回满到唤醒阈值后自动醒来。
    const uint32_t needed =
        (uint32_t)(PET_SLEEP_WAKE_ENERGY - m.energy) * PET_SLEEP_ENERGY_SECONDS_PER_POINT;
    events = pet_model_tick(&m, needed);
    assert((events & PET_EVENT_WOKE) != 0);
    assert(m.asleep == 0);
    assert(m.energy == PET_SLEEP_WAKE_ENERGY);
    assert(m.hunger == hunger_before);
}

static void test_pet_and_cooldown(void)
{
    pet_model_t m = make_pet();
    uint32_t events = 0xFFFFFFFFU;

    assert(pet_model_interact(&m, PET_ACT_PET, &events) == PET_OK);
    assert(events == PET_EVENT_NONE);
    assert(m.bond_points == 3);
    assert(m.pet_count == 1);
    assert(m.care_count == 1);
    assert(m.cheer_seconds == PET_CHEER_SECONDS);
    assert(m.idle_seconds == 0);
    assert(pet_model_mood(&m) == PET_MOOD_EXCITED);

    // 冷却期内重复撸不生效,也不扣亲密度。
    assert(pet_model_interact(&m, PET_ACT_PET, &events) == PET_ERR_COOLDOWN);
    assert(m.bond_points == 3);

    (void)pet_model_tick(&m, PET_PET_COOLDOWN_SECONDS);
    assert(m.cooldown == 0);
    assert(pet_model_interact(&m, PET_ACT_PET, &events) == PET_OK);
    assert(m.bond_points == 6);
    assert(m.pet_count == 2);
}

static void test_feed(void)
{
    pet_model_t m = make_pet();
    uint32_t events = 0;

    assert(pet_model_interact(&m, PET_ACT_FEED, &events) == PET_OK);
    assert(m.hunger == 100);  // 70 + 35 截到上限
    assert(m.bond_points == 2);
    assert(m.feed_count == 1);

    // 已经很饱时喂不进去。
    assert(pet_model_interact(&m, PET_ACT_FEED, &events) == PET_ERR_FULL);
    assert(m.feed_count == 1);

    // 刚好在阈值之下还能再吃一口。
    m.hunger = PET_FEED_FULL_THRESHOLD - 1U;
    assert(pet_model_interact(&m, PET_ACT_FEED, &events) == PET_OK);
    assert(m.hunger == 100);
    assert(m.feed_count == 2);
}

static void test_play(void)
{
    pet_model_t m = make_pet();
    uint32_t events = 0;

    assert(pet_model_interact(&m, PET_ACT_PLAY, &events) == PET_OK);
    assert(m.energy == 80 - PET_PLAY_ENERGY_COST);
    assert(m.hunger == 70 - PET_PLAY_HUNGER_COST);
    assert(m.bond_points == 5);
    assert(m.play_count == 1);

    // 精力不足或太饿都玩不动。
    m.energy = PET_PLAY_MIN_ENERGY - 1U;
    assert(pet_model_interact(&m, PET_ACT_PLAY, &events) == PET_ERR_TIRED);

    m.energy = 80;
    m.hunger = PET_PLAY_MIN_HUNGER - 1U;
    assert(pet_model_interact(&m, PET_ACT_PLAY, &events) == PET_ERR_TIRED);

    m.hunger = 70;
    m.energy = PET_PLAY_MIN_ENERGY;
    assert(pet_model_interact(&m, PET_ACT_PLAY, &events) == PET_OK);
}

static void test_sleeping_blocks_interaction(void)
{
    pet_model_t m = make_pet();
    m.asleep = 1;
    uint32_t events = 0xFFFFFFFFU;

    assert(pet_model_interact(&m, PET_ACT_PET, &events) == PET_ERR_SLEEPING);
    assert(pet_model_interact(&m, PET_ACT_FEED, &events) == PET_ERR_SLEEPING);
    assert(pet_model_interact(&m, PET_ACT_PLAY, &events) == PET_ERR_SLEEPING);
    assert(events == PET_EVENT_NONE);
    assert(m.bond_points == 0);
    assert(m.care_count == 0);
}

static void test_invalid_interaction(void)
{
    pet_model_t m = make_pet();

    assert(pet_model_interact(NULL, PET_ACT_PET, NULL) == PET_ERR_INVALID);
    assert(pet_model_interact(&m, PET_ACT_COUNT, NULL) == PET_ERR_INVALID);
    assert(m.bond_points == 0);

    // events 允许传 NULL。
    assert(pet_model_interact(&m, PET_ACT_PET, NULL) == PET_OK);
    assert(m.bond_points == 3);
}

static void test_bond_levels(void)
{
    pet_model_t m = make_pet();
    m.bond_points = PET_BOND_FAMILIAR_AT - 1U;
    assert(pet_model_bond(&m) == PET_BOND_STRANGER);

    // 喂食 +2,刚好跨过「熟悉」的门槛,并产生升级事件。
    uint32_t events = 0;
    assert(pet_model_interact(&m, PET_ACT_FEED, &events) == PET_OK);
    assert(pet_model_bond(&m) == PET_BOND_FAMILIAR);
    assert((events & PET_EVENT_BOND_LEVEL_UP) != 0);

    char notice[PET_STATUS_MAX];
    pet_model_bond_notice(&m, notice, sizeof(notice));
    assert(strcmp(notice, "更亲近了：熟悉") == 0);

    // 未跨级时不产生升级事件。
    m.hunger = 0;  // 上一口已经吃到上限,先腾出肚子
    events = 0xFFFFFFFFU;
    assert(pet_model_interact(&m, PET_ACT_FEED, &events) == PET_OK);
    assert(events == PET_EVENT_NONE);

    m.bond_points = PET_BOND_CLOSE_AT;
    assert(pet_model_bond(&m) == PET_BOND_CLOSE);
    m.bond_points = PET_BOND_BEST_AT;
    assert(pet_model_bond(&m) == PET_BOND_BEST);

    // 亲密度点数封顶,不会溢出回绕。
    m.bond_points = PET_BOND_MAX;
    m.hunger = 0;
    assert(pet_model_interact(&m, PET_ACT_FEED, &events) == PET_OK);
    assert(m.bond_points == PET_BOND_MAX);
    assert(pet_model_bond(&m) == PET_BOND_BEST);
}

static void test_bond_progress(void)
{
    pet_model_t m = make_pet();

    m.bond_points = 0;
    assert(pet_model_bond_progress(&m) == 0);
    m.bond_points = 10;
    assert(pet_model_bond_progress(&m) == 50);
    m.bond_points = PET_BOND_FAMILIAR_AT - 1U;
    assert(pet_model_bond_progress(&m) == 95);
    m.bond_points = PET_BOND_FAMILIAR_AT;
    assert(pet_model_bond_progress(&m) == 0);
    m.bond_points = 40;
    assert(pet_model_bond_progress(&m) == 50);
    m.bond_points = PET_BOND_CLOSE_AT;
    assert(pet_model_bond_progress(&m) == 0);
    m.bond_points = 90;
    assert(pet_model_bond_progress(&m) == 50);
    m.bond_points = PET_BOND_BEST_AT;
    assert(pet_model_bond_progress(&m) == 100);
    m.bond_points = PET_BOND_MAX;
    assert(pet_model_bond_progress(&m) == 100);
}

static void test_moods(void)
{
    pet_model_t m = make_pet();
    m.idle_seconds = PET_LONELY_IDLE_SECONDS;
    assert(pet_model_mood(&m) == PET_MOOD_LONELY);

    m = make_pet();
    m.energy = PET_SLEEPY_THRESHOLD;
    assert(pet_model_mood(&m) == PET_MOOD_SLEEPY);

    // 饿的优先级高于困。
    m.energy = PET_SLEEPY_THRESHOLD;
    m.hunger = PET_HUNGRY_THRESHOLD;
    assert(pet_model_mood(&m) == PET_MOOD_HUNGRY);

    // 睡着的优先级最高。
    m.asleep = 1;
    assert(pet_model_mood(&m) == PET_MOOD_SLEEPING);

    // 短暂开心之后回到常态。
    m = make_pet();
    m.cheer_seconds = PET_CHEER_SECONDS;
    assert(pet_model_mood(&m) == PET_MOOD_EXCITED);
    m.cheer_seconds = PET_CHEER_SECONDS / 2U;
    assert(pet_model_mood(&m) == PET_MOOD_HAPPY);
    m.cheer_seconds = 0;
    const pet_mood_t mood = pet_model_mood(&m);
    assert(mood == PET_MOOD_CONTENT || mood == PET_MOOD_CURIOUS);
}

static void test_sanitize(void)
{
    pet_model_t m = make_pet();
    m.version = PET_MODEL_VERSION + 1U;
    assert(pet_model_sanitize(&m) == false);

    m = make_pet();
    m.hunger = 200;
    m.energy = 255;
    m.volume = 255;
    m.asleep = 7;
    m.clock_set = 9;
    m.clock_sod = PET_SECONDS_PER_DAY + 5U;
    m.hunger_accum = PET_HUNGER_SECONDS_PER_POINT;
    m.energy_accum = PET_ENERGY_SECONDS_PER_POINT;
    m.cheer_seconds = PET_CHEER_SECONDS + 1U;
    m.cooldown = PET_PET_COOLDOWN_SECONDS + 1U;
    m.rng = 0;

    assert(pet_model_sanitize(&m) == true);
    assert(m.hunger == 100);
    assert(m.energy == 100);
    assert(m.volume == 100);
    assert(m.asleep == 1);
    assert(m.clock_set == 1);
    assert(m.clock_sod == 5U);
    assert(m.hunger_accum == 0);
    assert(m.energy_accum == 0);
    assert(m.cheer_seconds == 0);
    assert(m.cooldown == 0);
    assert(m.rng != 0);

    assert(pet_model_sanitize(NULL) == false);
}

static void test_names_and_status(void)
{
    assert(strcmp(pet_mood_name(PET_MOOD_HUNGRY), "饿了") == 0);
    assert(strcmp(pet_bond_name(PET_BOND_BEST), "挚友") == 0);
    assert(strcmp(pet_result_name(PET_ERR_FULL), "吃不下啦") == 0);
    assert(strcmp(pet_action_name(PET_ACT_PLAY), "玩耍") == 0);

    // 越界输入返回占位符,不会读到数组之外。
    assert(strcmp(pet_mood_name(PET_MOOD_COUNT), "?") == 0);
    assert(strcmp(pet_bond_name((pet_bond_t)(PET_BOND_BEST + 1)), "?") == 0);
    assert(strcmp(pet_result_name((pet_result_t)(PET_ERR_INVALID + 1)), "?") == 0);
    assert(strcmp(pet_action_name(PET_ACT_COUNT), "?") == 0);

    pet_model_t m = make_pet();
    char text[PET_STATUS_MAX];
    pet_model_status_text(&m, text, sizeof(text));
    assert(text[0] != '\0');
    assert(strlen(text) < sizeof(text));

    m.asleep = 1;
    pet_model_status_text(&m, text, sizeof(text));
    assert(strcmp(text, "嘘…它在打盹") == 0);

    // 缓冲区很小时也必须以 0 结尾。
    char tiny[4];
    pet_model_status_text(&m, tiny, sizeof(tiny));
    assert(strlen(tiny) < sizeof(tiny));
}

int main(void)
{
    test_new_pet();
    test_clock();
    test_age_format();
    test_tick_decay();
    test_becomes_hungry();
    test_sleep_cycle();
    test_pet_and_cooldown();
    test_feed();
    test_play();
    test_sleeping_blocks_interaction();
    test_invalid_interaction();
    test_bond_levels();
    test_bond_progress();
    test_moods();
    test_sanitize();
    test_names_and_status();
    return 0;
}
