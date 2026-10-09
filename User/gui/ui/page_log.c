/* 系统日志页：W25Q 日志分区倒序读取，最新置顶（从 page_main.c 迁出） */
#include "page_log.h"
#include "log.h"
#include "page_main.h"
#include "tasks.h"
#include "wifi_conn.h"
#include <stdio.h>

extern lv_font_t font_chinese_9;
extern lv_obj_t *main_scr;

static lv_obj_t *scr_log = NULL;
static lv_obj_t *list_log = NULL;

static void btn_back_click(lv_event_t *e)
{
    (void)e;
    lv_scr_load(main_scr);
    page_main_update_status(wifi_connected, g_mqtt_connected);   /* 返回后刷新主界面状态 */
}

lv_obj_t *page_log_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xF8F9FA), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *btn_back = lv_btn_create(scr);
    lv_obj_set_size(btn_back, 65, 35);
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 5, 5);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x0F3460), 0);

    lv_obj_t *lab_back = lv_label_create(btn_back);
    lv_label_set_text(lab_back, "返回");
    lv_obj_align(lab_back, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_font(lab_back, &font_chinese_9, 0);
    lv_obj_add_event_cb(btn_back, btn_back_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "系统运行日志");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_set_style_text_font(title, &font_chinese_9, 0);

    list_log = lv_list_create(scr);
    lv_obj_set_size(list_log, lv_obj_get_width(scr) - 10, lv_obj_get_height(scr) - 60);
    lv_obj_align(list_log, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_obj_set_style_radius(list_log, 6, 0);

    return scr;
}

void page_log_open(void)
{
    if (scr_log == NULL)
        scr_log = page_log_create();
    lv_scr_load(scr_log);
    page_log_refresh();
}

/* 从写指针（空白地址）倒序读，最多 30 条；新日志置顶 */
void page_log_refresh(void)
{
    if (list_log == NULL) return;
    lv_obj_clean(list_log);

    Log_Obj log_tmp;
    uint32_t offset = log_write_offset;
    char buf[128];
    const char *level_str;
    uint8_t cnt = 0;

    while (offset >= LOG_OBJ_SIZE && cnt < 30)
    {
        offset -= LOG_OBJ_SIZE;
        if (Log_ReadOne(offset, &log_tmp) == 0)
        {
            uint32_t sec = log_tmp.time_stamp / 1000U;
            uint32_t h = sec / 3600U, m = (sec % 3600U) / 60U, s = sec % 60U;
            switch (log_tmp.level)
            {
                case LOG_INFO:  level_str = "INFO";  break;
                case LOG_WARN:  level_str = "WARN";  break;
                case LOG_ERROR: level_str = "ERROR"; break;
                default:        level_str = "UNK";   break;
            }
            snprintf(buf, sizeof(buf), "%02u:%02u:%02u %s %s",
                     (unsigned)h, (unsigned)m, (unsigned)s, level_str, log_tmp.content);
            lv_list_add_text(list_log, buf);
            cnt++;
        }
    }

    if (cnt == 0)
        lv_list_add_text(list_log, "(no logs)");
}
