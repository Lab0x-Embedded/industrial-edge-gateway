/* 设置页 · 深色：功能入口中枢（WiFi / 日志 / 关于）
 * 用卡片 + 三个自绘按钮代替 lv_list：条目样式完全可控，
 * 且能把"图标（montserrat 承载）"和"中文标签（CJK 字体承载）"分成两个标签 ——
 * 一个标签里同时放图标和汉字会掉字形（图标字形只在 montserrat 里）。 */
#include "page_settings.h"
#include "page_main.h"
#include "page_log.h"
#include "page_about.h"
#include "page_wifi.h"
#include "ui_theme.h"
#include "tasks.h"

#define ITEM_H   38
#define ITEM_GAP 8

extern lv_obj_t *main_scr;

static lv_obj_t *scr_settings = NULL;

static void btn_back(void)
{
    lv_scr_load(main_scr);
}

/* 条目回调：user_data 直接携带"要打开的页面函数"。
 * 函数指针经 void* 传递在 ISO C 里属灰色地带，但 ARM 32 位下指针等宽，
 * 是 LVGL 嵌入式代码的通用做法——比维护一张索引表干净。 */
static void item_click(lv_event_t *e)
{
    void (*open)(void) = (void (*)(void))lv_event_get_user_data(e);
    open();
}

static void add_item(lv_obj_t *parent, const char *icon, const char *text, void (*open_fn)(void))
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, LV_PCT(100), ITEM_H);
    ui_style_btn(btn, C_TRACK, C_TEXT);

    lv_obj_t *li = lv_label_create(btn);
    lv_label_set_text(li, icon);
    lv_obj_set_style_text_font(li, UI_FONT_ASCII, 0);
    lv_obj_set_style_text_color(li, lv_color_hex(C_BLUE), 0);
    lv_obj_align(li, LV_ALIGN_LEFT_MID, 12, 0);

    lv_obj_t *lt = lv_label_create(btn);
    lv_label_set_text(lt, text);
    lv_obj_set_style_text_font(lt, UI_FONT_CJK, 0);
    lv_obj_set_style_text_color(lt, lv_color_hex(C_TEXT), 0);
    lv_obj_align(lt, LV_ALIGN_LEFT_MID, 40, 0);

    lv_obj_add_event_cb(btn, item_click, LV_EVENT_CLICKED, (void *)open_fn);
}

void page_settings_open(void)
{
    if (scr_settings == NULL)
    {
        scr_settings = lv_obj_create(NULL);
        ui_style_screen(scr_settings);
        ui_title_bar(scr_settings, "设置", btn_back, UI_FONT_CJK);

        lv_obj_t *card = lv_obj_create(scr_settings);
        lv_obj_set_size(card, 300, (ITEM_H + ITEM_GAP) * 3 + ITEM_GAP);
        lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 44);
        ui_style_card(card, true);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(card, ITEM_GAP, 0);
        lv_obj_set_style_pad_row(card, ITEM_GAP, 0);

        add_item(card, LV_SYMBOL_WIFI, "WiFi 配置",    page_wifi_open);
        add_item(card, LV_SYMBOL_LIST, "系统运行日志", page_log_open);
        add_item(card, LV_SYMBOL_OK,   "关于设备",     page_about_open);
    }
    lv_scr_load(scr_settings);
}
