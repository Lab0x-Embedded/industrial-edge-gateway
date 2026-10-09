#ifndef PAGE_MAIN_H
#define PAGE_MAIN_H

#include "lvgl.h"
lv_obj_t *page_main_create(void);
void page_main_update_data(float temp, float humi, float light);
void page_main_update_status(uint8_t wifi, uint8_t mqtt);
void page_main_update_uptime(uint32_t seconds);
/* 实时时间：synced=0 表示还没校过时（SNTP 没成功 / 没手动设过），
 * 界面把这种"近似时间"显示成暗色，避免看着像准的假时间。 */
void page_main_update_clock(uint8_t h, uint8_t m, uint8_t s, uint8_t synced);
#endif