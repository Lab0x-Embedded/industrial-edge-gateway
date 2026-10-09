/* 设置页：功能列表中枢（WiFi 配置 / 系统日志 / 关于设备） */
#include "page_settings.h"
#include "page_main.h"
#include "page_log.h"
#include "page_about.h"
#include "page_wifi.h"
#include "tasks.h"

extern lv_obj_t *main_scr;

static lv_obj_t *scr_settings = NULL;

static void btn_back_click(lv_event_t *e)
{
    (void)e;
    lv_scr_load(main_scr);
}

static void item_wifi_click(lv_event_t *e)
{
    (void)e;
    page_wifi_open();
}

static void item_log_click(lv_event_t *e)
{
    (void)e;
    page_log_open();
}

static void item_about_click(lv_event_t *e)
{
    (void)e;
    page_about_open();
}

static lv_obj_t *add_item(lv_obj_t *list, const char *icon, const char *text,
                          lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_list_add_btn(list, icon, text);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

void page_settings_open(void)
{
    if (scr_settings == NULL)
    {
        scr_settings = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(scr_settings, lv_color_hex(0xF8F9FA), 0);
        lv_obj_clear_flag(scr_settings, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *btn_back = lv_btn_create(scr_settings);
        lv_obj_set_size(btn_back, 65, 35);
        lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 5, 5);
        lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x0F3460), 0);

        lv_obj_t *lab_back = lv_label_create(btn_back);
        lv_label_set_text(lab_back, LV_SYMBOL_LEFT " Back");
        lv_obj_align(lab_back, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_text_color(lab_back, lv_color_white(), 0);
        lv_obj_add_event_cb(btn_back, btn_back_click, LV_EVENT_CLICKED, NULL);

        lv_obj_t *title = lv_label_create(scr_settings);
        lv_label_set_text(title, LV_SYMBOL_SETTINGS " Settings");
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

        lv_obj_t *list = lv_list_create(scr_settings);
        lv_obj_set_size(list, 300, 150);
        lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, -10);
        lv_obj_set_style_radius(list, 6, 0);

        add_item(list, LV_SYMBOL_WIFI,  "WiFi Config", item_wifi_click);
        add_item(list, LV_SYMBOL_LIST,  "System Log", item_log_click);
        add_item(list, LV_SYMBOL_OK,    "About",      item_about_click);
    }
    lv_scr_load(scr_settings);
}
