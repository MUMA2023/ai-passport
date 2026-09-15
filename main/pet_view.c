// main/pet_view.c —— 「家机桌宠」的界面绘制。
//
// 界面元素全部由 LVGL 图元拼出（圆角矩形、圆、标签），不使用位图资源：
// 这块板子没有 PSRAM，纯图元方案既省内存，缩放和动画也更灵活。
#include "pet_view.h"

#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(lv_font_pet_16);

#define PET_SCREEN_W 240
#define PET_SCREEN_H 320

/* 场景分层：上方是墙，PET_WALL_BOTTOM 以下是桌面。 */
#define PET_WALL_BOTTOM 156

/* 宠物本体在 pet_root 内的坐标（pet_root 为 120x120）。 */
#define PET_ROOT_W 120
#define PET_ROOT_H 120
#define PET_BASE_X 60
/* 身体最低点（脚底）在 pet_root 内的 y。 */
#define PET_FOOT_LOCAL_Y 104
/* pet_root 的 y：让脚底正好落在桌面线上，看起来是坐在桌上。 */
#define PET_BASE_Y (PET_WALL_BOTTOM - PET_FOOT_LOCAL_Y)

#define PET_TOAST_MS 1400
#define PET_WANDER_MS 5200
#define PET_BLINK_MS 2600
#define PET_BLINK_FADE_MS 90
#define PET_BOB_MS 1100
#define PET_BOB_LIFT 7
#define PET_WANDER_SPAN 26

struct pet_view {
    lv_obj_t *screen;
    lv_obj_t *hud;

    lv_obj_t *pet_root;
    lv_obj_t *eye_l;
    lv_obj_t *eye_r;
    lv_obj_t *eye_l_dot;
    lv_obj_t *eye_r_dot;
    lv_obj_t *mouth;
    lv_obj_t *zzz;
    lv_obj_t *spark[3];

    lv_obj_t *toast;
    lv_obj_t *status_label;
    lv_obj_t *bond_label;
    lv_obj_t *bond_fill;

    /* 状态页的六行明细，随陪伴时长一起刷新。 */
    lv_obj_t *rows[6];
    lv_obj_t *clock_label;

    lv_timer_t *blink_timer;
    lv_timer_t *wander_timer;
    lv_timer_t *toast_timer;

    pet_mood_t mood;
    int32_t base_x;
    int32_t base_y;
    bool sleeping;
    uint32_t wander_seed;
};

/* ------------------------------------------------------------------ 基础图元 */

static lv_obj_t *pet_box(lv_obj_t *parent, int x, int y, int w, int h,
                         uint32_t color, int radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    return obj;
}

static lv_obj_t *pet_transparent(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    return obj;
}

static lv_obj_t *pet_label(lv_obj_t *parent, const char *text,
                           const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static lv_obj_t *pet_panel(lv_obj_t *parent, int x, int y, int w, int h, int radius)
{
    lv_obj_t *panel = pet_box(parent, x, y, w, h, PET_C_PANEL, radius);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0xE0D8C8), 0);
    return panel;
}

static uint32_t pet_next_random(pet_view_t *view)
{
    uint32_t x = view->wander_seed ? view->wander_seed : 0x9E3779B9U;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    view->wander_seed = x;
    return x;
}

/* ------------------------------------------------------------------ 顶部信息条 */

/* HUD 内部句柄。挂在 HUD 的 user_data 上，随 HUD 一起释放，
   这样就不必依赖「第几个子对象是谁」这种脆弱的下标约定。 */
typedef struct {
    lv_obj_t *clock;
    lv_obj_t *shell;
    lv_obj_t *fill;
    lv_obj_t *percent;
} pet_hud_parts_t;

static void pet_hud_delete_cb(lv_event_t *event)
{
    lv_free(lv_event_get_user_data(event));
}

lv_obj_t *pet_hud_create(lv_obj_t *screen)
{
    pet_hud_parts_t *parts = lv_malloc(sizeof(pet_hud_parts_t));
    if (!parts) return NULL;

    lv_obj_t *hud = pet_transparent(screen, 0, 0, PET_SCREEN_W, 34);
    lv_obj_set_user_data(hud, parts);
    lv_obj_add_event_cb(hud, pet_hud_delete_cb, LV_EVENT_DELETE, parts);

    parts->clock = pet_label(hud, "--:--", &lv_font_montserrat_20, PET_C_INK);
    lv_obj_set_pos(parts->clock, 16, 8);

    parts->shell = pet_box(hud, 198, 12, 26, 13, PET_C_PANEL, 3);
    lv_obj_set_style_border_width(parts->shell, 2, 0);
    lv_obj_set_style_border_color(parts->shell, lv_color_hex(PET_C_INK), 0);
    parts->fill = pet_box(parts->shell, 2, 2, 0, 9, PET_C_GOOD, 2);
    lv_obj_set_style_bg_opa(parts->fill, LV_OPA_TRANSP, 0);
    pet_box(hud, 224, 15, 3, 7, PET_C_INK, 1);

    parts->percent = pet_label(hud, "", &lv_font_pet_16, PET_C_INK);
    lv_obj_set_pos(parts->percent, 148, 10);
    return hud;
}

void pet_hud_sync(lv_obj_t *hud, const pet_model_t *model,
                  const pet_view_battery_t *battery)
{
    if (!hud) return;
    pet_hud_parts_t *parts = (pet_hud_parts_t *)lv_obj_get_user_data(hud);
    if (!parts) return;

    if (parts->clock) {
        char text[8];
        pet_model_format_clock(model, text, sizeof(text));
        lv_label_set_text(parts->clock, text);
    }

    /* 电量：可用时画进度并在旁边写百分比；不可用时留空而不是显示假数字。 */
    if (battery && battery->available && battery->soc >= 0) {
        int soc = battery->soc > 100 ? 100 : battery->soc;
        uint32_t color = PET_C_GOOD;
        if (soc <= 15) color = PET_C_BAD;
        else if (soc <= 40) color = PET_C_WARN;

        if (parts->fill) {
            lv_obj_set_size(parts->fill, soc * 22 / 100, 9);
            lv_obj_set_style_bg_color(parts->fill, lv_color_hex(color), 0);
            lv_obj_set_style_bg_opa(parts->fill, LV_OPA_COVER, 0);
        }
        if (parts->percent) lv_label_set_text_fmt(parts->percent, "%d%%", soc);
    } else {
        if (parts->fill) lv_obj_set_style_bg_opa(parts->fill, LV_OPA_TRANSP, 0);
        if (parts->percent) lv_label_set_text(parts->percent, "");
    }
}

/* ------------------------------------------------------------------ 动画 */

static void pet_anim_y(void *obj, int32_t value)
{
    lv_obj_set_y((lv_obj_t *)obj, value);
}

static void pet_anim_x(void *obj, int32_t value)
{
    lv_obj_set_x((lv_obj_t *)obj, value);
}

static void pet_anim_opa(void *obj, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

static void pet_start_bob(pet_view_t *view);

/* 跳一下之后要恢复「呼吸」动画，否则互动一次以后宠物就再也不会上下浮动了。 */
static void pet_jump_done_cb(lv_anim_t *anim)
{
    pet_view_t *view = (pet_view_t *)lv_anim_get_user_data(anim);
    if (!view || !view->pet_root) return;
    lv_obj_set_y(view->pet_root, view->base_y);
    if (!view->sleeping) pet_start_bob(view);
}

static void pet_start_bob(pet_view_t *view)
{
    lv_anim_delete(view->pet_root, pet_anim_y);
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, view->pet_root);
    lv_anim_set_exec_cb(&anim, pet_anim_y);
    lv_anim_set_values(&anim, view->base_y, view->base_y - PET_BOB_LIFT);
    lv_anim_set_duration(&anim, PET_BOB_MS);
    lv_anim_set_playback_duration(&anim, PET_BOB_MS);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_start(&anim);
}

static void pet_start_jump(pet_view_t *view, int32_t lift)
{
    lv_anim_delete(view->pet_root, pet_anim_y);
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, view->pet_root);
    lv_anim_set_exec_cb(&anim, pet_anim_y);
    lv_anim_set_values(&anim, view->base_y, view->base_y - lift);
    lv_anim_set_duration(&anim, 130);
    lv_anim_set_playback_duration(&anim, 190);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_user_data(&anim, view);
    lv_anim_set_completed_cb(&anim, pet_jump_done_cb);
    lv_anim_start(&anim);
}

static void pet_start_shake(pet_view_t *view)
{
    lv_anim_delete(view->pet_root, pet_anim_x);
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, view->pet_root);
    lv_anim_set_exec_cb(&anim, pet_anim_x);
    lv_anim_set_values(&anim, lv_obj_get_x(view->pet_root), view->base_x - 8);
    lv_anim_set_duration(&anim, 70);
    lv_anim_set_playback_duration(&anim, 70);
    lv_anim_set_repeat_count(&anim, 2);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_start(&anim);
}

static void pet_wander_cb(lv_timer_t *timer)
{
    pet_view_t *view = (pet_view_t *)lv_timer_get_user_data(timer);
    if (!view || view->sleeping || !view->pet_root) return;
    const int32_t offset =
        (int32_t)(pet_next_random(view) % (uint32_t)(PET_WANDER_SPAN * 2 + 1)) - PET_WANDER_SPAN;

    lv_anim_delete(view->pet_root, pet_anim_x);
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, view->pet_root);
    lv_anim_set_exec_cb(&anim, pet_anim_x);
    lv_anim_set_values(&anim, lv_obj_get_x(view->pet_root), view->base_x + offset);
    lv_anim_set_duration(&anim, 1400);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_start(&anim);
}

/* 眨眼要短促：做一个快速淡出再淡回的动画，而不是把眼睛按住半闭两秒多。 */
static void pet_blink_anim(lv_obj_t *eye)
{
    if (!eye) return;
    lv_anim_delete(eye, pet_anim_opa);
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, eye);
    lv_anim_set_exec_cb(&anim, pet_anim_opa);
    lv_anim_set_values(&anim, LV_OPA_COVER, LV_OPA_20);
    lv_anim_set_duration(&anim, PET_BLINK_FADE_MS);
    lv_anim_set_playback_duration(&anim, PET_BLINK_FADE_MS);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_start(&anim);
}

static void pet_blink_cb(lv_timer_t *timer)
{
    pet_view_t *view = (pet_view_t *)lv_timer_get_user_data(timer);
    if (!view || view->sleeping) return;
    pet_blink_anim(view->eye_l);
    pet_blink_anim(view->eye_r);
}

static void pet_toast_hide_cb(lv_timer_t *timer)
{
    pet_view_t *view = (pet_view_t *)lv_timer_get_user_data(timer);
    if (!view || !view->toast) return;
    lv_obj_add_flag(view->toast, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(timer);
}

/* ------------------------------------------------------------------ 宠物本体 */

static void pet_apply_face(pet_view_t *view, pet_mood_t mood)
{
    int eye_w = 14;
    int eye_h = 15;
    int eye_y = 44;
    int mouth_w = 16;
    int mouth_h = 5;
    int mouth_y = 66;
    bool show_zzz = false;
    bool show_sparks = false;

    switch (mood) {
    case PET_MOOD_SLEEPING:
        eye_h = 3;
        mouth_w = 10;
        mouth_h = 3;
        mouth_y = 68;
        show_zzz = true;
        break;
    case PET_MOOD_HUNGRY:
        mouth_w = 12;
        mouth_h = 12;
        mouth_y = 62;
        break;
    case PET_MOOD_SLEEPY:
        eye_h = 8;
        mouth_w = 12;
        mouth_h = 4;
        break;
    case PET_MOOD_HAPPY:
        eye_w = 16;
        eye_h = 9;
        eye_y = 46;
        mouth_w = 22;
        mouth_h = 7;
        mouth_y = 64;
        break;
    case PET_MOOD_EXCITED:
        eye_w = 16;
        eye_h = 17;
        eye_y = 43;
        mouth_w = 24;
        mouth_h = 9;
        mouth_y = 63;
        show_sparks = true;
        break;
    case PET_MOOD_LONELY:
        eye_h = 12;
        mouth_w = 14;
        mouth_h = 4;
        mouth_y = 70;
        break;
    case PET_MOOD_CURIOUS:
        eye_w = 15;
        eye_h = 17;
        eye_y = 43;
        mouth_w = 12;
        mouth_h = 6;
        break;
    case PET_MOOD_CONTENT:
    default:
        break;
    }

    const int eye_radius = (eye_w < eye_h ? eye_w : eye_h) / 2;
    lv_obj_set_size(view->eye_l, eye_w, eye_h);
    lv_obj_set_size(view->eye_r, eye_w, eye_h);
    lv_obj_set_style_radius(view->eye_l, eye_radius, 0);
    lv_obj_set_style_radius(view->eye_r, eye_radius, 0);
    lv_obj_set_y(view->eye_l, eye_y);
    lv_obj_set_y(view->eye_r, eye_y);
    lv_obj_set_x(view->eye_l, 34 - (eye_w - 14) / 2);
    lv_obj_set_x(view->eye_r, 62 - (eye_w - 14) / 2);

    /* 眼睛眯成一条线时，高光会露到眼眶外面，所以只在眼睛够大时才显示。 */
    const bool show_dots = eye_h >= 9;
    if (view->eye_l_dot) {
        if (show_dots) lv_obj_remove_flag(view->eye_l_dot, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(view->eye_l_dot, LV_OBJ_FLAG_HIDDEN);
    }
    if (view->eye_r_dot) {
        if (show_dots) lv_obj_remove_flag(view->eye_r_dot, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(view->eye_r_dot, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_set_size(view->mouth, mouth_w, mouth_h);
    lv_obj_set_style_radius(view->mouth, (mouth_w < mouth_h ? mouth_w : mouth_h) / 2, 0);
    lv_obj_set_x(view->mouth, 60 - mouth_w / 2);
    lv_obj_set_y(view->mouth, mouth_y);

    if (view->zzz) {
        if (show_zzz) lv_obj_remove_flag(view->zzz, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(view->zzz, LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < 3; i++) {
        if (!view->spark[i]) continue;
        if (show_sparks) lv_obj_remove_flag(view->spark[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(view->spark[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void pet_build_body(pet_view_t *view)
{
    lv_obj_t *root = view->pet_root;

    /* 影子：让它看起来是坐在桌面上而不是飘着。 */
    pet_box(root, 30, 100, 60, 10, 0xE3D6BF, 5);

    /* 尾巴。身体右边界约在 x=104，所以尾巴要伸到 118 才露得出来：
       放在 92..108 的话会被身体压掉大半，只剩几个像素。 */
    pet_box(root, 94, 68, 24, 12, PET_C_ACCENT, 6);

    /* 脚。 */
    pet_box(root, 32, 92, 20, 12, PET_C_BODY_SHADE, 6);
    pet_box(root, 68, 92, 20, 12, PET_C_BODY_SHADE, 6);

    /* 耳朵（先画，被身体压住一半）。 */
    lv_obj_t *ear_l = pet_box(root, 22, 14, 22, 24, PET_C_BODY, 10);
    lv_obj_t *ear_r = pet_box(root, 76, 14, 22, 24, PET_C_BODY, 10);
    pet_box(ear_l, 6, 7, 10, 11, PET_C_CHEEK, 5);
    pet_box(ear_r, 6, 7, 10, 11, PET_C_CHEEK, 5);

    /* 身体。 */
    pet_box(root, 16, 28, 88, 74, PET_C_BODY, 34);

    /* 眼睛。 */
    view->eye_l = pet_box(root, 34, 44, 14, 15, PET_C_INK, 7);
    view->eye_r = pet_box(root, 62, 44, 14, 15, PET_C_INK, 7);
    view->eye_l_dot = pet_box(view->eye_l, 3, 3, 5, 5, 0xFFFFFF, 2);
    view->eye_r_dot = pet_box(view->eye_r, 3, 3, 5, 5, 0xFFFFFF, 2);

    /* 腮红。 */
    lv_obj_t *cheek_l = pet_box(root, 24, 62, 12, 9, PET_C_CHEEK, 4);
    lv_obj_t *cheek_r = pet_box(root, 84, 62, 12, 9, PET_C_CHEEK, 4);
    lv_obj_set_style_opa(cheek_l, LV_OPA_70, 0);
    lv_obj_set_style_opa(cheek_r, LV_OPA_70, 0);

    /* 嘴。 */
    view->mouth = pet_box(root, 52, 66, 16, 5, PET_C_INK, 2);

    /* 打盹时飘出的 z。 */
    view->zzz = pet_label(root, "z z", &lv_font_pet_16, PET_C_MUTED);
    lv_obj_set_pos(view->zzz, 92, 18);
    lv_obj_add_flag(view->zzz, LV_OBJ_FLAG_HIDDEN);

    /* 开心时冒出的小光点。 */
    for (int i = 0; i < 3; i++) {
        view->spark[i] = pet_box(root, 16 + i * 42, 20 - i * 3, 7, 7, PET_C_HEART, 3);
        lv_obj_add_flag(view->spark[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* ------------------------------------------------------------------ 主界面 */

pet_view_t *pet_view_create(void)
{
    pet_view_t *view = lv_malloc(sizeof(pet_view_t));
    if (!view) return NULL;
    memset(view, 0, sizeof(*view));
    view->wander_seed = 0x9E3779B9U;
    /* 用一个不可能的真实心情占位，保证第一次 sync 一定会重画表情。 */
    view->mood = PET_MOOD_COUNT;

    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_bg_color(screen, lv_color_hex(PET_C_WALL_TOP), 0);
    view->screen = screen;

    /* 房间：上方是墙，下方是桌面。 */
    pet_box(screen, 0, 0, PET_SCREEN_W, PET_WALL_BOTTOM, PET_C_WALL_TOP, 0);
    pet_box(screen, 0, 92, PET_SCREEN_W, PET_WALL_BOTTOM - 92, PET_C_WALL_LOW, 0);
    pet_box(screen, 0, PET_WALL_BOTTOM, PET_SCREEN_W, PET_SCREEN_H - PET_WALL_BOTTOM, PET_C_DESK, 0);
    pet_box(screen, 0, PET_WALL_BOTTOM, PET_SCREEN_W, 4, PET_C_DESK_EDGE, 0);
    /* 桌边一块小地毯，给宠物一个落点。 */
    pet_box(screen, 62, PET_WALL_BOTTOM + 4, 116, 12, PET_C_DESK_EDGE, 6);

    view->hud = pet_hud_create(screen);

    view->base_x = PET_BASE_X;
    view->base_y = PET_BASE_Y;
    view->pet_root = pet_transparent(screen, view->base_x, view->base_y,
                                     PET_ROOT_W, PET_ROOT_H);
    pet_build_body(view);

    /* 状态气泡。 */
    lv_obj_t *bubble = pet_panel(screen, 16, 226, 208, 34, 14);
    view->status_label = pet_label(bubble, " ", &lv_font_pet_16, PET_C_INK);
    lv_obj_center(view->status_label);

    /* 亲密度条。 */
    view->bond_label = pet_label(screen, "陌生", &lv_font_pet_16, PET_C_INK);
    lv_obj_set_pos(view->bond_label, 16, 266);
    lv_obj_t *track = pet_box(screen, 16, 288, 208, 12, 0xE7E0D2, 6);
    view->bond_fill = pet_box(track, 0, 0, 0, 12, PET_C_HEART, 6);

    /* 浮层提示。 */
    view->toast = pet_box(screen, 46, 178, 148, 30, PET_C_INK, 15);
    lv_obj_t *toast_label = pet_label(view->toast, "", &lv_font_pet_16, PET_C_PANEL);
    lv_obj_center(toast_label);
    lv_obj_add_flag(view->toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_user_data(view->toast, toast_label);

    pet_apply_face(view, PET_MOOD_CONTENT);
    pet_start_bob(view);

    view->blink_timer = lv_timer_create(pet_blink_cb, PET_BLINK_MS, view);
    view->wander_timer = lv_timer_create(pet_wander_cb, PET_WANDER_MS, view);
    view->toast_timer = lv_timer_create(pet_toast_hide_cb, PET_TOAST_MS, view);
    if (view->toast_timer) lv_timer_pause(view->toast_timer);

    lv_screen_load(screen);
    return view;
}

void pet_view_sync(pet_view_t *view, const pet_model_t *model,
                   const pet_view_battery_t *battery)
{
    if (!view || !model) return;

    const pet_mood_t mood = pet_model_mood(model);
    if (mood != view->mood) {
        const bool was_sleeping = view->sleeping;
        view->mood = mood;
        view->sleeping = mood == PET_MOOD_SLEEPING;
        pet_apply_face(view, mood);

        if (view->sleeping && !was_sleeping) {
            /* 睡着后停止走动和浮动，回到原位安静地待着。 */
            lv_anim_delete(view->pet_root, pet_anim_x);
            lv_anim_delete(view->pet_root, pet_anim_y);
            lv_obj_set_x(view->pet_root, view->base_x);
            lv_obj_set_y(view->pet_root, view->base_y);
        } else if (!view->sleeping && was_sleeping) {
            pet_start_bob(view);
        }
    }

    pet_hud_sync(view->hud, model, battery);

    if (view->status_label) {
        char text[PET_STATUS_MAX];
        pet_model_status_text(model, text, sizeof(text));
        lv_label_set_text(view->status_label, text);
    }

    const uint8_t progress = pet_model_bond_progress(model);
    if (view->bond_label) {
        lv_label_set_text_fmt(view->bond_label, "%s %u%%",
                              pet_bond_name(pet_model_bond(model)), (unsigned)progress);
    }
    if (view->bond_fill) {
        int width = progress * 204 / 100;
        if (progress > 0 && width < 12) width = 12;
        lv_obj_set_size(view->bond_fill, width, 12);
    }
}

void pet_view_react(pet_view_t *view, pet_action_t action, pet_result_t result)
{
    if (!view || !view->pet_root) return;

    if (result != PET_OK) {
        /* 拒绝时左右摇头。 */
        pet_start_shake(view);
        return;
    }

    /* 三种互动用不同的跳跃高度，手感上能区分开。 */
    int32_t lift = 12;
    if (action == PET_ACT_FEED) lift = 9;
    else if (action == PET_ACT_PLAY) lift = 17;
    pet_start_jump(view, lift);

    for (int i = 0; i < 3; i++) {
        if (!view->spark[i]) continue;
        lv_obj_remove_flag(view->spark[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(view->spark[i], LV_OPA_COVER, 0);
    }
    /* 让下一次 sync 重新计算表情（互动后心情通常已经变了）。 */
    view->mood = PET_MOOD_COUNT;
}

void pet_view_toast(pet_view_t *view, const char *text)
{
    if (!view || !view->toast) return;
    lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(view->toast);
    if (label) lv_label_set_text(label, text);
    lv_obj_remove_flag(view->toast, LV_OBJ_FLAG_HIDDEN);
    if (view->toast_timer) {
        lv_timer_reset(view->toast_timer);
        lv_timer_resume(view->toast_timer);
    }
}

void pet_view_delete(pet_view_t *view)
{
    if (!view) return;
    if (view->blink_timer) lv_timer_delete(view->blink_timer);
    if (view->wander_timer) lv_timer_delete(view->wander_timer);
    if (view->toast_timer) lv_timer_delete(view->toast_timer);
    /* 注意：lv_anim_delete 的 var 传 NULL 表示「删掉所有动画」，
       所以每个句柄都要先判空，不能图省事直接传。 */
    if (view->pet_root) lv_anim_delete(view->pet_root, NULL);
    if (view->eye_l) lv_anim_delete(view->eye_l, NULL);
    if (view->eye_r) lv_anim_delete(view->eye_r, NULL);
    if (view->screen) lv_obj_delete(view->screen);
    lv_free(view);
}

/* ------------------------------------------------------------------ 状态页 */

static void pet_fill_status_rows(pet_view_t *view, const pet_model_t *model)
{
    char age[24];
    pet_model_format_age(model->age_seconds, age, sizeof(age));
    char lines[6][PET_STATUS_MAX];
    snprintf(lines[0], sizeof(lines[0]), "亲密度   %s", pet_bond_name(pet_model_bond(model)));
    snprintf(lines[1], sizeof(lines[1]), "陪伴时长 %s", age);
    snprintf(lines[2], sizeof(lines[2]), "撸一撸   %u 次", (unsigned)model->pet_count);
    snprintf(lines[3], sizeof(lines[3]), "喂食     %u 次", (unsigned)model->feed_count);
    snprintf(lines[4], sizeof(lines[4]), "玩耍     %u 次", (unsigned)model->play_count);
    snprintf(lines[5], sizeof(lines[5]), "总互动   %u 次", (unsigned)model->care_count);
    for (int i = 0; i < 6; i++) {
        if (view->rows[i]) lv_label_set_text(view->rows[i], lines[i]);
    }
}

pet_view_t *pet_status_view_create(const pet_model_t *model,
                                   const pet_view_battery_t *battery)
{
    if (!model) return NULL;
    pet_view_t *view = lv_malloc(sizeof(pet_view_t));
    if (!view) return NULL;
    memset(view, 0, sizeof(*view));

    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_bg_color(screen, lv_color_hex(PET_C_WALL_LOW), 0);
    view->screen = screen;

    view->hud = pet_hud_create(screen);
    pet_hud_sync(view->hud, model, battery);

    lv_obj_t *title = pet_label(screen, "陪伴档案", &lv_font_pet_16, PET_C_INK);
    lv_obj_set_pos(title, 16, 42);

    lv_obj_t *card = pet_panel(screen, 12, 68, 216, 158, 16);
    for (int i = 0; i < 6; i++) {
        view->rows[i] = pet_label(card, " ", &lv_font_pet_16, PET_C_INK);
        lv_obj_set_pos(view->rows[i], 16, 12 + i * 23);
    }
    pet_fill_status_rows(view, model);

    lv_obj_t *hint = pet_label(screen, "长按确定返回", &lv_font_pet_16, PET_C_MUTED);
    lv_obj_set_pos(hint, 16, 244);

    lv_screen_load(screen);
    return view;
}

void pet_status_view_sync(pet_view_t *view, const pet_model_t *model,
                          const pet_view_battery_t *battery)
{
    if (!view || !model) return;
    pet_hud_sync(view->hud, model, battery);
    pet_fill_status_rows(view, model);
}

void pet_status_view_delete(pet_view_t *view)
{
    if (!view) return;
    if (view->screen) lv_obj_delete(view->screen);
    lv_free(view);
}

/* ------------------------------------------------------------------ 时钟页 */

pet_view_t *pet_clock_view_create(const pet_model_t *model)
{
    if (!model) return NULL;
    pet_view_t *view = lv_malloc(sizeof(pet_view_t));
    if (!view) return NULL;
    memset(view, 0, sizeof(*view));

    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_bg_color(screen, lv_color_hex(PET_C_WALL_LOW), 0);
    view->screen = screen;

    lv_obj_t *title = pet_label(screen, "校准时钟", &lv_font_pet_16, PET_C_INK);
    lv_obj_set_pos(title, 16, 42);

    lv_obj_t *card = pet_panel(screen, 12, 78, 216, 110, 16);
    view->clock_label = pet_label(card, "--:--", &lv_font_montserrat_20, PET_C_INK);
    lv_obj_center(view->clock_label);

    lv_obj_t *hint = pet_label(screen, "上/下调分钟\n长按确定返回", &lv_font_pet_16, PET_C_MUTED);
    lv_obj_set_pos(hint, 16, 202);

    pet_clock_view_sync(view, model);
    lv_screen_load(screen);
    return view;
}

void pet_clock_view_sync(pet_view_t *view, const pet_model_t *model)
{
    if (!view || !view->clock_label) return;
    char text[8];
    pet_model_format_clock(model, text, sizeof(text));
    lv_label_set_text(view->clock_label, text);
}

void pet_clock_view_delete(pet_view_t *view)
{
    if (!view) return;
    if (view->screen) lv_obj_delete(view->screen);
    lv_free(view);
}
