/* 主监控页 · 深色工业风
 * 版面（320x240）：
 *   y   0-26  顶栏：中文标题「边缘网关监控」+ 右上 WiFi/云 状态
 *   y  30-160 双仪表盘：温度 / 湿度（130px 弧盘）
 *   y 164-194 实时时间条：HH:MM:SS（24px）+ 运行时长 + 设置入口
 *   y 198-234 光照读数卡片：只有数值，没有进度条（用户明确要求去掉橙色条）
 * 色板与控件样式统一走 ui_theme.h，本页不再自己写色值。
 */
#include "lvgl.h"
#include "ui_theme.h"
#include "page_settings.h"
#include "tasks.h"
#include "FreeRTOS.h"
#include "task.h"
#include "stm32f4xx.h"
#include <stdio.h>

static lv_obj_t *label_temp, *label_humi, *label_light;
static lv_obj_t *arc_temp, *arc_humi;
static lv_obj_t *label_status, *label_uptime, *label_clock;

/* 所有数值控件收为本页静态，不导出全局句柄 */

/* 造一个仪表盘：lv_arc 去掉旋钮 + 圆头弧线，135°~45° 扫 270° 的经典表盘形态；
 * 中心放大号数值，弧线下沿放名称标签 */
static lv_obj_t *make_gauge(lv_obj_t *parent, int32_t min, int32_t max,
                            uint32_t color, const char *name)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_size(arc, 130, 130);
    lv_obj_remove_style_all(arc);               /* 先清默认主题（含旋钮那套），再自绘 */
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_arc_set_range(arc, min, max);
    lv_arc_set_bg_angles(arc, 135, 45);
    lv_arc_set_value(arc, min);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(C_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);

    lv_obj_t *lab_name = lv_label_create(arc);
    lv_label_set_text(lab_name, name);
    lv_obj_set_style_text_font(lab_name, UI_FONT_ASCII, 0);
    lv_obj_set_style_text_color(lab_name, lv_color_hex(C_DIM), 0);
    lv_obj_align(lab_name, LV_ALIGN_CENTER, 0, 22);
    return arc;
}

static void btn_set_click(lv_event_t *e)
{
    (void)e;
    page_settings_open();
}

lv_obj_t *page_main_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    ui_style_screen(scr);

    /* ---- 顶栏：中文标题 | 状态 ---- */
    lv_obj_t *top = lv_obj_create(scr);
    lv_obj_set_size(top, 320, 26);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_style_all(top);
    lv_obj_set_style_bg_color(top, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(top);
    lv_label_set_text(title, "边缘网关监控");
    lv_obj_set_style_text_font(title, UI_FONT_CJK, 0);   /* 中文标题用小字号（14px 抗锯齿） */
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 8, 0);

    label_status = lv_label_create(top);
    lv_label_set_text(label_status, LV_SYMBOL_WIFI " OFF");
    lv_obj_set_style_text_font(label_status, UI_FONT_ASCII, 0);  /* 含图标，必须用 montserrat */
    lv_obj_set_style_text_color(label_status, lv_color_hex(C_FAIL), 0);
    lv_obj_align(label_status, LV_ALIGN_RIGHT_MID, -8, 0);

    /* ---- 双仪表：温度 / 湿度 ----
     * 量程对应传感器实际范围（温 -10~60℃，湿 0~100%） */
    arc_temp = make_gauge(scr, -10, 60, C_TEMP, "TEMP C");
    lv_obj_align(arc_temp, LV_ALIGN_TOP_LEFT, 14, 30);
    label_temp = lv_label_create(arc_temp);
    lv_obj_set_style_text_font(label_temp, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(label_temp, lv_color_hex(C_TEMP), 0);
    lv_obj_align(label_temp, LV_ALIGN_CENTER, 0, -8);
    lv_label_set_text(label_temp, "--");

    arc_humi = make_gauge(scr, 0, 100, C_HUMI, "HUMI %");
    lv_obj_align(arc_humi, LV_ALIGN_TOP_RIGHT, -14, 30);
    label_humi = lv_label_create(arc_humi);
    lv_obj_set_style_text_font(label_humi, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(label_humi, lv_color_hex(C_HUMI), 0);
    lv_obj_align(label_humi, LV_ALIGN_CENTER, 0, -8);
    lv_label_set_text(label_humi, "--");

    /* ---- 实时时间条：HH:MM:SS | 运行时长 | 设置 ---- */
    lv_obj_t *card_t = lv_obj_create(scr);
    lv_obj_set_size(card_t, 300, 30);
    lv_obj_align(card_t, LV_ALIGN_TOP_MID, 0, 164);
    ui_style_card(card_t, false);

    label_clock = lv_label_create(card_t);
    lv_label_set_text(label_clock, "--:--:--");
    lv_obj_set_style_text_font(label_clock, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(label_clock, lv_color_hex(C_DIM), 0);  /* 未校时=暗色 */
    lv_obj_align(label_clock, LV_ALIGN_LEFT_MID, 10, 0);

    lv_obj_t *btn_set = lv_btn_create(card_t);
    lv_obj_set_size(btn_set, 34, 22);
    lv_obj_align(btn_set, LV_ALIGN_RIGHT_MID, -6, 0);
    ui_style_btn(btn_set, C_TRACK, C_TEXT);
    lv_obj_t *lab_set = lv_label_create(btn_set);
    lv_label_set_text(lab_set, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(lab_set, UI_FONT_ASCII, 0);
    lv_obj_center(lab_set);
    lv_obj_add_event_cb(btn_set, btn_set_click, LV_EVENT_CLICKED, NULL);

    label_uptime = lv_label_create(card_t);
    lv_label_set_text(label_uptime, "运行 --:--:--");
    lv_obj_set_style_text_font(label_uptime, UI_FONT_CJK, 0);
    lv_obj_set_style_text_color(label_uptime, lv_color_hex(C_DIM), 0);
    lv_obj_align_to(label_uptime, btn_set, LV_ALIGN_OUT_LEFT_MID, -8, 0);

    /* ---- 光照读数卡片（无进度条）---- */
    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_set_size(card, 300, 36);
    lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, -6);
    ui_style_card(card, true);

    lv_obj_t *lab_ltitle = lv_label_create(card);
    lv_label_set_text(lab_ltitle, LV_SYMBOL_EYE_OPEN " LIGHT");
    lv_obj_set_style_text_font(lab_ltitle, UI_FONT_ASCII, 0);
    lv_obj_set_style_text_color(lab_ltitle, lv_color_hex(C_DIM), 0);
    lv_obj_align(lab_ltitle, LV_ALIGN_LEFT_MID, 10, 0);

    lv_obj_t *lab_lunit = lv_label_create(card);
    lv_label_set_text(lab_lunit, "lx");
    lv_obj_set_style_text_font(lab_lunit, UI_FONT_ASCII, 0);
    lv_obj_set_style_text_color(lab_lunit, lv_color_hex(C_DIM), 0);
    lv_obj_align(lab_lunit, LV_ALIGN_RIGHT_MID, -10, 0);

    label_light = lv_label_create(card);
    lv_obj_set_style_text_font(label_light, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(label_light, lv_color_hex(C_LIGHT), 0);
    lv_label_set_text(label_light, "--");
    lv_obj_align_to(label_light, lab_lunit, LV_ALIGN_OUT_LEFT_MID, -4, 0);

    return scr;
}

/* ---- 数据刷新 ---- */
static float last_temp = -999, last_humi = -999, last_light = -99999;

void page_main_update_data(float temp, float humi, float light)
{
    if (temp == last_temp && humi == last_humi && light == last_light)
        return;

    lv_label_set_text_fmt(label_temp, "%.1f", temp);
    lv_label_set_text_fmt(label_humi, "%.0f", humi);
    lv_label_set_text_fmt(label_light, "%.0f", light);
    lv_arc_set_value(arc_temp, (int16_t)temp);
    lv_arc_set_value(arc_humi, (int16_t)humi);

    /* 数据大幅跳变时亮屏（沿用原版行为：突变即提醒） */
    extern TickType_t idle_tick;
    extern uint8_t lcd_bl_en;
    if ((temp - last_temp > 3 || last_temp - temp > 3) ||
        (humi - last_humi > 3 || last_humi - humi > 3) ||
        (light - last_light > 10 || last_light - light > 10))
    {
        idle_tick = xTaskGetTickCount();
        HAL_GPIO_WritePin(GPIOF, GPIO_PIN_10, GPIO_PIN_SET);
        lcd_bl_en = 1;
    }

    last_temp = temp; last_humi = humi; last_light = light;
}

void page_main_update_status(uint8_t wifi, uint8_t mqtt)
{
    if (!label_status) return;
    if (!wifi)
    {
        lv_label_set_text(label_status, LV_SYMBOL_WIFI " OFF");
        lv_obj_set_style_text_color(label_status, lv_color_hex(C_FAIL), 0);
    }
    else if (!mqtt)
    {
        lv_label_set_text(label_status, LV_SYMBOL_WIFI " NO CLOUD");
        lv_obj_set_style_text_color(label_status, lv_color_hex(C_WARN), 0);
    }
    else
    {
        lv_label_set_text(label_status, LV_SYMBOL_WIFI " ONLINE");
        lv_obj_set_style_text_color(label_status, lv_color_hex(C_OK), 0);
    }
}

void page_main_update_uptime(uint32_t seconds)
{
    if (!label_uptime) return;
    uint32_t h = seconds / 3600, m = (seconds % 3600) / 60, s = seconds % 60;
    lv_label_set_text_fmt(label_uptime, "运行 %02u:%02u:%02u",
                          (unsigned)h, (unsigned)m, (unsigned)s);
}

void page_main_update_clock(uint8_t h, uint8_t m, uint8_t s, uint8_t synced)
{
    if (!label_clock) return;
    lv_label_set_text_fmt(label_clock, "%02u:%02u:%02u", h, m, s);
    /* 未校时（没联网 SNTP 成功过、也没手动设过）→ 暗色，别装成准的 */
    lv_obj_set_style_text_color(label_clock,
                                lv_color_hex(synced ? C_TEXT : C_DIM), 0);
}
