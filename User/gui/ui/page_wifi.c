/* WiFi 配置页 · 深色
 *
 * 交互定稿（用户拍板，替代旧的"下拉框 / 手动输入二选一"）：
 *   第 1 行 = SSID 输入框 + 「WiFi 列表」按钮；
 *   点按钮 → 全屏模态列表（按信号强度排序 + 显示 dBm）→ 点某行即把它填进
 *   SSID 输入框，焦点自动跳到密码框，接着输密码、点连接。
 *   没有"模式"这个概念了：SSID 输入框常驻、也能手输（配隐藏热点），
 *   列表只当选择器 —— 少一套状态，少一次误解。
 *
 * 扫描仍然异步：UI 只置 wifi_scan_request，AT 任务在持锁的 AT 语境里执行
 * （~10s 的阻塞扫描不会冻住界面），UI 用 500ms 定时器轮询 wifi_scan_state。
 */
#include "page_wifi.h"
#include "page_settings.h"
#include "ui_theme.h"
#include "wifi_conn.h"
#include "tasks.h"
#include "stm32f4xx.h"
#include <stdio.h>
#include <string.h>

#define SCAN_FRESH_MS   60000u   /* 结果新鲜度：超过这个时间再打开弹窗就重扫 */
#define AP_ROW_H        26

static lv_obj_t *scr_wifi = NULL;
static lv_obj_t *ta_ssid, *ta_pass, *btn_conn, *label_st, *kb;
static lv_obj_t *modal = NULL, *modal_list = NULL;
static lv_timer_t *wifi_timer = NULL;
static uint8_t  last_scan_state = WIFI_SCAN_IDLE;
static uint8_t  timeout_shown = 0;
static uint32_t scan_start_tick = 0;

static void btn_back(void)
{
    page_settings_open();
}

/* 焦点切换：清掉两个输入框的聚焦态，再给目标发一次 FOCUSED
 * （ta_focus_cb 借这个事件把键盘绑过去；LVGL 也据此画光标） */
static void focus_ta(lv_obj_t *ta)
{
    lv_obj_clear_state(ta_ssid, LV_STATE_FOCUSED);
    lv_obj_clear_state(ta_pass, LV_STATE_FOCUSED);
    lv_event_send(ta, LV_EVENT_FOCUSED, NULL);
}

static void ta_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    lv_keyboard_set_textarea(kb, ta);
}

static void set_status(const char *txt, uint32_t color)
{
    lv_label_set_text(label_st, txt);
    lv_obj_set_style_text_color(label_st, lv_color_hex(color), 0);
}

/* 发起扫描：只置标志，真正的串口操作在 AT 任务里（界面不阻塞） */
static void start_scan(void)
{
    wifi_scan_state   = WIFI_SCAN_RUNNING;
    wifi_scan_request = 1;
    last_scan_state   = WIFI_SCAN_RUNNING;
    scan_start_tick   = HAL_GetTick();
    timeout_shown     = 0;
}

/* ============================================================
 *  模态热点列表
 * ============================================================ */

static void modal_row_click(lv_event_t *e);   /* 前向声明：行工厂里要用 */

/* 提示行（扫描中 / 无热点 / 扫描失败 / 扫描超时） */
static void modal_plain_row(const char *txt, uint32_t color)
{
    lv_obj_t *row = lv_obj_create(modal_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), AP_ROW_H);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *lab = lv_label_create(row);
    lv_label_set_text(lab, txt);
    lv_obj_set_style_text_font(lab, UI_FONT_CJK, 0);
    lv_obj_set_style_text_color(lab, lv_color_hex(color), 0);
    lv_obj_align(lab, LV_ALIGN_LEFT_MID, 8, 0);
}

/* 一个热点行：左 SSID（中文/ASCII 都走 CJK 字体），右信号 dBm。
 * 选中的那个（与输入框内容一致）用亮一档的底色标出来。 */
static void modal_ap_row(const char *ssid, int8_t rssi, uint8_t selected)
{
    char buf[16];

    lv_obj_t *row = lv_btn_create(modal_list);
    lv_obj_set_size(row, LV_PCT(100), AP_ROW_H);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    ui_style_btn(row, selected ? 0x475569 : C_TRACK, C_TEXT);

    lv_obj_t *l = lv_label_create(row);            /* 子标签 0 = SSID（点击回调从它取） */
    lv_label_set_text(l, ssid);
    lv_obj_set_style_text_font(l, UI_FONT_CJK, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 8, 0);

    if (rssi != -127) snprintf(buf, sizeof buf, "%d", (int)rssi);
    else              snprintf(buf, sizeof buf, "--");
    lv_obj_t *r = lv_label_create(row);
    lv_label_set_text(r, buf);
    lv_obj_set_style_text_font(r, UI_FONT_ASCII, 0);
    lv_obj_set_style_text_color(r, lv_color_hex(selected ? C_TEXT : C_DIM), 0);
    lv_obj_align(r, LV_ALIGN_RIGHT_MID, -8, 0);

    lv_obj_add_event_cb(row, modal_row_click, LV_EVENT_CLICKED, NULL);
}

/* 按当前扫描状态重建列表。只在"状态变化 / 开窗"时调用，别每 500ms 重建
 * （每次重建都要删建十几个对象，白烧 LVGL 堆）。 */
static void modal_refresh(void)
{
    if (modal_list == NULL) return;
    lv_obj_clean(modal_list);

    if (wifi_scan_state == WIFI_SCAN_RUNNING) { modal_plain_row("扫描中...", C_BLUE); return; }
    if (wifi_scan_state == WIFI_SCAN_FAIL)    { modal_plain_row("扫描失败",  C_WARN); return; }
    if (wifi_ap_count == 0)                   { modal_plain_row("无热点",    C_WARN); return; }

    const char *cur = lv_textarea_get_text(ta_ssid);
    for (int i = 0; i < wifi_ap_count && i < WIFI_AP_MAX; i++)
        modal_ap_row(wifi_ap_list[i], wifi_ap_rssi[i],
                     (cur[0] != 0) && (strcmp(cur, wifi_ap_list[i]) == 0));
}

/* 点行 → 选择该热点：填入输入框 + 关窗 + 焦点去密码框 */
static void modal_row_click(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_target(e);
    lv_obj_t *lab = lv_obj_get_child(row, 0);       /* 子标签 0 = SSID 文本（标签自己持有副本） */
    if (lab == NULL) return;

    const char *ssid = lv_label_get_text(lab);
    lv_textarea_set_text(ta_ssid, ssid);

    /* 密码框跟着选中项走：选中的正是当前已存网络 → 刷回已存密码（重连/改密都顺手）；
     * 选的是别的网络 → 清空。不清的话，上一个网络输过的密码会残留——点连接就把
     * (新SSID, 旧密码) 写进 flash，之后怎么切、复位都连不上（手册 #27 同源事故）。 */
    char cur_ssid[33], cur_pass[65];
    wifi_conn_get(cur_ssid, sizeof cur_ssid, cur_pass, sizeof cur_pass);
    lv_textarea_set_text(ta_pass, (strcmp(ssid, cur_ssid) == 0) ? cur_pass : "");

    lv_obj_add_flag(modal, LV_OBJ_FLAG_HIDDEN);     /* 选完就关，别挡着输密码 */
    focus_ta(ta_pass);
}

/* 点面板外的压暗区 / 点 ✕ 都关窗（面板本身 clickable，点它不会冒泡到这里） */
static void modal_close_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(modal, LV_OBJ_FLAG_HIDDEN);
}

static void btn_rescan_click(lv_event_t *e)
{
    (void)e;
    start_scan();
    modal_refresh();                                /* 立刻切到"扫描中..." */
}

static void modal_create(void)
{
    modal = lv_obj_create(scr_wifi);
    lv_obj_remove_style_all(modal);
    lv_obj_set_size(modal, 320, 240);
    lv_obj_align(modal, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_hex(C_DARK), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_80, 0);       /* 压暗背景 */
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(modal, modal_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *panel = lv_obj_create(modal);
    lv_obj_set_size(panel, 300, 206);   /* 206 = 标题 34 + 列表 168 + 余量：失败时 6 行自检放得下 */
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 22);
    ui_style_card(panel, true);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);      /* 吞掉点击：点面板不该关窗（无需回调） */

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "WiFi 列表");
    lv_obj_set_style_text_font(title, UI_FONT_CJK, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 8);

    lv_obj_t *btn_rf = lv_btn_create(panel);
    lv_obj_set_size(btn_rf, 56, 24);
    lv_obj_align(btn_rf, LV_ALIGN_TOP_RIGHT, -42, 6);
    ui_style_btn(btn_rf, C_TRACK, C_TEXT);
    lv_obj_t *lab_rf = lv_label_create(btn_rf);
    lv_label_set_text(lab_rf, "刷新");
    lv_obj_set_style_text_font(lab_rf, UI_FONT_CJK, 0);
    lv_obj_center(lab_rf);
    lv_obj_add_event_cb(btn_rf, btn_rescan_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_x = lv_btn_create(panel);
    lv_obj_set_size(btn_x, 30, 24);
    lv_obj_align(btn_x, LV_ALIGN_TOP_RIGHT, -6, 6);
    ui_style_btn(btn_x, C_TRACK, C_TEXT);
    lv_obj_t *lab_x = lv_label_create(btn_x);
    lv_label_set_text(lab_x, LV_SYMBOL_CLOSE);          /* 图标：必须 montserrat */
    lv_obj_set_style_text_font(lab_x, UI_FONT_ASCII, 0);
    lv_obj_center(lab_x);
    lv_obj_add_event_cb(btn_x, modal_close_cb, LV_EVENT_CLICKED, NULL);

    modal_list = lv_obj_create(panel);
    lv_obj_set_size(modal_list, 288, 168);   /* 6 行(26px+2 间距)：失败时最多 6 行自检信息 */
    lv_obj_align(modal_list, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_set_style_bg_color(modal_list, lv_color_hex(C_DARK), 0);
    lv_obj_set_style_bg_opa(modal_list, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(modal_list, 6, 0);
    lv_obj_set_style_border_width(modal_list, 0, 0);
    lv_obj_set_style_pad_all(modal_list, 2, 0);
    lv_obj_set_style_pad_right(modal_list, 4, 0);   /* 给右侧滚动条留位置，别压住 dBm */
    lv_obj_set_style_pad_row(modal_list, 2, 0);
    lv_obj_set_flex_flow(modal_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(modal_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(modal_list, 3, LV_PART_SCROLLBAR);      /* 细滚动条 */
    lv_obj_set_style_bg_color(modal_list, lv_color_hex(C_TRACK), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(modal_list, LV_OPA_COVER, LV_PART_SCROLLBAR);
}

static void modal_open(void)
{
    if (modal == NULL) modal_create();
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(modal);              /* 盖在页面所有控件之上 */

    /* 没结果、或结果太旧 → 重扫一次；已经在扫了就别重复下发请求
     * （AT 任务一次扫描就要 10s 级，重复请求等于再占一轮串口） */
    if (wifi_scan_state != WIFI_SCAN_RUNNING &&
        (wifi_ap_count == 0 || (HAL_GetTick() - scan_start_tick) > SCAN_FRESH_MS))
        start_scan();

    modal_refresh();
}

static void btn_list_click(lv_event_t *e)
{
    (void)e;
    modal_open();
}

static void btn_connect_click(lv_event_t *e)
{
    (void)e;
    const char *ssid = lv_textarea_get_text(ta_ssid);
    const char *pass = lv_textarea_get_text(ta_pass);

    if (ssid[0] == 0) {                      /* 没选热点也没手输 SSID */
        set_status("NO SSID", C_FAIL);
        return;
    }
    if (pass[0] == 0) {
        /* 切换网络时密码框会被清空（防上一个网络的密码残留），这里把"忘输密码"
         * 挡在存档之前——否则空密码会按开放网络去入网，对 WPA2 AP 必被拒，
         * 现场只会看到一句没头没尾的 wifi denied */
        set_status("NO PASS", C_FAIL);
        return;
    }
    if (wifi_conn_save(ssid, pass) != 0) {
        set_status("Save FAIL", C_FAIL);
        return;
    }
    wifi_reconnect_request = 1;      /* AT 任务 ≤1s 响应；标志在重连尝试完整结束才清 */
}

/* 小眼睛：切换密码明文/圆点。图标按"点下去会发生什么"画：
 * 密文态显示睁眼（点它看明文），明文态显示闭眼（点它藏回去）。 */
static void btn_eye_click(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *lab = lv_obj_get_child(btn, 0);
    bool masked = lv_textarea_get_password_mode(ta_pass);

    lv_textarea_set_password_mode(ta_pass, !masked);
    lv_label_set_text(lab, masked ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
    lv_obj_center(lab);     /* 两个图标字宽不一定相等，重对一次免得歪 */
}

/* 500ms 状态轮询（仅本页可见时刷新）
 * 1) 扫描状态变化 → 重建弹窗列表；扫太久（>25s）给个"扫描超时"提示
 * 2) 连接状态四态，优先级：Connecting > WiFi FAIL > NO CLOUD > WiFi OK。
 *    Connecting 能贯穿全程的原因：wifi_reconnect_request 从 UI 置 1 起，
 *    直到 AT 任务把"退网→连新 WiFi→TCP→MQTT"整个重连流程跑完才清 0。 */
static void wifi_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (lv_scr_act() != scr_wifi) return;

    uint8_t modal_shown = (modal != NULL) && !lv_obj_has_flag(modal, LV_OBJ_FLAG_HIDDEN);

    if (wifi_scan_state != last_scan_state)
    {
        last_scan_state = wifi_scan_state;
        timeout_shown   = 0;
        if (modal_shown) modal_refresh();
    }
    else if (modal_shown && !timeout_shown &&
             wifi_scan_state == WIFI_SCAN_RUNNING &&
             (HAL_GetTick() - scan_start_tick) > 25000)
    {
        /* AT 任务可能正卡在整轮重连里：只提示，不动共享状态 ——
         * 等它真扫完（DONE/FAIL）上面的分支会照常刷新列表 */
        timeout_shown = 1;
        lv_obj_clean(modal_list);
        modal_plain_row("扫描超时", C_WARN);
    }

    /* 判据顺序与主屏一致：MQTT 在线必然踩着活的 WiFi —— 别让过期的
     * wifi_connected（模块自连不置 1）把在线状态谎报成 WiFi FAIL。 */
    if (wifi_reconnect_request)
    {
        set_status("Connecting...", C_BLUE);
        lv_obj_add_state(btn_conn, LV_STATE_DISABLED);
    }
    else if (g_mqtt_connected)
    {
        set_status("WiFi OK", C_OK);
        lv_obj_clear_state(btn_conn, LV_STATE_DISABLED);
    }
    else if (wifi_connected)
    {
        set_status("NO CLOUD", C_WARN);
        lv_obj_clear_state(btn_conn, LV_STATE_DISABLED);
    }
    else
    {
        set_status("WiFi FAIL", C_FAIL);
        lv_obj_clear_state(btn_conn, LV_STATE_DISABLED);
    }
}

/* 小工具：带中文字体的按钮 */
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

        /* ---- 第 1 行：SSID 输入框（常驻可手输）+ 「WiFi 列表」选择器 ---- */
        ta_ssid = lv_textarea_create(scr_wifi);
        lv_obj_set_size(ta_ssid, 198, 30);
        lv_obj_align(ta_ssid, LV_ALIGN_TOP_LEFT, 6, 42);
        lv_textarea_set_one_line(ta_ssid, true);
        lv_textarea_set_max_length(ta_ssid, 32);
        lv_textarea_set_placeholder_text(ta_ssid, "SSID");
        ui_style_textarea(ta_ssid);
        lv_obj_add_event_cb(ta_ssid, ta_focus_cb, LV_EVENT_FOCUSED, NULL);

        lv_obj_t *btn_list = make_btn(scr_wifi, 104, 30, 210, 42, C_TRACK, "WiFi 列表");
        lv_obj_add_event_cb(btn_list, btn_list_click, LV_EVENT_CLICKED, NULL);

        /* ---- 第 2 行：密码(164) + 小眼睛(30) + 连接(104)，左右边距仍对称 6px ---- */
        ta_pass = lv_textarea_create(scr_wifi);
        lv_obj_set_size(ta_pass, 164, 30);
        lv_obj_align(ta_pass, LV_ALIGN_TOP_LEFT, 6, 76);
        lv_textarea_set_one_line(ta_pass, true);
        lv_textarea_set_password_mode(ta_pass, true);
        lv_textarea_set_max_length(ta_pass, 64);
        lv_textarea_set_placeholder_text(ta_pass, "密码");
        ui_style_textarea(ta_pass);
        lv_obj_add_event_cb(ta_pass, ta_focus_cb, LV_EVENT_FOCUSED, NULL);

        lv_obj_t *btn_eye = lv_btn_create(scr_wifi);
        lv_obj_set_size(btn_eye, 30, 30);
        lv_obj_align(btn_eye, LV_ALIGN_TOP_LEFT, 174, 76);
        ui_style_btn(btn_eye, C_TRACK, C_TEXT);
        lv_obj_t *lab_eye = lv_label_create(btn_eye);
        lv_label_set_text(lab_eye, LV_SYMBOL_EYE_OPEN);   /* 初始密文态：点它看明文 */
        lv_obj_set_style_text_font(lab_eye, UI_FONT_ASCII, 0);   /* 图标：必须 montserrat */
        lv_obj_center(lab_eye);
        lv_obj_add_event_cb(btn_eye, btn_eye_click, LV_EVENT_CLICKED, NULL);

        btn_conn = make_btn(scr_wifi, 104, 30, 210, 76, C_BLUE, "连接");
        lv_obj_add_event_cb(btn_conn, btn_connect_click, LV_EVENT_CLICKED, NULL);

        /* 连接状态挂标题栏右上角：下面整块高度让给键盘 */
        label_st = lv_label_create(scr_wifi);
        lv_obj_set_style_text_font(label_st, UI_FONT_ASCII, 0);
        lv_obj_align(label_st, LV_ALIGN_TOP_RIGHT, -8, 8);
        set_status("--", C_DIM);

        /* 底部键盘常驻（本页核心交互是输入）：输入区只剩两行，
         * 键盘从 92 加高到 128，误触率低很多 */
        kb = lv_keyboard_create(scr_wifi);
        lv_obj_set_size(kb, 320, 128);
        lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
        ui_style_keyboard(kb);

        /* 预填当前凭据（W25Q 存档或编译期默认），方便只改其中一项 */
        wifi_conn_get(ssid, sizeof ssid, pass, sizeof pass);
        lv_textarea_set_text(ta_ssid, ssid);
        lv_textarea_set_text(ta_pass, pass);
        focus_ta(ta_pass);            /* 就绪在密码框，一进来打字就进密码 */

        wifi_timer = lv_timer_create(wifi_timer_cb, 500, NULL);
        start_scan();                 /* 进页面先扫一次，「WiFi 列表」打开即有内容 */
    }
    lv_scr_load(scr_wifi);
}
