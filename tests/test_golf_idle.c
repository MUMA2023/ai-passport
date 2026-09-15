// tests/test_golf_idle.c —— 闲置调光状态机的主机端测试。
//
// 这里钉住的是"只在状态翻转的那一帧上报"这条契约。两个方向写错都是真 bug:
// 每帧都上报会让背光被反复重设,从不恢复则会让屏幕再也亮不起来 —— 后者在真机上
// 表现为"玩着玩着屏幕黑了",而前者根本看不出来。编译方式见 tools/validate.sh。
#include <assert.h>
#include <stdio.h>

#include "golf_idle.h"

// 阈值之前一直保持正常亮度。
static void test_stays_bright_until_threshold(void) {
    golf_idle_state_t idle;
    golf_idle_init(&idle, 1000);

    assert(!golf_idle_tick(&idle, 1000));
    assert(!golf_idle_tick(&idle, 1000 + GOLF_IDLE_DIM_MS - 1));
    assert(!idle.dimmed);
}

// 恰好到阈值那一帧翻转,并且只上报一次。
static void test_dims_exactly_at_threshold_and_only_once(void) {
    golf_idle_state_t idle;
    golf_idle_init(&idle, 0);

    assert(!golf_idle_tick(&idle, GOLF_IDLE_DIM_MS - 1));
    assert(golf_idle_tick(&idle, GOLF_IDLE_DIM_MS));      // 翻转的那一帧
    assert(idle.dimmed);
    assert(!golf_idle_tick(&idle, GOLF_IDLE_DIM_MS + 20)); // 不重复上报
    assert(!golf_idle_tick(&idle, GOLF_IDLE_DIM_MS * 10));
    assert(idle.dimmed);
}

// 亮着的时候按键要把计时重置,而不是被当成"唤醒"。
static void test_activity_while_bright_only_resets_the_timer(void) {
    golf_idle_state_t idle;
    golf_idle_init(&idle, 0);

    assert(!golf_idle_note_activity(&idle, GOLF_IDLE_DIM_MS - 1));
    assert(!idle.dimmed);
    assert(!golf_idle_tick(&idle, GOLF_IDLE_DIM_MS));        // 从按键那刻重新计时
    assert(!golf_idle_tick(&idle, 2 * GOLF_IDLE_DIM_MS - 2));
    assert(golf_idle_tick(&idle, 2 * GOLF_IDLE_DIM_MS - 1));
}

// 暗着的时候按键要报告一次唤醒,并且只报告一次。
static void test_activity_while_dimmed_reports_wake_once(void) {
    golf_idle_state_t idle;
    golf_idle_init(&idle, 0);

    assert(golf_idle_tick(&idle, GOLF_IDLE_DIM_MS));
    assert(golf_idle_note_activity(&idle, GOLF_IDLE_DIM_MS + 5));   // 唤醒
    assert(!idle.dimmed);
    assert(!golf_idle_note_activity(&idle, GOLF_IDLE_DIM_MS + 40)); // 已经亮了,不再上报
    assert(!golf_idle_tick(&idle, GOLF_IDLE_DIM_MS + 60));          // 从唤醒那刻重新计时
}

// 唤醒之后还能再次闲置调暗 —— 不能只生效一次。
static void test_can_dim_again_after_waking(void) {
    golf_idle_state_t idle;
    golf_idle_init(&idle, 0);

    assert(golf_idle_tick(&idle, GOLF_IDLE_DIM_MS));
    assert(golf_idle_note_activity(&idle, 5 * GOLF_IDLE_DIM_MS));
    assert(golf_idle_tick(&idle, 6 * GOLF_IDLE_DIM_MS));
    assert(golf_idle_note_activity(&idle, 7 * GOLF_IDLE_DIM_MS));
    assert(golf_idle_tick(&idle, 8 * GOLF_IDLE_DIM_MS));
}

int main(void) {
    test_stays_bright_until_threshold();
    test_dims_exactly_at_threshold_and_only_once();
    test_activity_while_bright_only_resets_the_timer();
    test_activity_while_dimmed_reports_wake_once();
    test_can_dim_again_after_waking();
    puts("golf_idle: all tests passed");
    return 0;
}
