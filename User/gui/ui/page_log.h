#ifndef PAGE_LOG_H
#define PAGE_LOG_H
#include "lvgl.h"

lv_obj_t *page_log_create(void);
void page_log_refresh(void);
void page_log_open(void);      /* 设置页入口：创建/加载 + 刷新列表 */

#endif
