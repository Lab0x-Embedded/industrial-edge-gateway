/* 设置页 · 深色：功能列表中枢（WiFi / 日志 / 关于） */
#include "page_settings.h"
#include "page_main.h"
#include "page_log.h"
#include "page_about.h"
#include "page_wifi.h"
#include "tasks.h"

#define C_BG     0x0F172A
#define C_CARD   0x1E293B
#define C_TRACK  0x334155
#define C_TEXT   0xE2E8F0

extern lv_obj_t *main_scr;

static lv_obj_t *scr_settings = NULL;

static void btn_back_click(lv_event_t *e)
{
    (void)e;
    lv_scr_load(main_scr);
}

/* 回调直传打开函数（函数指针经 user_data 传递，ARM 32 位下安全） */
static void item_click(lv_event_t *e)
{
    void (*open)(void) = (void (*)(void))lv_event_get_user_data(e);
    open();
}

static void add_item(lv_obj_t *list, const char *icon, const char *text, void (*open_fn)(void))
{
    lv_obj_t *btn = lv_list_add_btn(list, icon, text);
    lv_obj_add_event_cb(btn, item_click, LV_EVENT_CLICKED, (void *)open_fn);
    lv_obj_set_style_bg_color(btn, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_text_color(btn, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(C_TRACK), 0);
}

static void title_bar(lv_obj_t *scr, const char *title)
{
    lv_obj_t *btn_back = lv_btn_create(scr);
    lv_obj_set_size(btn_back, 60, 30);
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 5, 6);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(C_TRACK), 0);
    lv_obj_set_style_radius(btn_back, 6, 0);

    lv_obj_t *lab_back = lv_label_create(btn_back);
    lv_label_set_text(lab_back, LV_SYMBOL_LEFT);
    lv_obj_align(lab_back, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(lab_back, lv_color_hex(C_TEXT), 0);
    lv_obj_add_event_cb(btn_back, btn_back_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *t = lv_label_create(scr);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(C_TEXT), 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 12);
}

void page_settings_open(void)
{
    if (scr_settings == NULL)
    {
        scr_settings = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(scr_settings, lv_color_hex(C_BG), 0);
        lv_obj_clear_flag(scr_settings, LV_OBJ_FLAG_SCROLLABLE);
        title_bar(scr_settings, LV_SYMBOL_SETTINGS " Settings");

        lv_obj_t *list = lv_list_create(scr_settings);
        lv_obj_set_size(list, 300, 150);
        lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, -8);
        lv_obj_set_style_bg_color(list, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_border_color(list, lv_color_hex(C_TRACK), 0);
        lv_obj_set_style_radius(list, 8, 0);

        add_item(list, LV_SYMBOL_WIFI, "WiFi Config",  page_wifi_open);
        add_item(list, LV_SYMBOL_LIST, "System Log",   page_log_open);
        add_item(list, LV_SYMBOL_OK,   "About Device", page_about_open);
    }
    lv_scr_load(scr_settings);
}
