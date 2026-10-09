/* 设置页 · 深色：功能入口中枢（WiFi / 日志 / 校时 / 关于）
 * 用卡片 + 自绘按钮代替 lv_list：条目样式完全可控，
 * 且能把"图标（montserrat 承载）"和"中文标签（CJK 字体承载）"分成两个标签 ——
 * 一个标签里同时放图标和汉字会掉字形（图标字形只在 montserrat 里）。 */
#include "page_settings.h"
#include "page_main.h"
#include "page_log.h"
#include "page_about.h"
#include "page_wifi.h"
#include "wifi_conn.h"
#include "ui_theme.h"
#include "tasks.h"
#include "stm32f4xx.h"

#define ITEM_H   38
#define ITEM_GAP 8

extern lv_obj_t *main_scr;

static lv_obj_t *scr_settings = NULL;
static lv_obj_t *label_sync   = NULL;   /* "同步当前时间"条目的文字标签 */
static uint8_t   sync_ui      = 0;      /* 0=待机 1=等结果 2=结果展示中 */
static uint32_t  sync_ui_tick = 0;

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

/* "同步当前时间"：UI 只置请求标志，AT 任务在持锁语境里跑校时（并发约定见
 * wifi_conn.h）。成败由本条目文字直接给反馈：校时中... → 校时成功/校时失败。 */
static void timesync_click(lv_event_t *e)
{
    (void)e;
    if (sync_ui == 1) return;               /* 上一轮还在等结果，别重复下发 */
    wifi_timesync_result = 0;
    wifi_timesync_request  = 1;
    sync_ui = 1;
    lv_label_set_text(label_sync, "校时中...");
    lv_obj_set_style_text_color(label_sync, lv_color_hex(C_BLUE), 0);
}

/* 300ms 轮询：等 AT 任务把 wifi_timesync_result 写好 → 显示结果 4s → 回待机文案 */
static void settings_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (lv_scr_act() != scr_settings || label_sync == NULL) return;

    if (sync_ui == 1)
    {
        if (wifi_timesync_result == 1) {
            sync_ui = 2; sync_ui_tick = HAL_GetTick();
            lv_label_set_text(label_sync, "校时成功");
            lv_obj_set_style_text_color(label_sync, lv_color_hex(C_OK), 0);
        } else if (wifi_timesync_result == 2) {
            sync_ui = 2; sync_ui_tick = HAL_GetTick();
            lv_label_set_text(label_sync, "校时失败");
            lv_obj_set_style_text_color(label_sync, lv_color_hex(C_FAIL), 0);
        }
    }
    else if (sync_ui == 2 && (HAL_GetTick() - sync_ui_tick) > 4000)
    {
        sync_ui = 0;
        lv_label_set_text(label_sync, "同步当前时间");
        lv_obj_set_style_text_color(label_sync, lv_color_hex(C_TEXT), 0);
    }
}

/* 返回条目的文字标签（调用方可留着改文案，比如校时反馈） */
static lv_obj_t *add_item(lv_obj_t *parent, const char *icon, const char *text,
                          lv_event_cb_t cb, void *user_data)
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

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    return lt;
}

void page_settings_open(void)
{
    if (scr_settings == NULL)
    {
        scr_settings = lv_obj_create(NULL);
        ui_style_screen(scr_settings);
        ui_title_bar(scr_settings, "设置", btn_back, UI_FONT_CJK);

        lv_obj_t *card = lv_obj_create(scr_settings);
        lv_obj_set_size(card, 300, (ITEM_H + ITEM_GAP) * 4 + ITEM_GAP);
        lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 44);
        ui_style_card(card, true);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(card, ITEM_GAP, 0);
        lv_obj_set_style_pad_row(card, ITEM_GAP, 0);

        /* 顺序约定："关于设备"必须压轴（版本/信息类条目放最后），校时放日志后 */
        add_item(card, LV_SYMBOL_WIFI,    "WiFi 配置",    item_click, page_wifi_open);
        add_item(card, LV_SYMBOL_LIST,    "系统运行日志", item_click, page_log_open);
        label_sync = add_item(card, LV_SYMBOL_REFRESH, "同步当前时间", timesync_click, NULL);
        add_item(card, LV_SYMBOL_OK,      "关于设备",     item_click, page_about_open);

        settings_timer_cb(NULL);                        /* 先刷一次，避免首帧残留旧文案 */
        lv_timer_create(settings_timer_cb, 300, NULL);
    }
    lv_scr_load(scr_settings);
}
