/* 界面主题层实现：色板在 ui_theme.h，这里只放控件样式工厂 */
#include "ui_theme.h"

/* 统一返回按钮事件：把回调指针经 user_data 带进来（与 page_settings.c 同一手法） */
static void back_btn_click(lv_event_t *e)
{
    void (*cb)(void) = (void (*)(void))lv_event_get_user_data(e);
    if (cb) cb();
}

void ui_style_screen(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
}

void ui_style_card(lv_obj_t *obj, bool border)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 8, 0);
    lv_obj_set_style_border_width(obj, border ? 1 : 0, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(C_TRACK), 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);   /* 默认主题的投影在深色底上很脏 */
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

void ui_style_btn(lv_obj_t *btn, uint32_t bg, uint32_t fg)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_text_color(btn, lv_color_hex(fg), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(C_DARK), LV_STATE_PRESSED);
    /* 禁用态：灰底灰字（明确"点不了"，仍然占位，不隐藏） */
    lv_obj_set_style_bg_color(btn, lv_color_hex(C_TRACK), LV_STATE_DISABLED);
    lv_obj_set_style_text_color(btn, lv_color_hex(C_DIM), LV_STATE_DISABLED);
}

void ui_style_textarea(lv_obj_t *ta)
{
    lv_obj_set_style_bg_color(ta, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(C_TRACK), 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_radius(ta, 6, 0);
    lv_obj_set_style_pad_all(ta, 6, 0);
    lv_obj_set_style_shadow_width(ta, 0, 0);
    lv_obj_set_style_text_font(ta, UI_FONT_ASCII, 0);
    /* 光标：深色底上默认样式几乎看不见，给一条蓝色竖线 */
    lv_obj_set_style_border_color(ta, lv_color_hex(C_BLUE), LV_PART_CURSOR);
    lv_obj_set_style_border_width(ta, 2, LV_PART_CURSOR);
}

void ui_style_keyboard(lv_obj_t *kb)
{
    lv_obj_set_style_bg_color(kb, lv_color_hex(C_DARK), 0);   /* 键缝底色 */
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(kb, 2, 0);
    lv_obj_set_style_radius(kb, 0, 0);
    lv_obj_set_style_shadow_width(kb, 0, 0);

    lv_obj_set_style_bg_color(kb, lv_color_hex(C_CARD), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, lv_color_hex(C_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 4, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, lv_color_hex(C_BLUE), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(kb, lv_color_hex(C_OK), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb, lv_color_hex(C_DARK), LV_PART_ITEMS | LV_STATE_CHECKED);
}

/* 下拉框：本体 + 弹出列表都要打。
 * 为什么必须分开打：弹出列表不是下拉框的子对象（lv_dropdown.c 里
 * 把它挂到屏幕下、并 reparent 到最上层），样式不从下拉框继承；
 * 只改本体的话，点开的列表还是默认浅色主题那份"白底黑字"。
 * 行高 = 字体行高 + LV_PART_SELECTED 的 text_line_space，
 * 高亮行底色是 LV_PART_SELECTED 画的矩形 —— 320x240 触屏上
 * 行高给到 ~28px 才好点。 */
void ui_style_dropdown(lv_obj_t *dd)
{
    lv_obj_set_style_bg_color(dd, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(dd, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dd, lv_color_hex(C_TRACK), 0);
    lv_obj_set_style_border_width(dd, 1, 0);
    lv_obj_set_style_radius(dd, 6, 0);
    lv_obj_set_style_pad_left(dd, 8, 0);
    lv_obj_set_style_text_color(dd, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(dd, UI_FONT_CJK, 0);
    lv_obj_set_style_shadow_width(dd, 0, 0);
    lv_obj_set_style_bg_color(dd, lv_color_hex(C_TRACK), LV_PART_INDICATOR);

    lv_obj_t *list = lv_dropdown_get_list(dd);       /* 独立对象，必须单独打 */
    if (list == NULL) return;

    lv_obj_set_style_bg_color(list, lv_color_hex(C_CARD), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(list, lv_color_hex(C_TRACK), LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(list, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list, 6, LV_PART_MAIN);
    lv_obj_set_style_text_color(list, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(list, UI_FONT_CJK, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(list, 0, LV_PART_MAIN);

    lv_obj_set_style_bg_color(list, lv_color_hex(C_BLUE), LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_set_style_radius(list, 6, LV_PART_SELECTED);
    lv_obj_set_style_text_color(list, lv_color_hex(C_DARK), LV_PART_SELECTED);
    lv_obj_set_style_text_line_space(list, 12, LV_PART_SELECTED);
}

void ui_title_bar(lv_obj_t *scr, const char *title,
                  void (*back_cb)(void), const lv_font_t *font)
{
    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 60, 30);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 5, 6);
    ui_style_btn(btn, C_TRACK, C_TEXT);

    lv_obj_t *lab = lv_label_create(btn);          /* 左箭头图标：必须用带图标的字体 */
    lv_label_set_text(lab, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(lab, UI_FONT_ASCII, 0);
    lv_obj_center(lab);
    if (back_cb) lv_obj_add_event_cb(btn, back_btn_click, LV_EVENT_CLICKED, (void *)back_cb);

    lv_obj_t *t = lv_label_create(scr);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, font ? font : UI_FONT_ASCII, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(C_TEXT), 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 12);
}

/* 键值行：左键（暗）+ 右值（亮），透明行容器。
 * 用 lv_obj_remove_style_all 一次性甩掉默认主题给容器的浅底/描边/内边距 ——
 * 这是"关于页背景色很奇怪"的根治办法（旧版用 lv_list_add_text，行底色
 * 来自默认浅色主题，压在深色卡片上就花了）。 */
lv_obj_t *ui_add_kv_row(lv_obj_t *parent, const char *key,
                        const char *value, uint32_t value_color)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 24);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *k = lv_label_create(row);
    lv_label_set_text(k, key);
    lv_obj_set_style_text_font(k, UI_FONT_ASCII, 0);
    lv_obj_set_style_text_color(k, lv_color_hex(C_DIM), 0);
    lv_obj_align(k, LV_ALIGN_LEFT_MID, 10, 0);

    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, value);
    lv_obj_set_style_text_font(v, UI_FONT_ASCII, 0);
    lv_obj_set_style_text_color(v, lv_color_hex(value_color), 0);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, -10, 0);
    return v;
}
