/* 关于设备页 · 深色：设备 + 网络信息面板（IP / 运行时长 / 当前时间每秒刷新）
 *
 * 为什么不用 lv_list：lv_list 行的底色来自 LVGL 默认浅色主题，压在深色卡片上
 * 一块块发白 —— 用户反馈的"背景色很奇怪"就是这个。这版改成自绘键值行：
 * 透明行（lv_obj_remove_style_all）+ 左键暗色 + 右值亮色，flex 纵向排列，
 * 不再有任何主题残留。
 *
 * 动态行直接持有右值标签指针刷新。旧版按"列表第 N 项的子对象"索引取标签，
 * 而 lv_list_add_text 返回的就是标签本身（没有子对象）→ 取到 NULL → 空指针写
 * → 进页面 1 秒后死机。教训：LVGL 树操作要么存返回值，要么确认层级再取。 */
#include "page_about.h"
#include "page_settings.h"
#include "ui_theme.h"
#include "app_version.h"
#include "clock.h"
#include "cloud.h"
#include "wifi_conn.h"
#include "w25qxx.h"
#include "stm32f4xx.h"
#include <stdio.h>

static lv_obj_t *scr_about = NULL;
static lv_obj_t *label_ip = NULL, *label_up = NULL, *label_time = NULL;
static lv_timer_t *about_timer = NULL;

static void btn_back(void)
{
    page_settings_open();
}

static void about_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (lv_scr_act() != scr_about) return;

    uint32_t sec = HAL_GetTick() / 1000U;
    uint8_t hh, mm, ss;
    Clock_GetTime(&hh, &mm, &ss);

    lv_label_set_text_fmt(label_ip, "%s", g_esp_ip);
    lv_label_set_text_fmt(label_up, "%02u:%02u:%02u",
                          (unsigned)(sec / 3600), (unsigned)((sec % 3600) / 60),
                          (unsigned)(sec % 60));
    lv_label_set_text_fmt(label_time, "%02u:%02u:%02u", hh, mm, ss);
    /* 未校时的时间是暗色，避免和"准的时间"混淆 */
    lv_obj_set_style_text_color(label_time,
                                lv_color_hex(Clock_IsSynced() ? C_OK : C_DIM), 0);
}

void page_about_open(void)
{
    if (scr_about == NULL)
    {
        char buf[96];

        scr_about = lv_obj_create(NULL);
        ui_style_screen(scr_about);
        ui_title_bar(scr_about, "关于设备", btn_back, UI_FONT_CJK);

        lv_obj_t *card = lv_obj_create(scr_about);
        lv_obj_set_size(card, 300, 202);
        lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 38);
        ui_style_card(card, true);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);   /* 行高 24，8 行正好填满 */
        lv_obj_set_style_pad_all(card, 4, 0);
        lv_obj_set_style_pad_row(card, 0, 0);

        snprintf(buf, sizeof(buf), "v%s  %s", FW_VERSION_STRING, FW_BUILD_DATE);
        ui_add_kv_row(card, "FW VERSION", buf, C_TEXT);

        ui_add_kv_row(card, "BOARD", HW_BOARD_NAME, C_TEXT);

        snprintf(buf, sizeof(buf), "%d.%d.%d  320x240",
                 LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
        ui_add_kv_row(card, "LVGL", buf, C_TEXT);

        snprintf(buf, sizeof(buf), "%06lX  W25Q128 16MB",
                 (unsigned long)SPI_FLASH_ReadID());
        ui_add_kv_row(card, "FLASH", buf, C_TEXT);

        ui_add_kv_row(card, "CLOUD", CLOUD_PROFILE_NAME, C_TEXT);

        label_ip   = ui_add_kv_row(card, "IP",     "--", C_HUMI);
        label_up   = ui_add_kv_row(card, "UPTIME", "--:--:--", C_TEXT);
        label_time = ui_add_kv_row(card, "TIME",   "--:--:--", C_DIM);

        about_timer = lv_timer_create(about_timer_cb, 1000, NULL);
    }
    lv_scr_load(scr_about);
}
