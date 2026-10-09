/* 关于设备页：固件版本 / 平台信息 / 芯片标识（OTA 的版本基建） */
#include "page_about.h"
#include "page_settings.h"
#include "app_version.h"
#include "cloud.h"
#include "wifi_conn.h"
#include "w25qxx.h"
#include "log.h"           /* log_write_offset 分区状态 */
#include "stm32f4xx.h"
#include <stdio.h>

extern lv_obj_t *main_scr;

static lv_obj_t *scr_about = NULL;
static lv_obj_t *list_about = NULL;
static lv_timer_t *about_timer = NULL;

static void btn_back_click(lv_event_t *e)
{
    (void)e;
    page_settings_open();
}

/* 运行时长/IP 动态刷新（仅当本页显示时） */
static void about_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (lv_scr_act() != scr_about || list_about == NULL) return;

    uint32_t sec = HAL_GetTick() / 1000U;
    char buf[96];
    snprintf(buf, sizeof(buf), LV_SYMBOL_SETTINGS " Up: %02u:%02u:%02u",
             (unsigned)(sec / 3600), (unsigned)((sec % 3600) / 60), (unsigned)(sec % 60));
    lv_obj_t *item = lv_obj_get_child(list_about, 5);   /* 第 6 行：运行时长 */
    if (item) lv_label_set_text(lv_obj_get_child(item, 0), buf);

    snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " IP: %s", g_esp_ip);
    item = lv_obj_get_child(list_about, 6);             /* 第 7 行：IP */
    if (item) lv_label_set_text(lv_obj_get_child(item, 0), buf);
}

static void add_row(lv_obj_t *list, const char *text)
{
    lv_list_add_text(list, text);
}

void page_about_open(void)
{
    if (scr_about == NULL)
    {
        char buf[96];
        scr_about = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(scr_about, lv_color_hex(0xF8F9FA), 0);
        lv_obj_clear_flag(scr_about, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *btn_back = lv_btn_create(scr_about);
        lv_obj_set_size(btn_back, 65, 35);
        lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 5, 5);
        lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x0F3460), 0);

        lv_obj_t *lab_back = lv_label_create(btn_back);
        lv_label_set_text(lab_back, LV_SYMBOL_LEFT " Back");
        lv_obj_align(lab_back, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_text_color(lab_back, lv_color_white(), 0);
        lv_obj_add_event_cb(btn_back, btn_back_click, LV_EVENT_CLICKED, NULL);

        lv_obj_t *title = lv_label_create(scr_about);
        lv_label_set_text(title, "About");
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

        list_about = lv_list_create(scr_about);
        lv_obj_set_size(list_about, 300, 160);
        lv_obj_align(list_about, LV_ALIGN_BOTTOM_MID, 0, -10);
        lv_obj_set_style_radius(list_about, 6, 0);

        snprintf(buf, sizeof(buf), "FW: v" FW_VERSION_STRING "  (" FW_BUILD_DATE ")");
        add_row(list_about, buf);
        snprintf(buf, sizeof(buf), "Board: " HW_BOARD_NAME);
        add_row(list_about, buf);
        snprintf(buf, sizeof(buf), "LVGL: %d.%d.%d   LCD: 320x240",
                 LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
        add_row(list_about, buf);
        snprintf(buf, sizeof(buf), "Flash: %06lX (W25Q128 16MB)",
                 (unsigned long)SPI_FLASH_ReadID());
        add_row(list_about, buf);
        snprintf(buf, sizeof(buf), "Cloud: %s", CLOUD_PROFILE_NAME);
        add_row(list_about, buf);
        add_row(list_about, LV_SYMBOL_SETTINGS " Up: --:--:--");   /* 定时刷新 */
        add_row(list_about, LV_SYMBOL_WIFI " IP: --");

        about_timer = lv_timer_create(about_timer_cb, 1000, NULL);
    }
    lv_scr_load(scr_about);
}
