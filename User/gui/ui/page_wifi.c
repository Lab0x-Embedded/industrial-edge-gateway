/* WiFi 配置页 · 深色
 * 交互定稿（用户反馈"一个下拉框一个输入框"的设计很怪）：
 *   默认只显示一个「选择热点」下拉选择器 —— 扫描结果直接进下拉，点开即选；
 *   要手打 SSID 时点「手动输入」，第 1 行原地换成输入框，再点「选择热点」换回。
 *   扫到的第一个选项是占位行「选择热点」，不会被当成热点连（回调里忽略 index 0）。
 * 扫描走异步：UI 只置 wifi_scan_request，~10s 的阻塞扫描在 AT 任务里做，
 * 界面不会卡住（旧版在点击回调里同步扫，界面冻 ~10s）。
 */
#include "page_wifi.h"
#include "page_settings.h"
#include "ui_theme.h"
#include "wifi_conn.h"
#include "tasks.h"
#include <stdio.h>

#define OPT_PLACEHOLDER  "选择热点"

static lv_obj_t *scr_wifi = NULL;
static lv_obj_t *ta_ssid, *ta_pass, *btn_conn, *btn_scan, *btn_mode, *lab_mode;
static lv_obj_t *label_st, *dd_ssid, *kb;
static lv_timer_t *wifi_timer = NULL;
static uint8_t mode_list = 1;              /* 1=下拉选热点  0=手打 SSID */
static uint8_t last_scan_state = WIFI_SCAN_IDLE;

static void btn_back(void)
{
    page_settings_open();
}

static void ta_focus_cb(lv_event_t *e)
{
    lv_keyboard_set_textarea(kb, lv_event_get_target(e));
}

static void set_status(const char *txt, uint32_t color)
{
    lv_label_set_text(label_st, txt);
    lv_obj_set_style_text_color(label_st, lv_color_hex(color), 0);
}

/* 列表模式 ⇄ 自定义输入模式：同一行两个控件只显示一个，不换版面 */
static void set_mode(uint8_t list_mode)
{
    mode_list = list_mode;
    if (list_mode) {
        lv_obj_add_flag(ta_ssid, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(dd_ssid, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lab_mode, "手动输入");
        lv_keyboard_set_textarea(kb, ta_pass);
    } else {
        lv_obj_add_flag(dd_ssid, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ta_ssid, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lab_mode, "选择热点");
        lv_keyboard_set_textarea(kb, ta_ssid);
    }
}

static void btn_mode_click(lv_event_t *e)
{
    (void)e;
    set_mode(!mode_list);
}

/* 下拉选中热点 → 填入内部 SSID 变量（密码仍需手输，热点列表里拿不到密码） */
static void dd_ssid_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    if (lv_dropdown_get_selected(dd) == 0) return;   /* index 0 是占位行 */
    char buf[48];
    lv_dropdown_get_selected_str(dd, buf, sizeof buf);
    lv_textarea_set_text(ta_ssid, buf);
}

/* 发起扫描：只置标志，真正的串口操作在 AT 任务里（界面不阻塞） */
static void start_scan(void)
{
    lv_obj_set_style_text_color(dd_ssid, lv_color_hex(C_TEXT), 0);
    lv_dropdown_set_options(dd_ssid, "扫描...");
    lv_dropdown_set_selected(dd_ssid, 0);
    last_scan_state = WIFI_SCAN_RUNNING;
    wifi_scan_state = WIFI_SCAN_RUNNING;
    wifi_scan_request = 1;
}

static void btn_scan_click(lv_event_t *e)
{
    (void)e;
    if (!mode_list) set_mode(1);   /* 自定义模式下点刷新 = 回到列表并重扫 */
    start_scan();
}

/* 扫描完成：把结果拼成下拉选项（LVGL 选项即换行分隔），首行是占位行 */
static void fill_ap_options(void)
{
    static char opts[WIFI_AP_MAX * 34 + 16];
    int i, off = 0;

    off += snprintf(opts + off, sizeof(opts) - off, "%s\n", OPT_PLACEHOLDER);
    for (i = 0; i < wifi_ap_count && i < WIFI_AP_MAX; i++)
        off += snprintf(opts + off, sizeof(opts) - off, "%s\n", wifi_ap_list[i]);
    if (off > 0) opts[off - 1] = 0;               /* 去掉末尾换行 */

    lv_dropdown_set_options(dd_ssid, opts);
    lv_dropdown_set_selected(dd_ssid, 0);
    lv_obj_set_style_text_color(dd_ssid, lv_color_hex(C_TEXT), 0);
}

static void btn_connect_click(lv_event_t *e)
{
    (void)e;
    const char *ssid = lv_textarea_get_text(ta_ssid);
    const char *pass = lv_textarea_get_text(ta_pass);

    if (ssid[0] == 0) {                      /* 没选热点也没手打 SSID */
        set_status("NO SSID", C_FAIL);
        return;
    }
    if (wifi_conn_save(ssid, pass) != 0) {
        set_status("Save FAIL", C_FAIL);
        return;
    }
    wifi_reconnect_request = 1;      /* AT 任务 ≤1s 响应；标志在重连尝试完整结束才清 */
}

/* 500ms 状态轮询（仅本页可见时刷新）
 * 1) 扫描状态 → 下拉框文案（下拉自己就是扫描进度指示）
 * 2) 连接状态四态，优先级：Connecting > WiFi FAIL > NO CLOUD > WiFi OK。
 *    Connecting 能贯穿全程的原因：wifi_reconnect_request 从 UI 置 1 起，
 *    直到 AT 任务把"退网→连新 WiFi→TCP→MQTT"整个重连流程跑完才清 0。 */
static void wifi_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (lv_scr_act() != scr_wifi) return;

    if (wifi_scan_state != last_scan_state)
    {
        last_scan_state = wifi_scan_state;
        if (wifi_scan_state == WIFI_SCAN_DONE)
        {
            fill_ap_options();
            if (!mode_list) set_mode(1);      /* 手打模式下重扫成功 → 回到列表 */
        }
        else if (wifi_scan_state == WIFI_SCAN_FAIL)
        {
            lv_dropdown_set_options(dd_ssid, "扫描失败");
            lv_dropdown_set_selected(dd_ssid, 0);
            lv_obj_set_style_text_color(dd_ssid, lv_color_hex(C_WARN), 0);
        }
    }

    if (wifi_reconnect_request)
    {
        set_status("Connecting...", C_BLUE);
        lv_obj_add_state(btn_conn, LV_STATE_DISABLED);
    }
    else if (!wifi_connected)
    {
        set_status("WiFi FAIL", C_FAIL);
        lv_obj_clear_state(btn_conn, LV_STATE_DISABLED);
    }
    else if (!g_mqtt_connected)
    {
        set_status("NO CLOUD", C_WARN);
        lv_obj_clear_state(btn_conn, LV_STATE_DISABLED);
    }
    else
    {
        set_status("WiFi OK", C_OK);
        lv_obj_clear_state(btn_conn, LV_STATE_DISABLED);
    }
}

/* 小工具：造一个带中文字体的按钮 */
static lv_obj_t *make_btn(lv_obj_t *parent, int w, int h, int x, int y,
                          uint32_t bg, const char *text)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, x, y);
    ui_style_btn(btn, bg, C_TEXT);
    lv_obj_t *lab = lv_label_create(btn);
    lv_label_set_text(lab, text);
    lv_obj_set_style_text_font(lab, UI_FONT_CJK, 0);
    lv_obj_center(lab);
    return btn;
}

void page_wifi_open(void)
{
    if (scr_wifi == NULL)
    {
        char ssid[33], pass[65];

        scr_wifi = lv_obj_create(NULL);
        ui_style_screen(scr_wifi);
        ui_title_bar(scr_wifi, "WiFi 配置", btn_back, UI_FONT_CJK);

        /* ---- 第 1 行：热点选择器（下拉）⇄ 自定义 SSID 输入框 + 刷新 ---- */
        dd_ssid = lv_dropdown_create(scr_wifi);
        lv_obj_set_size(dd_ssid, 198, 30);
        lv_obj_align(dd_ssid, LV_ALIGN_TOP_LEFT, 6, 42);
        ui_style_dropdown(dd_ssid);
        lv_dropdown_set_options(dd_ssid, OPT_PLACEHOLDER);
        lv_dropdown_set_selected_highlight(dd_ssid, true);
        lv_obj_add_event_cb(dd_ssid, dd_ssid_cb, LV_EVENT_VALUE_CHANGED, NULL);

        ta_ssid = lv_textarea_create(scr_wifi);
        lv_obj_set_size(ta_ssid, 198, 30);
        lv_obj_align(ta_ssid, LV_ALIGN_TOP_LEFT, 6, 42);
        lv_textarea_set_one_line(ta_ssid, true);
        lv_textarea_set_max_length(ta_ssid, 32);
        lv_textarea_set_placeholder_text(ta_ssid, "SSID");
        ui_style_textarea(ta_ssid);
        lv_obj_add_event_cb(ta_ssid, ta_focus_cb, LV_EVENT_FOCUSED, NULL);
        lv_obj_add_flag(ta_ssid, LV_OBJ_FLAG_HIDDEN);   /* 默认手打框不出现 */

        btn_scan = make_btn(scr_wifi, 104, 30, 210, 42, C_TRACK, "刷新");
        lv_obj_add_event_cb(btn_scan, btn_scan_click, LV_EVENT_CLICKED, NULL);

        /* ---- 第 2 行：密码 + 手动输入/选择热点 切换 ---- */
        ta_pass = lv_textarea_create(scr_wifi);
        lv_obj_set_size(ta_pass, 198, 30);
        lv_obj_align(ta_pass, LV_ALIGN_TOP_LEFT, 6, 76);
        lv_textarea_set_one_line(ta_pass, true);
        lv_textarea_set_password_mode(ta_pass, true);
        lv_textarea_set_max_length(ta_pass, 64);
        lv_textarea_set_placeholder_text(ta_pass, "密码");
        ui_style_textarea(ta_pass);
        lv_obj_add_event_cb(ta_pass, ta_focus_cb, LV_EVENT_FOCUSED, NULL);

        btn_mode = make_btn(scr_wifi, 104, 30, 210, 76, C_TRACK, "手动输入");
        lab_mode = lv_obj_get_child(btn_mode, 0);
        lv_obj_add_event_cb(btn_mode, btn_mode_click, LV_EVENT_CLICKED, NULL);

        /* ---- 第 3 行：连接 + 状态 ---- */
        btn_conn = make_btn(scr_wifi, 198, 30, 6, 110, C_BLUE, "连接");
        lv_obj_add_event_cb(btn_conn, btn_connect_click, LV_EVENT_CLICKED, NULL);

        label_st = lv_label_create(scr_wifi);
        lv_obj_set_size(label_st, 104, 30);
        lv_label_set_long_mode(label_st, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(label_st, UI_FONT_ASCII, 0);
        lv_obj_align(label_st, LV_ALIGN_TOP_LEFT, 210, 110);
        set_status("--", C_DIM);

        /* 底部键盘常驻（本页核心交互是输入）：压到 92px，上面三行各 30px */
        kb = lv_keyboard_create(scr_wifi);
        lv_obj_set_size(kb, 320, 92);
        lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
        ui_style_keyboard(kb);
        lv_keyboard_set_textarea(kb, ta_pass);

        /* 预填当前凭据：来自 W25Q 存档（设置页保存过）或编译期默认。
         * 注意 ta_ssid 在列表模式下是隐藏的，但始终是"要连的 SSID"的载体。 */
        wifi_conn_get(ssid, sizeof ssid, pass, sizeof pass);
        lv_textarea_set_text(ta_ssid, ssid);
        lv_textarea_set_text(ta_pass, pass);

        wifi_timer = lv_timer_create(wifi_timer_cb, 500, NULL);
        start_scan();          /* 进页面自动扫一次，省掉"先点扫描"这一步 */
    }
    lv_scr_load(scr_wifi);
}
