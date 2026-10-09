/* WiFi 配置页 · 深色：SSID 扫描下拉 + 密码键盘输入 → 存 W25Q → AT 任务全量重连 */
#include "page_wifi.h"
#include "page_settings.h"
#include "wifi_conn.h"
#include "tasks.h"
#include <stdio.h>

#define C_BG     0x0F172A
#define C_CARD   0x1E293B
#define C_TRACK  0x334155
#define C_TEXT   0xE2E8F0
#define C_DIM    0x94A3B8
#define C_BLUE   0x60A5FA
#define C_OK     0x34D399
#define C_FAIL   0xF87171
#define C_WARN   0xFBBF24

static lv_obj_t *scr_wifi = NULL;
static lv_obj_t *ta_ssid, *ta_pass, *btn_conn, *btn_scan, *label_st, *dd_ssid, *kb;
static lv_timer_t *wifi_timer = NULL;

static void btn_back_click(lv_event_t *e)
{
    (void)e;
    page_settings_open();
}

static void ta_focus_cb(lv_event_t *e)
{
    lv_keyboard_set_textarea(kb, lv_event_get_target(e));
}

static void btn_connect_click(lv_event_t *e)
{
    (void)e;
    const char *ssid = lv_textarea_get_text(ta_ssid);
    const char *pass = lv_textarea_get_text(ta_pass);

    if (wifi_conn_save(ssid, pass) != 0)
    {
        lv_label_set_text(label_st, "Save FAIL");
        lv_obj_set_style_text_color(label_st, lv_color_hex(C_FAIL), 0);
        return;
    }
    wifi_reconnect_request = 1;      /* AT 任务 ≤1s 响应；标志在重连尝试完整结束才清 */
}

/* 下拉选择热点 → 填入 SSID 输入框 */
static void dd_ssid_cb(lv_event_t *e)
{
    char buf[48];
    lv_dropdown_get_selected_str(lv_event_get_target(e), buf, sizeof(buf));
    lv_textarea_set_text(ta_ssid, buf);
}

/* 扫描可用热点：阻塞最长 ~10s（期间界面冻结，先强制刷新出 Scanning 状态） */
static void btn_scan_click(lv_event_t *e)
{
    (void)e;
    static char ssids[12][33];
    static char opts[12 * 34 + 1];
    int n, i, off = 0;

    lv_obj_add_state(btn_scan, LV_STATE_DISABLED);
    lv_label_set_text(label_st, "Scanning...");
    lv_obj_set_style_text_color(label_st, lv_color_hex(C_BLUE), 0);
    lv_refr_now(NULL);                               /* 阻塞前把界面刷出去 */

    n = wifi_scan(ssids, 12);
    if (n < 0)
    {
        lv_label_set_text(label_st, "Scan FAIL");
        lv_obj_set_style_text_color(label_st, lv_color_hex(C_FAIL), 0);
    }
    else if (n == 0)
    {
        lv_label_set_text(label_st, "No AP");
        lv_obj_set_style_text_color(label_st, lv_color_hex(C_WARN), 0);
    }
    else
    {
        for (i = 0; i < n; i++)
            off += snprintf(opts + off, sizeof(opts) - off, "%s\n", ssids[i]);
        if (off > 0) opts[off - 1] = 0;              /* 去掉末尾换行 */
        lv_dropdown_set_options(dd_ssid, opts);
        lv_label_set_text(label_st, "Found");
        lv_obj_set_style_text_color(label_st, lv_color_hex(C_OK), 0);
    }
    lv_obj_clear_state(btn_scan, LV_STATE_DISABLED);
}

/* 500ms 轮询：Connecting 贯穿整个重连尝试（请求标志由 AT 任务在尝试结束时清除） */
static void wifi_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (lv_scr_act() != scr_wifi) return;

    if (wifi_reconnect_request)
    {
        lv_label_set_text(label_st, "Connecting...");
        lv_obj_set_style_text_color(label_st, lv_color_hex(C_BLUE), 0);
        lv_obj_add_state(btn_conn, LV_STATE_DISABLED);
    }
    else if (!wifi_connected)
    {
        lv_label_set_text(label_st, "WiFi FAIL");
        lv_obj_set_style_text_color(label_st, lv_color_hex(C_FAIL), 0);
        lv_obj_clear_state(btn_conn, LV_STATE_DISABLED);
    }
    else if (!g_mqtt_connected)
    {
        lv_label_set_text(label_st, "NO CLOUD");
        lv_obj_set_style_text_color(label_st, lv_color_hex(C_WARN), 0);
        lv_obj_clear_state(btn_conn, LV_STATE_DISABLED);
    }
    else
    {
        lv_label_set_text(label_st, "WiFi OK");
        lv_obj_set_style_text_color(label_st, lv_color_hex(C_OK), 0);
        lv_obj_clear_state(btn_conn, LV_STATE_DISABLED);
    }
}

static void style_ta(lv_obj_t *ta)
{
    lv_obj_set_style_bg_color(ta, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(C_TRACK), 0);
}

void page_wifi_open(void)
{
    if (scr_wifi == NULL)
    {
        char ssid[33], pass[65];
        scr_wifi = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(scr_wifi, lv_color_hex(C_BG), 0);
        lv_obj_clear_flag(scr_wifi, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *btn_back = lv_btn_create(scr_wifi);
        lv_obj_set_size(btn_back, 60, 30);
        lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 5, 6);
        lv_obj_set_style_bg_color(btn_back, lv_color_hex(C_TRACK), 0);
        lv_obj_set_style_radius(btn_back, 6, 0);

        lv_obj_t *lab_back = lv_label_create(btn_back);
        lv_label_set_text(lab_back, LV_SYMBOL_LEFT);
        lv_obj_align(lab_back, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_text_color(lab_back, lv_color_hex(C_TEXT), 0);
        lv_obj_add_event_cb(btn_back, btn_back_click, LV_EVENT_CLICKED, NULL);

        lv_obj_t *title = lv_label_create(scr_wifi);
        lv_label_set_text(title, LV_SYMBOL_WIFI " WiFi");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

        /* 第一行：SSID 输入 + Scan */
        ta_ssid = lv_textarea_create(scr_wifi);
        lv_obj_set_size(ta_ssid, 150, 30);
        lv_obj_align(ta_ssid, LV_ALIGN_TOP_LEFT, 5, 42);
        lv_textarea_set_one_line(ta_ssid, true);
        lv_textarea_set_max_length(ta_ssid, 32);
        lv_textarea_set_placeholder_text(ta_ssid, "SSID");
        style_ta(ta_ssid);
        lv_obj_add_event_cb(ta_ssid, ta_focus_cb, LV_EVENT_FOCUSED, NULL);

        btn_scan = lv_btn_create(scr_wifi);
        lv_obj_set_size(btn_scan, 72, 30);
        lv_obj_align(btn_scan, LV_ALIGN_TOP_LEFT, 160, 42);
        lv_obj_set_style_bg_color(btn_scan, lv_color_hex(C_TRACK), 0);
        lv_obj_set_style_radius(btn_scan, 6, 0);
        lv_obj_t *lab_scan = lv_label_create(btn_scan);
        lv_label_set_text(lab_scan, LV_SYMBOL_REFRESH " Scan");
        lv_obj_align(lab_scan, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_text_color(lab_scan, lv_color_hex(C_TEXT), 0);
        lv_obj_add_event_cb(btn_scan, btn_scan_click, LV_EVENT_CLICKED, NULL);

        /* 第二行：密码输入 + Connect */
        ta_pass = lv_textarea_create(scr_wifi);
        lv_obj_set_size(ta_pass, 150, 30);
        lv_obj_align(ta_pass, LV_ALIGN_TOP_LEFT, 5, 78);
        lv_textarea_set_one_line(ta_pass, true);
        lv_textarea_set_password_mode(ta_pass, true);
        lv_textarea_set_max_length(ta_pass, 64);
        lv_textarea_set_placeholder_text(ta_pass, "Password");
        style_ta(ta_pass);
        lv_obj_add_event_cb(ta_pass, ta_focus_cb, LV_EVENT_FOCUSED, NULL);

        btn_conn = lv_btn_create(scr_wifi);
        lv_obj_set_size(btn_conn, 72, 30);
        lv_obj_align(btn_conn, LV_ALIGN_TOP_LEFT, 160, 78);
        lv_obj_set_style_bg_color(btn_conn, lv_color_hex(C_BLUE), 0);
        lv_obj_set_style_radius(btn_conn, 6, 0);
        lv_obj_t *lab_conn = lv_label_create(btn_conn);
        lv_label_set_text(lab_conn, "Connect");
        lv_obj_align(lab_conn, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_event_cb(btn_conn, btn_connect_click, LV_EVENT_CLICKED, NULL);

        /* 状态 + 扫描结果下拉 */
        label_st = lv_label_create(scr_wifi);
        lv_obj_set_size(label_st, 78, 30);
        lv_label_set_long_mode(label_st, LV_LABEL_LONG_WRAP);
        lv_obj_align(label_st, LV_ALIGN_TOP_RIGHT, -5, 58);
        lv_obj_set_style_text_color(label_st, lv_color_hex(C_DIM), 0);
        lv_label_set_text(label_st, "--");

        dd_ssid = lv_dropdown_create(scr_wifi);
        lv_obj_set_size(dd_ssid, 310, 28);
        lv_obj_align(dd_ssid, LV_ALIGN_TOP_MID, 0, 114);
        lv_dropdown_set_options(dd_ssid, "Select AP...");
        lv_obj_set_style_bg_color(dd_ssid, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_text_color(dd_ssid, lv_color_hex(C_TEXT), 0);
        lv_obj_add_event_cb(dd_ssid, dd_ssid_cb, LV_EVENT_VALUE_CHANGED, NULL);

        /* 底部键盘（深色） */
        kb = lv_keyboard_create(scr_wifi);
        lv_obj_set_size(kb, 320, 92);
        lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(kb, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_bg_color(kb, lv_color_hex(C_TRACK), LV_PART_ITEMS);
        lv_obj_set_style_text_color(kb, lv_color_hex(C_TEXT), LV_PART_ITEMS);

        /* 预填当前凭据（W25Q 存档或编译期默认） */
        wifi_conn_get(ssid, sizeof ssid, pass, sizeof pass);
        lv_textarea_set_text(ta_ssid, ssid);
        lv_textarea_set_text(ta_pass, pass);

        wifi_timer = lv_timer_create(wifi_timer_cb, 500, NULL);
    }
    lv_scr_load(scr_wifi);
}
