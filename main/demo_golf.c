// main/demo_golf.c —— 迷你高尔夫玩法页。
//
// 三键交互:
//   瞄准阶段   上/下 短按 = 微调 4 度;上/下 长按 = 粗调 32 度;确定 短按 = 进入力度
//   力度阶段   力度条自动来回摆动;确定 短按 = 按当前力度击球;上 = 退回瞄准
//   进洞之后   确定 短按 = 下一洞 / 结算
//
// 长按确定返回菜单由 main.c 统一拦截,所以本页不使用"按住蓄力"这类操作。
//
// 按键事件的一个硬约束:BUTTON_PRESS_DOWN 在按下瞬间就上报,而
// BUTTON_LONG_PRESS_START 要等长按阈值之后才上报。所以"按下即响应"与
// "区分短按/长按"对同一个键是互斥的,本页的处理是:
//   * 上/下 用 PRESS 做微调(手感跟手),长按到达时再补足 32-4=28 度,避免多走一格;
//   * 确定 在力度阶段用 PRESS(摆动条有效窗口只有几十毫秒),其他阶段用 CLICK,
//     这样"长按确定返回菜单"不会先闪出一格力度。
#include "demo.h"

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "golf_input.h"
#include "golf_model.h"
#include "lvgl.h"
#include "rtttl_player.h"
#include "ui_pixel.h"

// 游戏循环节拍。20ms 与 50Hz 刷新同量级,手感足够且不吃满 CPU。
#define GOLF_TICK_MS 20
// 瞄准阶段脏区要覆盖整条辅助线,球本身再加一圈描边。
#define GOLF_BALL_MARGIN (GOLF_BALL_RADIUS + 2)
// 连续撞墙时最多每 140ms 出一声,避免音效刷屏。
#define GOLF_BOUNCE_SOUND_GUARD_MS 140

// 画布几何:必须与 ui_pixel 面板内沿对齐。
#define COURSE_X 8
#define COURSE_Y 64
#define COURSE_PANEL_X 4
#define COURSE_PANEL_Y 60
#define COURSE_PANEL_W 232
#define COURSE_PANEL_H (GOLF_WORLD_H + 8)

// 屏幕布局。
#define HUD_X 6
#define HUD_Y 46
#define BATTERY_X 190
#define BATTERY_Y 28
#define BATTERY_W 46
// 力度条刻意不用 ui_pixel_panel_create:面板自带的右下投影会压到下面的状态文字上。
// 这里用墨色外框 + 纸色内槽的两块纯色方块,保持同一套配色与零圆角语言。
#define POWER_TRACK_X 8
#define POWER_TRACK_Y 228
#define POWER_TRACK_W 224
#define POWER_TRACK_H 16
#define POWER_FILL_X (POWER_TRACK_X + 4)
#define POWER_FILL_Y (POWER_TRACK_Y + 4)
#define POWER_FILL_MAX_W (POWER_TRACK_W - 8)
#define POWER_FILL_H (POWER_TRACK_H - 8)
#define STATUS_X 6
#define STATUS_Y 246
#define HINT_X 6
#define HINT_Y 264

static const char *TAG = "golf";

static const char *SFX_AIM = "aim:d=64,o=7,b=700:c7";
static const char *SFX_READY = "ready:d=32,o=6,b=400:c6,g6";
static const char *SFX_CANCEL = "cancel:d=32,o=6,b=400:g6,c6";
static const char *SFX_STRIKE = "hit:d=32,o=5,b=300:c6,e6";
static const char *SFX_BOUNCE = "bounce:d=32,o=6,b=420:16g";
static const char *SFX_SAND = "sand:d=32,o=3,b=220:16c";
static const char *SFX_WATER = "splash:d=16,o=5,b=200:c5,16p,g4";
static const char *SFX_SUNK = "in:d=16,o=6,b=200:c6,e6,g6,c7";
static const char *SFX_ROUND = "done:d=8,o=6,b=140:c6,e6,g6,c7,g6,c7";

// 调色板索引与 golf_px_t 一一对应,画布因此可以直接用语义值当索引。
static const uint32_t GOLF_PALETTE[] = {
    0x000000,   // GOLF_PX_NONE        透明
    0x3F6B22,   // GOLF_PX_ROUGH       粗草
    0x6FBF3A,   // GOLF_PX_FAIRWAY     球道
    0xA8E05A,   // GOLF_PX_GREEN       果岭
    0xE8D08A,   // GOLF_PX_SAND        沙坑
    0x3FA9E0,   // GOLF_PX_WATER       水障碍
    0x17202A,   // GOLF_PX_WALL        障碍墙
    0x0E1620,   // GOLF_PX_WALL_SHADOW 墙体投影
    0x0A0F14,   // GOLF_PX_CUP         洞口
    0xF4F4EA,   // GOLF_PX_POLE        旗杆
    0xE43B2F,   // GOLF_PX_FLAG        旗面
    0xFFFFFF,   // GOLF_PX_BALL        球心
    0x94A3B8,   // GOLF_PX_BALL_EDGE   球描边
    0x2A3A18,   // GOLF_PX_BALL_SHADOW 球的投影
    0xFFD928,   // GOLF_PX_AIM         瞄准辅助线
    0xFFFFFF,   // 预留
};

// 按键回调的载荷:只带"哪个键、什么事件",不做任何判断。
typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t ev;
} golf_key_msg_t;

typedef struct {
    int x0;
    int y0;
    int x1;
    int y1;
    bool valid;
} golf_rect_t;

LV_DRAW_BUF_DEFINE_STATIC(golf_buf, GOLF_WORLD_W, GOLF_WORLD_H, LV_COLOR_FORMAT_I4);

static golf_model_t s_model;
static golf_key_state_t s_keys;
static lv_obj_t *s_scr;
static lv_obj_t *s_canvas;
static lv_obj_t *s_hud_label;
static lv_obj_t *s_status_label;
static lv_obj_t *s_hint_label;
static lv_obj_t *s_battery_label;
static lv_obj_t *s_power_fill;
static lv_timer_t *s_timer;
static QueueHandle_t s_input_queue;
static golf_rect_t s_dirty;
static uint64_t s_last_bounce_sound_ms;
static int s_shown_power_percent = -1;
static int s_shown_strokes = -1;
static int s_shown_hole = -1;
static int s_shown_angle = -1;
static golf_phase_t s_shown_phase = GOLF_PHASE_ROUND_DONE;

static void handle_key(bsp_btn_t btn, bsp_btn_ev_t ev);
static void refresh_all(void);

static uint64_t now_ms(void) {
    return (uint64_t)esp_timer_get_time() / 1000ULL;
}

static void canvas_px(lv_obj_t *canvas, int x, int y, uint8_t color) {
    lv_draw_buf_t *draw_buf = lv_canvas_get_draw_buf(canvas);
    uint8_t *data = lv_draw_buf_goto_xy(draw_buf, x, y);
    if (!data) return;
    // I4:一个字节存两个像素,左像素在高 4 位。
    const uint8_t shift = (uint8_t)(4 - 4 * (x & 1));
    *data = (uint8_t)((*data & ~(0x0FU << shift)) | ((color & 0x0FU) << shift));
}

static void set_canvas_palette(lv_obj_t *canvas) {
    for (uint8_t i = 0; i < sizeof(GOLF_PALETTE) / sizeof(GOLF_PALETTE[0]); i++) {
        lv_canvas_set_palette(canvas, i,
                              lv_color_to_32(lv_color_hex(GOLF_PALETTE[i]), LV_OPA_COVER));
    }
}

// 只写画布缓冲,不触碰 LVGL 失效逻辑。
static void paint_pixels(int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > GOLF_WORLD_W - 1) x1 = GOLF_WORLD_W - 1;
    if (y1 > GOLF_WORLD_H - 1) y1 = GOLF_WORLD_H - 1;
    if (x0 > x1 || y0 > y1 || !s_canvas) return;

    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            uint8_t value = (uint8_t)golf_model_scene_pixel(&s_model, x, y);
            const golf_px_t overlay = golf_model_overlay_pixel(&s_model, x, y);
            if (overlay != GOLF_PX_NONE) value = (uint8_t)overlay;
            canvas_px(s_canvas, x, y, value);
        }
    }
}

// 整块失效。建屏阶段画布坐标尚未确定,所以这里不用 lv_obj_get_coords。
static void paint_full(void) {
    paint_pixels(0, 0, GOLF_WORLD_W - 1, GOLF_WORLD_H - 1);
    if (s_canvas) lv_obj_invalidate(s_canvas);
}

// 局部重绘:只失效变化的那一小块。仅在布局完成后的定时器里调用。
static void paint_region(int x0, int y0, int x1, int y1) {
    paint_pixels(x0, y0, x1, y1);
    if (!s_canvas) return;

    lv_area_t base;
    lv_obj_get_coords(s_canvas, &base);
    lv_area_t area = {
        .x1 = base.x1 + x0,
        .y1 = base.y1 + y0,
        .x2 = base.x1 + x1,
        .y2 = base.y1 + y1,
    };
    lv_obj_invalidate_area(s_canvas, &area);
}

// 当前动态内容(球 + 瞄准线)的包围盒。
static void dynamic_rect(golf_rect_t *rect) {
    const int ball_x = golf_model_ball_pixel_x(&s_model);
    const int ball_y = golf_model_ball_pixel_y(&s_model);
    rect->x0 = ball_x - GOLF_BALL_MARGIN;
    rect->y0 = ball_y - GOLF_BALL_MARGIN;
    rect->x1 = ball_x + GOLF_BALL_MARGIN;
    rect->y1 = ball_y + GOLF_BALL_MARGIN;

    if (s_model.phase == GOLF_PHASE_AIM) {
        const int tip_x = golf_model_aim_tip_pixel_x(&s_model);
        const int tip_y = golf_model_aim_tip_pixel_y(&s_model);
        if (tip_x - GOLF_BALL_MARGIN < rect->x0) rect->x0 = tip_x - GOLF_BALL_MARGIN;
        if (tip_y - GOLF_BALL_MARGIN < rect->y0) rect->y0 = tip_y - GOLF_BALL_MARGIN;
        if (tip_x + GOLF_BALL_MARGIN > rect->x1) rect->x1 = tip_x + GOLF_BALL_MARGIN;
        if (tip_y + GOLF_BALL_MARGIN > rect->y1) rect->y1 = tip_y + GOLF_BALL_MARGIN;
    }
    rect->valid = true;
}

// 只重画动态层变化的区域。没有变化时直接返回,避免无谓的刷屏。
static void paint_dynamic(void) {
    golf_rect_t current;
    dynamic_rect(&current);
    if (s_dirty.valid &&
        s_dirty.x0 == current.x0 && s_dirty.y0 == current.y0 &&
        s_dirty.x1 == current.x1 && s_dirty.y1 == current.y1) {
        return;
    }

    golf_rect_t area = current;
    if (s_dirty.valid) {
        if (s_dirty.x0 < area.x0) area.x0 = s_dirty.x0;
        if (s_dirty.y0 < area.y0) area.y0 = s_dirty.y0;
        if (s_dirty.x1 > area.x1) area.x1 = s_dirty.x1;
        if (s_dirty.y1 > area.y1) area.y1 = s_dirty.y1;
    }
    paint_region(area.x0, area.y0, area.x1, area.y1);
    s_dirty = current;
}

static void play_bounce_sound(void) {
    const uint64_t time_ms = now_ms();
    if (time_ms - s_last_bounce_sound_ms < GOLF_BOUNCE_SOUND_GUARD_MS) return;
    s_last_bounce_sound_ms = time_ms;
    rtttl_player_play(SFX_BOUNCE);
}

static void play_event_sound(golf_event_t event) {
    switch (event) {
        case GOLF_EVENT_AIM:          rtttl_player_play(SFX_AIM);    break;
        case GOLF_EVENT_POWER_START:  rtttl_player_play(SFX_READY);  break;
        case GOLF_EVENT_POWER_CANCEL: rtttl_player_play(SFX_CANCEL); break;
        case GOLF_EVENT_STRIKE:       rtttl_player_play(SFX_STRIKE); break;
        case GOLF_EVENT_BOUNCE:       play_bounce_sound();           break;
        case GOLF_EVENT_SAND:         rtttl_player_play(SFX_SAND);   break;
        case GOLF_EVENT_WATER:        rtttl_player_play(SFX_WATER);  break;
        case GOLF_EVENT_SUNK:         rtttl_player_play(SFX_SUNK);   break;
        case GOLF_EVENT_ROUND_DONE:   rtttl_player_play(SFX_ROUND);  break;
        default: break;
    }
}

static const char *hint_for_phase(golf_phase_t phase) {
    switch (phase) {
        case GOLF_PHASE_AIM:        return "UP/DN AIM   OK SWING";
        case GOLF_PHASE_POWER:      return "OK SWING   UP CANCEL";
        case GOLF_PHASE_ROLLING:    return "ROLLING...";
        case GOLF_PHASE_SUNK:       return "OK NEXT HOLE";
        default:                    return "OK PLAY AGAIN";
    }
}

static void refresh_battery(void) {
    if (!s_battery_label) return;
    const int soc = bsp_battery_soc();
    if (soc < 0) {
        // 电量计不可用时不要显示数字,只给一个中性占位。
        lv_label_set_text(s_battery_label, "--%");
    } else {
        lv_label_set_text_fmt(s_battery_label, "%d%%", soc);
    }
}

static void refresh_hud(void) {
    const golf_hole_t *hole = golf_model_hole(&s_model);
    lv_label_set_text_fmt(s_hud_label, "HOLE %u/%u  PAR %u  STROKE %u",
                          (unsigned)(s_model.hole_index + 1),
                          (unsigned)GOLF_HOLE_COUNT,
                          (unsigned)hole->par,
                          (unsigned)s_model.strokes);
    s_shown_hole = s_model.hole_index;
    s_shown_strokes = s_model.strokes;
}

static void refresh_status(void) {
    const golf_hole_t *hole = golf_model_hole(&s_model);
    switch (s_model.phase) {
        case GOLF_PHASE_AIM:
            lv_label_set_text_fmt(s_status_label, "%s  AIM %d DEG",
                                  hole->name, (int)s_model.angle_deg);
            break;
        case GOLF_PHASE_POWER:
            lv_label_set_text_fmt(s_status_label, "POWER %d%%",
                                  golf_model_power_percent(&s_model));
            break;
        case GOLF_PHASE_ROLLING:
            lv_label_set_text(s_status_label, "ROLLING");
            break;
        case GOLF_PHASE_SUNK:
            lv_label_set_text_fmt(s_status_label, "IN %u STROKES",
                                  (unsigned)s_model.scores[s_model.hole_index]);
            break;
        default:
            lv_label_set_text_fmt(s_status_label, "TOTAL %d  PAR %d",
                                  golf_model_total_strokes(&s_model),
                                  golf_model_total_par());
            break;
    }
    lv_label_set_text(s_hint_label, hint_for_phase(s_model.phase));
    s_shown_phase = s_model.phase;
    s_shown_angle = s_model.angle_deg;
}

static void refresh_power_bar(void) {
    int percent = 0;
    if (s_model.phase == GOLF_PHASE_POWER) percent = golf_model_power_percent(&s_model);
    if (percent == s_shown_power_percent) return;
    s_shown_power_percent = percent;

    int width = POWER_FILL_MAX_W * percent / 100;
    if (width < 0) width = 0;
    lv_obj_set_width(s_power_fill, width);
    // 力度越大越红,给一个不用读数字也能判断的提示。
    uint32_t color = UI_YELLOW;
    if (percent >= 80) color = UI_RED;
    else if (percent >= 50) color = UI_ORANGE;
    lv_obj_set_style_bg_color(s_power_fill, lv_color_hex(color), 0);
}

static void refresh_all(void) {
    if (!s_scr) return;
    s_dirty.valid = false;
    s_shown_power_percent = -1;
    s_shown_hole = -1;
    s_shown_strokes = -1;
    s_shown_angle = -1;
    s_shown_phase = GOLF_PHASE_ROUND_DONE;
    paint_full();
    refresh_battery();
    refresh_hud();
    refresh_status();
    refresh_power_bar();
}

// 每拍只更新真正变化的控件。LVGL 堆只有 24KB,状态文字每拍重建会持续产生碎片,
// 所以角度、力度、阶段、杆数都要先比对再写。
static void refresh_live(void) {
    if (!s_scr) return;
    if (s_model.hole_index != s_shown_hole || s_model.strokes != s_shown_strokes) {
        refresh_hud();
    }

    bool status_stale = s_model.phase != s_shown_phase;
    if (!status_stale && s_model.phase == GOLF_PHASE_AIM) {
        status_stale = s_model.angle_deg != s_shown_angle;
    }
    if (!status_stale && s_model.phase == GOLF_PHASE_POWER) {
        status_stale = golf_model_power_percent(&s_model) != s_shown_power_percent;
    }
    if (status_stale) refresh_status();

    refresh_power_bar();
}

// 按键翻译层(golf_input.c)只认自己的三键枚举,这里做一次映射。
// OK 长按被 main.c 统一拦截用于返回菜单,所以本页收不到 GOLF_KEY_EV_LONG 的 OK。
static bool map_key(bsp_btn_t btn, bsp_btn_ev_t ev,
                    golf_key_t *out_key, golf_key_ev_t *out_ev) {
    switch (btn) {
        case BSP_BTN_UP:   *out_key = GOLF_KEY_UP;   break;
        case BSP_BTN_DOWN: *out_key = GOLF_KEY_DOWN; break;
        case BSP_BTN_OK:   *out_key = GOLF_KEY_OK;   break;
        default: return false;
    }
    switch (ev) {
        case BSP_BTN_PRESS: *out_ev = GOLF_KEY_EV_PRESS; break;
        case BSP_BTN_CLICK: *out_ev = GOLF_KEY_EV_CLICK; break;
        case BSP_BTN_LONG:  *out_ev = GOLF_KEY_EV_LONG;  break;
        default: return false;   // 双击不参与本页交互
    }
    return true;
}

static void handle_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    golf_key_t key;
    golf_key_ev_t key_ev;
    if (!map_key(btn, ev, &key, &key_ev)) return;

    const uint8_t hole_before = s_model.hole_index;
    const golf_event_t event = golf_input_key(&s_model, &s_keys, key, key_ev, now_ms());

    if (s_model.hole_index != hole_before) {
        // 换洞/重开等于整屏都变了,局部重绘不够,直接全画。
        s_dirty.valid = false;
        refresh_all();
    }
    if (event != GOLF_EVENT_NONE) play_event_sound(event);
}

static void timer_cb(lv_timer_t *timer) {
    (void)timer;

    golf_key_msg_t input;
    while (s_input_queue && xQueueReceive(s_input_queue, &input, 0) == pdTRUE) {
        handle_key(input.btn, input.ev);
    }

    const golf_event_t event = golf_model_tick(&s_model, GOLF_TICK_MS);
    if (event != GOLF_EVENT_NONE) play_event_sound(event);

    paint_dynamic();
    refresh_live();
}

static lv_obj_t *label_create(lv_obj_t *parent, const lv_font_t *font,
                              uint32_t color, int x, int y) {
    lv_obj_t *label = ui_pixel_label(parent, "", font, color);
    lv_obj_set_pos(label, x, y);
    return label;
}

// 与 ui_pixel 内部同款的纯色方块:零圆角、无边框、无内边距。
static lv_obj_t *block_create(lv_obj_t *parent, int x, int y, int w, int h,
                              uint32_t color) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    return obj;
}

void demo_golf_enter(void) {
    golf_model_init(&s_model);
    s_input_queue = xQueueCreate(8, sizeof(golf_key_msg_t));
    if (!s_input_queue) ESP_LOGE(TAG, "input queue allocation failed");
    for (int i = 0; i < GOLF_KEY_COUNT; i++) {
        s_keys.last_press_ms[i] = 0;
        s_keys.press_seen[i] = false;
    }
    s_dirty.valid = false;
    s_last_bounce_sound_ms = 0;

    if (rtttl_player_start() != ESP_OK) {
        ESP_LOGW(TAG, "sound effects unavailable");
    }

    s_scr = ui_pixel_screen_create("Mini Golf");

    s_battery_label = label_create(s_scr, &lv_font_montserrat_14, UI_INK,
                                   BATTERY_X, BATTERY_Y);
    lv_obj_set_width(s_battery_label, BATTERY_W);
    lv_obj_set_style_text_align(s_battery_label, LV_TEXT_ALIGN_RIGHT, 0);

    s_hud_label = label_create(s_scr, &lv_font_montserrat_14, UI_INK, HUD_X, HUD_Y);

    // 球场面板的内沿正好落在 (COURSE_X, COURSE_Y),画布贴满这块区域。
    ui_pixel_panel_create(s_scr, COURSE_PANEL_X, COURSE_PANEL_Y,
                          COURSE_PANEL_W, COURSE_PANEL_H, UI_GRASS_DARK);
    LV_DRAW_BUF_INIT_STATIC(golf_buf);
    s_canvas = lv_canvas_create(s_scr);
    lv_canvas_set_draw_buf(s_canvas, &golf_buf);
    set_canvas_palette(s_canvas);
    lv_obj_set_pos(s_canvas, COURSE_X, COURSE_Y);

    // 力度条:墨色外框 + 纸色内槽 + 会变宽变色的填充块。
    block_create(s_scr, POWER_TRACK_X, POWER_TRACK_Y, POWER_TRACK_W, POWER_TRACK_H, UI_INK);
    block_create(s_scr, POWER_FILL_X, POWER_FILL_Y, POWER_FILL_MAX_W, POWER_FILL_H, UI_PAPER);
    s_power_fill = block_create(s_scr, POWER_FILL_X, POWER_FILL_Y, 0, POWER_FILL_H, UI_YELLOW);

    s_status_label = label_create(s_scr, &lv_font_montserrat_14, UI_INK, STATUS_X, STATUS_Y);
    s_hint_label = label_create(s_scr, &lv_font_montserrat_14, UI_INK, HINT_X, HINT_Y);

    refresh_all();
    lv_screen_load(s_scr);
    s_timer = lv_timer_create(timer_cb, GOLF_TICK_MS, NULL);

    lv_mem_monitor_t memory;
    lv_mem_monitor(&memory);
    ESP_LOGI(TAG, "ready: %u bytes LVGL free, %u%% fragmented, canvas %u bytes",
             (unsigned)memory.free_size, memory.frag_pct,
             (unsigned)(GOLF_WORLD_W * GOLF_WORLD_H / 2));
}

void demo_golf_exit(void) {
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    rtttl_player_stop();
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
    }
    if (s_input_queue) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
    }
    s_canvas = NULL;
    s_hud_label = NULL;
    s_status_label = NULL;
    s_hint_label = NULL;
    s_battery_label = NULL;
    s_power_fill = NULL;
    s_dirty.valid = false;
}

void demo_golf_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (!s_input_queue) return;
    const golf_key_msg_t msg = { .btn = btn, .ev = ev };
    if (xQueueSend(s_input_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "input queue full: key=%d event=%d", btn, ev);
    }
}
