/* 关于页 · 深色：固件版本/板卡/LVGL/Flash/云平台/运行时长/IP（时长与 IP 每秒刷新） */
#include "page_about.h"
#include "page_settings.h"
#include "app_version.h"
#include "cloud.h"
#include "wifi_conn.h"
#include "w25qxx.h"
#include "stm32f4xx.h"
#include <stdio.h>

#define C_BG     0x0F172A
#define C_CARD   0x1E293B
#define C_TRACK  0x334155
#define C_TEXT   0xE2E8F0
#define C_DIM    0x94A3B8

static lv_obj_t *scr_about = NULL;
static lv_obj_t *list_about = NULL;
/* 动态行直接持有标签指针刷新。
 * 旧版按"列表第 N 项的子对象"索引取标签，而 lv_list_add_text 返回的就是
 * 标签本身（没有子对象）→ 取到 NULL → 空指针写 → 进页面 1 秒后死机。
 * 教训：LVGL 树操作要么存返回值，要么 lv_obj_get_child 前确认层级。 */
static lv_obj_t *label_up = NULL, *label_ip = NULL;
static lv_timer_t *about_timer = NULL;

static void btn_back_click(lv_event_t *e)
{
    (void)e;
    page_settings_open();
}

static void about_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (lv_scr_act() != scr_about) return;

    uint32_t sec = HAL_GetTick() / 1000U;
    lv_label_set_text_fmt(label_up, LV_SYMBOL_SETTINGS " Up: %02u:%02u:%02u",
                          (unsigned)(sec / 3600), (unsigned)((sec % 3600) / 60),
                          (unsigned)(sec % 60));
    lv_label_set_text_fmt(label_ip, LV_SYMBOL_WIFI " IP: %s", g_esp_ip);
}

void page_about_open(void)
{
    if (scr_about == NULL)
    {
        char buf[96];
        scr_about = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(scr_about, lv_color_hex(C_BG), 0);
        lv_obj_clear_flag(scr_about, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *btn_back = lv_btn_create(scr_about);
        lv_obj_set_size(btn_back, 60, 30);
        lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 5, 6);
        lv_obj_set_style_bg_color(btn_back, lv_color_hex(C_TRACK), 0);
        lv_obj_set_style_radius(btn_back, 6, 0);

        lv_obj_t *lab_back = lv_label_create(btn_back);
        lv_label_set_text(lab_back, LV_SYMBOL_LEFT);
        lv_obj_align(lab_back, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_text_color(lab_back, lv_color_hex(C_TEXT), 0);
        lv_obj_add_event_cb(btn_back, btn_back_click, LV_EVENT_CLICKED, NULL);

        lv_obj_t *title = lv_label_create(scr_about);
        lv_label_set_text(title, LV_SYMBOL_OK " About");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

        list_about = lv_list_create(scr_about);
        lv_obj_set_size(list_about, 300, 165);
        lv_obj_align(list_about, LV_ALIGN_BOTTOM_MID, 0, -8);
        lv_obj_set_style_bg_color(list_about, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_border_color(list_about, lv_color_hex(C_TRACK), 0);
        lv_obj_set_style_radius(list_about, 8, 0);
        lv_obj_set_style_text_color(list_about, lv_color_hex(C_TEXT), 0);

        lv_list_add_text(list_about, "FW: v" FW_VERSION_STRING "  " FW_BUILD_DATE);
        lv_list_add_text(list_about, "Board: " HW_BOARD_NAME);
        snprintf(buf, sizeof(buf), "LVGL: %d.%d.%d   320x240",
                 LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
        lv_list_add_text(list_about, buf);
        snprintf(buf, sizeof(buf), "Flash: %06lX (W25Q128 16MB)",
                 (unsigned long)SPI_FLASH_ReadID());
        lv_list_add_text(list_about, buf);
        snprintf(buf, sizeof(buf), "Cloud: %s", CLOUD_PROFILE_NAME);
        lv_list_add_text(list_about, buf);
        label_up = lv_list_add_text(list_about, LV_SYMBOL_SETTINGS " Up: --:--:--");
        label_ip = lv_list_add_text(list_about, LV_SYMBOL_WIFI " IP: --");

        about_timer = lv_timer_create(about_timer_cb, 1000, NULL);
    }
    lv_scr_load(scr_about);
}
