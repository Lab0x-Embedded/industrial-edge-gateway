/* 系统日志页 · 深色：W25Q 日志分区倒序读取，最新置顶
 *
 * 深色改版第一版漏了这一页（底色还是 0xF8F9FA 浅灰 + 0x0F3460 深蓝按钮），
 * 这版按 ui_theme 的色板重做，并顺手做了两件事：
 *   1) 行数上限 30 → 10：320x240 一屏也就装 10 行，而 LVGL 堆只有 48KB，
 *      30 行对象 + 字符串一次要吃掉近 10KB（整机 RAM 已用 86%+）。
 *   2) 等级配色：INFO 白 / WARN 琥珀 / ERROR 红，扫一眼就知道有没有事。 */
#include "page_log.h"
#include "log.h"
#include "page_main.h"
#include "ui_theme.h"
#include "tasks.h"
#include "wifi_conn.h"
#include <stdio.h>

extern lv_obj_t *main_scr;

#define LOG_ROWS   10          /* 一屏显示条数（行高 16px） */

static lv_obj_t *scr_log = NULL;
static lv_obj_t *list_log = NULL;
static lv_obj_t *label_count = NULL;

static void btn_back(void)
{
    lv_scr_load(main_scr);
    page_main_update_status(wifi_connected, g_mqtt_connected);   /* 返回后刷新主界面状态 */
}

static void btn_refresh_click(lv_event_t *e)
{
    (void)e;
    page_log_refresh();
}

/* 一行日志：透明行 + 单标签（整个行一个颜色，够用且省对象） */
static void add_log_row(const char *txt, uint32_t color, const lv_font_t *font)
{
    lv_obj_t *row = lv_obj_create(list_log);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 16);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lab = lv_label_create(row);
    lv_label_set_text(lab, txt);
    lv_obj_set_style_text_font(lab, font, 0);
    lv_obj_set_style_text_color(lab, lv_color_hex(color), 0);
    lv_obj_align(lab, LV_ALIGN_LEFT_MID, 6, 0);
}

lv_obj_t *page_log_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    ui_style_screen(scr);
    ui_title_bar(scr, "系统运行日志", btn_back, UI_FONT_CJK);

    label_count = lv_label_create(scr);
    lv_label_set_text(label_count, "共 0 条");
    lv_obj_set_style_text_font(label_count, UI_FONT_CJK, 0);
    lv_obj_set_style_text_color(label_count, lv_color_hex(C_DIM), 0);
    lv_obj_align(label_count, LV_ALIGN_TOP_LEFT, 8, 42);

    lv_obj_t *btn_rf = lv_btn_create(scr);
    lv_obj_set_size(btn_rf, 64, 24);
    lv_obj_align(btn_rf, LV_ALIGN_TOP_RIGHT, -6, 40);
    ui_style_btn(btn_rf, C_TRACK, C_TEXT);
    lv_obj_t *lab_rf = lv_label_create(btn_rf);
    lv_label_set_text(lab_rf, "刷新");
    lv_obj_set_style_text_font(lab_rf, UI_FONT_CJK, 0);
    lv_obj_center(lab_rf);
    lv_obj_add_event_cb(btn_rf, btn_refresh_click, LV_EVENT_CLICKED, NULL);

    list_log = lv_obj_create(scr);
    lv_obj_set_size(list_log, 300, 172);
    lv_obj_align(list_log, LV_ALIGN_TOP_MID, 0, 66);
    ui_style_card(list_log, true);
    lv_obj_set_flex_flow(list_log, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(list_log, 2, 0);
    lv_obj_set_style_pad_row(list_log, 0, 0);

    return scr;
}

void page_log_open(void)
{
    if (scr_log == NULL)
        scr_log = page_log_create();
    lv_scr_load(scr_log);
    page_log_refresh();
}

/* 从写指针（空白地址）倒序读，最多 LOG_ROWS 条；新日志置顶 */
void page_log_refresh(void)
{
    if (list_log == NULL) return;
    lv_obj_clean(list_log);

    Log_Obj log_tmp;
    uint32_t offset = log_write_offset;
    char buf[112];
    const char *level_str;
    uint32_t level_color;
    uint8_t cnt = 0;

    while (offset >= LOG_OBJ_SIZE && cnt < LOG_ROWS)
    {
        offset -= LOG_OBJ_SIZE;
        if (Log_ReadOne(offset, &log_tmp) == 0)
        {
            uint32_t sec = log_tmp.time_stamp / 1000U;
            uint32_t h = sec / 3600U, m = (sec % 3600U) / 60U, s = sec % 60U;
            switch (log_tmp.level)
            {
                case LOG_INFO:  level_str = "INFO";  level_color = C_TEXT; break;
                case LOG_WARN:  level_str = "WARN";  level_color = C_WARN; break;
                case LOG_ERROR: level_str = "ERROR"; level_color = C_FAIL; break;
                default:        level_str = "UNK";   level_color = C_DIM;  break;
            }
            /* T+ 前缀：这条时间戳是"上电以来的运行时长"，不是墙上时间 */
            snprintf(buf, sizeof(buf), "T+%02u:%02u:%02u %-5s %s",
                     (unsigned)h, (unsigned)m, (unsigned)s, level_str, log_tmp.content);
            add_log_row(buf, level_color, UI_FONT_ASCII);
            cnt++;
        }
    }

    if (cnt == 0)
        add_log_row("暂无日志", C_DIM, UI_FONT_CJK);

    if (label_count)
        lv_label_set_text_fmt(label_count, "共 %u 条", (unsigned)cnt);
}
