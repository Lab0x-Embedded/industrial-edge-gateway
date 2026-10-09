/* WiFi 配置页：SSID/密码输入（内置键盘）→ 存 W25Q → 通知 AT 任务全量重连 */
#include "page_wifi.h"
#include "page_settings.h"
#include "wifi_conn.h"
#include "tasks.h"          /* g_mqtt_connected */

static lv_obj_t *scr_wifi = NULL;
static lv_obj_t *ta_ssid, *ta_pass, *btn_conn, *label_st;
static lv_timer_t *wifi_timer = NULL;

static void btn_back_click(lv_event_t *e)
{
    (void)e;
    page_settings_open();
}

/* 输入框获得焦点 → 键盘跟随 */
static void ta_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    lv_obj_t *kb = lv_event_get_user_data(e);
    lv_keyboard_set_textarea(kb, ta);
}

static void btn_connect_click(lv_event_t *e)
{
    (void)e;
    const char *ssid = lv_textarea_get_text(ta_ssid);
    const char *pass = lv_textarea_get_text(ta_pass);

    if (wifi_conn_save(ssid, pass) != 0)
    {
        lv_label_set_text(label_st, "Save FAIL");
        lv_obj_set_style_text_color(label_st, lv_color_hex(0xFF6B6B), 0);
        return;
    }
    wifi_reconnect_request = 1;      /* AT 任务 ≤1s 内响应，用新凭据全量重连 */
}

/* 500ms 轮询连接状态（仅本页显示时刷新） */
static void wifi_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (lv_scr_act() != scr_wifi) return;

    if (wifi_reconnect_request)
    {
        lv_label_set_text(label_st, "Connecting...");
        lv_obj_set_style_text_color(label_st, lv_color_hex(0x4A90E2), 0);
        lv_obj_add_state(btn_conn, LV_STATE_DISABLED);
    }
    else if (!wifi_connected)
    {
        lv_label_set_text(label_st, "WiFi FAIL");
        lv_obj_set_style_text_color(label_st, lv_color_hex(0xFF6B6B), 0);
        lv_obj_clear_state(btn_conn, LV_STATE_DISABLED);
    }
    else if (!g_mqtt_connected)
    {
        lv_label_set_text(label_st, "NO CLOUD");
        lv_obj_set_style_text_color(label_st, lv_color_hex(0xF6AD55), 0);
        lv_obj_clear_state(btn_conn, LV_STATE_DISABLED);
    }
    else
    {
        lv_label_set_text(label_st, "WiFi OK");
        lv_obj_set_style_text_color(label_st, lv_color_hex(0x2ECC71), 0);
        lv_obj_clear_state(btn_conn, LV_STATE_DISABLED);
    }
}

void page_wifi_open(void)
{
    if (scr_wifi == NULL)
    {
        char ssid[33], pass[65];
        scr_wifi = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(scr_wifi, lv_color_hex(0xF8F9FA), 0);
        lv_obj_clear_flag(scr_wifi, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *btn_back = lv_btn_create(scr_wifi);
        lv_obj_set_size(btn_back, 65, 35);
        lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 5, 5);
        lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x0F3460), 0);

        lv_obj_t *lab_back = lv_label_create(btn_back);
        lv_label_set_text(lab_back, LV_SYMBOL_LEFT " Back");
        lv_obj_align(lab_back, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_text_color(lab_back, lv_color_white(), 0);
        lv_obj_add_event_cb(btn_back, btn_back_click, LV_EVENT_CLICKED, NULL);

        lv_obj_t *title = lv_label_create(scr_wifi);
        lv_label_set_text(title, LV_SYMBOL_WIFI " WiFi");
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

        /* 键盘常驻底部（压缩高度），上区放输入与操作 */
        lv_obj_t *kb = lv_keyboard_create(scr_wifi);
        lv_obj_set_size(kb, 320, 92);
        lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);

        ta_ssid = lv_textarea_create(scr_wifi);
        lv_obj_set_size(ta_ssid, 195, 34);
        lv_obj_align(ta_ssid, LV_ALIGN_TOP_LEFT, 5, 45);
        lv_textarea_set_one_line(ta_ssid, true);
        lv_textarea_set_max_length(ta_ssid, 32);
        lv_textarea_set_placeholder_text(ta_ssid, "SSID");
        lv_obj_add_event_cb(ta_ssid, ta_focus_cb, LV_EVENT_FOCUSED, kb);

        ta_pass = lv_textarea_create(scr_wifi);
        lv_obj_set_size(ta_pass, 195, 34);
        lv_obj_align(ta_pass, LV_ALIGN_TOP_LEFT, 5, 84);
        lv_textarea_set_one_line(ta_pass, true);
        lv_textarea_set_password_mode(ta_pass, true);
        lv_textarea_set_max_length(ta_pass, 64);
        lv_textarea_set_placeholder_text(ta_pass, "Password");
        lv_obj_add_event_cb(ta_pass, ta_focus_cb, LV_EVENT_FOCUSED, kb);

        btn_conn = lv_btn_create(scr_wifi);
        lv_obj_set_size(btn_conn, 105, 34);
        lv_obj_align(btn_conn, LV_ALIGN_TOP_RIGHT, -5, 45);
        lv_obj_set_style_bg_color(btn_conn, lv_color_hex(0x4A90E2), 0);

        lv_obj_t *lab_conn = lv_label_create(btn_conn);
        lv_label_set_text(lab_conn, "Connect");
        lv_obj_align(lab_conn, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_text_color(lab_conn, lv_color_white(), 0);
        lv_obj_add_event_cb(btn_conn, btn_connect_click, LV_EVENT_CLICKED, NULL);

        label_st = lv_label_create(scr_wifi);
        lv_obj_set_width(label_st, 105);
        lv_label_set_long_mode(label_st, LV_LABEL_LONG_WRAP);
        lv_obj_align(label_st, LV_ALIGN_TOP_RIGHT, -5, 88);
        lv_label_set_text(label_st, "--");

        /* 预填当前凭据（W25Q 存档或编译期默认） */
        wifi_conn_get(ssid, sizeof ssid, pass, sizeof pass);
        lv_textarea_set_text(ta_ssid, ssid);
        lv_textarea_set_text(ta_pass, pass);

        wifi_timer = lv_timer_create(wifi_timer_cb, 500, NULL);
    }
    lv_scr_load(scr_wifi);
}
