/* 主监控页 · 深色工业风：双仪表盘（温/湿）+ 光照横条 + 顶栏状态/时长/设置入口 */
#include "lvgl.h"
#include "page_settings.h"
#include "tasks.h"
#include "FreeRTOS.h"
#include "task.h"
#include "stm32f4xx.h"
#include <stdio.h>

/* ---- 设计系统：石板深色（各页共用同一套色板，改主题只动这里）----
 * 背景/卡片/轨道三级灰阶 + 每项数据一个强调色，数字用内置 Montserrat 字体 */
#define C_BG        0x0F172A   /* 背景 slate-900 */
#define C_CARD      0x1E293B   /* 卡片 slate-800 */
#define C_TRACK     0x334155   /* 轨道/边框 slate-700 */
#define C_TEXT      0xE2E8F0
#define C_TEXT_DIM  0x94A3B8
#define C_TEMP      0xF87171   /* 温度 red-400 */
#define C_HUMI      0x2DD4BF   /* 湿度 teal-400 */
#define C_LIGHT     0xFBBF24   /* 光照 amber-400 */
#define C_OK        0x34D399
#define C_FAIL      0xF87171
#define C_WARN      0xFBBF24

static lv_obj_t *label_temp, *label_humi, *label_light;
static lv_obj_t *arc_temp, *arc_humi, *bar_light;
static lv_obj_t *label_status, *label_uptime;

/* 所有数值控件收为本页静态，不再像旧版那样导出全局句柄 */

/* 造一个仪表盘：lv_arc 去掉旋钮 + 圆头弧线，135°~45° 扫 270° 的经典表盘形态；
 * 中心放大号数值，弧线下沿放名称标签 */
static lv_obj_t *make_gauge(lv_obj_t *parent, int32_t min, int32_t max,
                            uint32_t color, const char *name)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_size(arc, 130, 130);
    lv_arc_set_range(arc, min, max);
    lv_arc_set_bg_angles(arc, 135, 45);
    lv_arc_set_value(arc, min);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);          /* 隐藏旋钮，纯仪表形态 */
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(C_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);

    lv_obj_t *lab_name = lv_label_create(arc);
    lv_label_set_text(lab_name, name);
    lv_obj_set_style_text_font(lab_name, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lab_name, lv_color_hex(C_TEXT_DIM), 0);
    lv_obj_align(lab_name, LV_ALIGN_CENTER, 0, 20);
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
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 顶栏：状态 | 设置 | 运行时长 ---- */
    lv_obj_t *top = lv_obj_create(scr);
    lv_obj_set_size(top, 320, 26);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(top, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_radius(top, 0, 0);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    label_status = lv_label_create(top);
    lv_label_set_text(label_status, LV_SYMBOL_WIFI " OFF");
    lv_obj_set_style_text_font(label_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label_status, lv_color_hex(C_FAIL), 0);
    lv_obj_align(label_status, LV_ALIGN_LEFT_MID, 6, 0);

    lv_obj_t *btn_set = lv_btn_create(top);
    lv_obj_set_size(btn_set, 34, 20);
    lv_obj_align(btn_set, LV_ALIGN_RIGHT_MID, -82, 0);
    lv_obj_set_style_bg_color(btn_set, lv_color_hex(C_TRACK), 0);
    lv_obj_set_style_radius(btn_set, 4, 0);
    lv_obj_t *lab_set = lv_label_create(btn_set);
    lv_label_set_text(lab_set, LV_SYMBOL_SETTINGS);
    lv_obj_align(lab_set, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(lab_set, lv_color_hex(C_TEXT), 0);
    lv_obj_add_event_cb(btn_set, btn_set_click, LV_EVENT_CLICKED, NULL);

    label_uptime = lv_label_create(top);
    lv_label_set_text(label_uptime, "00:00:00");
    lv_obj_set_style_text_font(label_uptime, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label_uptime, lv_color_hex(C_TEXT_DIM), 0);
    lv_obj_align(label_uptime, LV_ALIGN_RIGHT_MID, -6, 0);

    /* ---- 双仪表：温度 / 湿度 ----
     * 130px 弧盘左右各一，量程对应传感器实际范围（温 -10~60℃，湿 0~100%） */
    arc_temp = make_gauge(scr, -10, 60, C_TEMP, "TEMP C");
    lv_obj_align(arc_temp, LV_ALIGN_TOP_LEFT, 14, 34);
    label_temp = lv_label_create(arc_temp);
    lv_obj_set_style_text_font(label_temp, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(label_temp, lv_color_hex(C_TEMP), 0);
    lv_obj_align(label_temp, LV_ALIGN_CENTER, 0, -6);
    lv_label_set_text(label_temp, "--");

    arc_humi = make_gauge(scr, 0, 100, C_HUMI, "HUMI %");
    lv_obj_align(arc_humi, LV_ALIGN_TOP_RIGHT, -14, 34);
    label_humi = lv_label_create(arc_humi);
    lv_obj_set_style_text_font(label_humi, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(label_humi, lv_color_hex(C_HUMI), 0);
    lv_obj_align(label_humi, LV_ALIGN_CENTER, 0, -6);
    lv_label_set_text(label_humi, "--");

    /* ---- 光照横条卡片 ---- */
    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_set_size(card, 300, 56);
    lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(C_TRACK), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lab_ltitle = lv_label_create(card);
    lv_label_set_text(lab_ltitle, LV_SYMBOL_EYE_OPEN " LIGHT lx");
    lv_obj_set_style_text_font(lab_ltitle, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lab_ltitle, lv_color_hex(C_TEXT_DIM), 0);
    lv_obj_align(lab_ltitle, LV_ALIGN_TOP_LEFT, 10, 6);

    label_light = lv_label_create(card);
    lv_obj_set_style_text_font(label_light, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(label_light, lv_color_hex(C_LIGHT), 0);
    lv_obj_align(label_light, LV_ALIGN_TOP_RIGHT, -10, 4);
    lv_label_set_text(label_light, "--");

    bar_light = lv_bar_create(card);
    lv_obj_set_size(bar_light, 278, 8);
    lv_obj_align(bar_light, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_bar_set_range(bar_light, 0, 20000);
    lv_bar_set_value(bar_light, 0, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_light, lv_color_hex(C_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_light, lv_color_hex(C_LIGHT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_light, 4, 0);

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
    lv_bar_set_value(bar_light, (int32_t)light, LV_ANIM_ON);

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
    lv_label_set_text_fmt(label_uptime, "%02u:%02u:%02u",
                          (unsigned)h, (unsigned)m, (unsigned)s);
}
