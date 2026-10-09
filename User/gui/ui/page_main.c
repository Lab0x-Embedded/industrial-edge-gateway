#include "lvgl.h"
#include "tasks.h" 
#include "stdio.h"
#include "stm32f4xx.h"
#include "page_settings.h"

// 字体声明
extern lv_font_t font_chinese_9;


lv_obj_t *bar_light;
lv_obj_t *label_light;

lv_obj_t *page_main_create(void);
static void btn_click_jump_settings(lv_event_t *e);
/* 全局控件 */
static lv_obj_t *label_temp;
static lv_obj_t *label_humi;
static lv_obj_t *label_status;
static lv_obj_t *label_uptime;
static lv_obj_t *bar_temp;
static lv_obj_t *bar_humi;
static lv_obj_t *btn_test;  // 按钮句柄

/* 设置按钮事件回调 */
static void btn_click_jump_settings(lv_event_t *e)
{
    (void)e;
    page_settings_open();
}

/**
 * @brief  创建主监控页面
 * @retval 返回页面根对象
 */
lv_obj_t *page_main_create(void)
{
    // 创建独立屏幕对象，NULL 表示不依附默认屏幕
    lv_obj_t *scr = lv_obj_create(NULL);			

    // 设置屏幕背景色：浅灰蓝
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xE8EEF5), 0);
    // 禁用屏幕整体滑动功能
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);							
    // 关闭滚动条显示
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);				 

    // ===================== 顶部主标题 =====================
    lv_obj_t *title = lv_label_create(scr);                 // 创建文本标签
    lv_label_set_text(title, "边缘网关监控");               // 设置标题文字
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);            // 顶部居中，Y偏移4
    lv_obj_set_style_text_color(title, lv_color_hex(0x2D3748), 0); // 文字深灰
    lv_obj_set_style_text_font(title, &font_chinese_9, 0);   // 设置中文字体

    // 定义三张卡片宽度、高度，320屏无缝均分
    const int card_w1 = 106;
    const int card_w2 = 106;
    const int card_w3 = 108;
    const int card_h = 90;

    // 计算卡片组水平居中偏移量
    int group_x = (320 - (card_w1 + card_w2 + card_w3)) / 2;
    // 卡片Y坐标：底部状态栏(32高)上方，预留间距
    int base_y = 240 - 32 - card_h - 42;

    // ===================== 左侧 温度卡片 =====================
    lv_obj_t *card_temp = lv_obj_create(scr);               // 创建卡片容器
    lv_obj_set_size(card_temp, card_w1, card_h);            // 设置卡片尺寸
    lv_obj_align(card_temp, LV_ALIGN_TOP_LEFT, group_x, base_y); // 定位卡片
    lv_obj_clear_flag(card_temp, LV_OBJ_FLAG_SCROLLABLE);   // 禁止卡片内部滑动
    lv_obj_set_style_bg_color(card_temp, lv_color_hex(0xFFFFFF), 0); // 卡片白底
    lv_obj_set_style_border_width(card_temp, 1, 0);         // 边框宽度1px
    lv_obj_set_style_border_color(card_temp, lv_color_hex(0xC9D8E8), 0); // 边框浅灰
    lv_obj_set_style_radius(card_temp, 0, 0);               // 直角卡片

    // 卡片顶部标签：温度
    lv_obj_t *icon_temp = lv_label_create(card_temp);
    lv_label_set_text(icon_temp, "温度");
    lv_obj_align(icon_temp, LV_ALIGN_TOP_MID, 0, 4);         // 靠近卡片顶部
    lv_obj_set_style_text_color(icon_temp, lv_color_hex(0xE53E3E), 0); // 红色文字
    lv_obj_set_style_text_font(icon_temp, &font_chinese_9, 0);

    // 温度数值显示标签
    label_temp = lv_label_create(card_temp);
    lv_label_set_text(label_temp, "--.-C");
    lv_obj_align(label_temp, LV_ALIGN_TOP_MID, 0, 32);
    lv_obj_set_style_text_color(label_temp, lv_color_hex(0xE53E3E), 0);
    lv_obj_set_style_text_font(label_temp, &font_chinese_9, 0);

    // 温度进度条
    bar_temp = lv_bar_create(card_temp);
    lv_obj_set_size(bar_temp, 85, 8);                       // 进度条尺寸
    lv_obj_align(bar_temp, LV_ALIGN_BOTTOM_MID, 0, 0);      // 紧贴卡片底部
    lv_bar_set_range(bar_temp, -10, 60);                    // 进度条量程
    lv_bar_set_value(bar_temp, 0, LV_ANIM_ON);              // 初始值0，开启动画
    lv_obj_set_style_bg_color(bar_temp, lv_color_hex(0xE2E8F0), LV_PART_MAIN); // 轨道底色
    lv_obj_set_style_bg_color(bar_temp, lv_color_hex(0xE53E3E), LV_PART_INDICATOR); // 进度色
    lv_obj_set_style_radius(bar_temp, 2, 0);                // 进度条圆角

    // ===================== 中间 光照卡片 =====================
    lv_obj_t *card_light = lv_obj_create(scr);
    lv_obj_set_size(card_light, card_w2, card_h);
    lv_obj_align(card_light, LV_ALIGN_TOP_LEFT, group_x + card_w1, base_y);
    lv_obj_clear_flag(card_light, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card_light, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(card_light, 1, 0);
    lv_obj_set_style_border_color(card_light, lv_color_hex(0xC9D8E8), 0);
    lv_obj_set_style_radius(card_light, 0, 0);

    // 卡片顶部标签：光照
    lv_obj_t *icon_light = lv_label_create(card_light);
    lv_label_set_text(icon_light, "光照");
    lv_obj_align(icon_light, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_text_color(icon_light, lv_color_hex(0xD69E00), 0); // 黄色文字
    lv_obj_set_style_text_font(icon_light, &font_chinese_9, 0);

    // 光照数值显示标签
    label_light = lv_label_create(card_light);
    lv_label_set_text(label_light, "----");
    lv_obj_align(label_light, LV_ALIGN_TOP_MID, 0, 32);
    lv_obj_set_style_text_color(label_light, lv_color_hex(0xD69E00), 0);
    lv_obj_set_style_text_font(label_light, &font_chinese_9, 0);

    // 光照进度条
    bar_light = lv_bar_create(card_light);
    lv_obj_set_size(bar_light, 85, 8);
    lv_obj_align(bar_light, LV_ALIGN_BOTTOM_MID, 0, 0);      // 紧贴卡片底部
    lv_bar_set_range(bar_light, 0, 20000);                  // 光照量程 0~20000
    lv_bar_set_value(bar_light, 0, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_light, lv_color_hex(0xE2E8F0), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_light, lv_color_hex(0xD69E00), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_light, 2, 0);

    // ===================== 右侧 湿度卡片 =====================
    lv_obj_t *card_humi = lv_obj_create(scr);
    lv_obj_set_size(card_humi, card_w3, card_h);
    lv_obj_align(card_humi, LV_ALIGN_TOP_LEFT, group_x + card_w1 + card_w2, base_y);
    lv_obj_clear_flag(card_humi, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card_humi, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(card_humi, 1, 0);
    lv_obj_set_style_border_color(card_humi, lv_color_hex(0xC9D8E8), 0);
    lv_obj_set_style_radius(card_humi, 0, 0);

    // 卡片顶部标签：湿度
    lv_obj_t *icon_humi = lv_label_create(card_humi);
    lv_label_set_text(icon_humi, "湿度");
    lv_obj_align(icon_humi, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_text_color(icon_humi, lv_color_hex(0x319795), 0); // 青绿色文字
    lv_obj_set_style_text_font(icon_humi, &font_chinese_9, 0);

    // 湿度数值显示标签
    label_humi = lv_label_create(card_humi);
    lv_label_set_text(label_humi, "--.-%");
    lv_obj_align(label_humi, LV_ALIGN_TOP_MID, 0, 32);
    lv_obj_set_style_text_color(label_humi, lv_color_hex(0x319795), 0);
    lv_obj_set_style_text_font(label_humi, &font_chinese_9, 0);

    // 湿度进度条
    bar_humi = lv_bar_create(card_humi);
    lv_obj_set_size(bar_humi, 85, 8);
    lv_obj_align(bar_humi, LV_ALIGN_BOTTOM_MID, 0, 0);      // 紧贴卡片底部
    lv_bar_set_range(bar_humi, 0, 100);                    // 湿度量程 0~100%
    lv_bar_set_value(bar_humi, 0, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_humi, lv_color_hex(0xE2E8F0), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_humi, lv_color_hex(0x319795), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_humi, 2, 0);

    // ===================== 底部状态栏 =====================
    lv_obj_t *footer = lv_obj_create(scr);
    lv_obj_set_width(footer, lv_obj_get_width(scr));         // 宽度铺满整屏
    lv_obj_set_height(footer, 32);                          // 状态栏高度32
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, 0);        // 贴屏幕底部
    lv_obj_set_style_bg_color(footer, lv_color_hex(0x121A2C), 0); // 深色背景
    lv_obj_set_style_radius(footer, 0, 0);
    lv_obj_set_style_border_width(footer, 1, 0);
    lv_obj_set_style_border_color(footer, lv_color_hex(0x121A2C), 0);
    lv_obj_clear_flag(footer, LV_OBJ_FLAG_SCROLLABLE);       // 禁止状态栏滑动
    lv_obj_set_scrollbar_mode(footer, LV_SCROLLBAR_MODE_OFF);			//关闭滑动条

    // 状态栏左侧：WiFi 连接状态（LVGL 内置 WiFi 图标 + 英文，不占中文字库）
    label_status = lv_label_create(footer);
    lv_label_set_text(label_status, LV_SYMBOL_WIFI" OFF");
    lv_obj_align(label_status, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_set_style_text_color(label_status, lv_color_hex(0xFF6B6B), 0);
    lv_obj_set_style_text_font(label_status, &lv_font_montserrat_14, 0);

    // 右侧：系统运行时长
    label_uptime = lv_label_create(footer);
    lv_label_set_text(label_uptime, "运行 00:00:00");
    lv_obj_align(label_uptime, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_set_style_text_color(label_uptime, lv_color_hex(0xC9D8E8), 0);
    lv_obj_set_style_text_font(label_uptime, &font_chinese_9, 0);

    // ===================== 右上角 设置按钮 =====================
    btn_test = lv_btn_create(scr);
    lv_obj_set_size(btn_test, 65, 35);
    lv_obj_align(btn_test, LV_ALIGN_TOP_RIGHT, -3, 3);
    lv_obj_set_style_bg_color(btn_test, lv_color_hex(0x4A90E2), 0);
    lv_obj_set_style_radius(btn_test, 4, 0);

    lv_obj_t *lab_btn = lv_label_create(btn_test);
    lv_label_set_text(lab_btn, LV_SYMBOL_SETTINGS);
    lv_obj_align(lab_btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(lab_btn, lv_color_white(), 0);

    /* 设置中枢：WiFi 配置 / 系统日志 / 关于设备 */
    lv_obj_add_event_cb(btn_test, btn_click_jump_settings, LV_EVENT_CLICKED, NULL);

    // 返回当前页面句柄
    return scr;
}
extern TickType_t idle_tick;
extern uint8_t lcd_bl_en ; //1亮屏 0熄屏
// 更新函数
void page_main_update_data(float temp, float humi, float light)
{
    static float temp_last=-999;
    static float humi_last=-999;
    static float light_last=-99999;

		//数据变化才更新
    if(temp!=temp_last || humi!=humi_last || light!=light_last)
    {
        lv_label_set_text_fmt(label_temp, "%.1fC", temp);
        lv_label_set_text_fmt(label_humi, "%.1f%%", humi);
        lv_label_set_text_fmt(label_light, "%.0fL", light);

				//改变进度条的值
        lv_bar_set_value(bar_temp, (int)temp, LV_ANIM_ON);
        lv_bar_set_value(bar_humi, (int)humi, LV_ANIM_ON);
        lv_bar_set_value(bar_light, (int)light, LV_ANIM_ON);

			if((temp-temp_last)>3 || (temp-temp_last)<-3 || (humi-humi_last)>3 || (humi-humi_last)<-3|| (light-light_last)>10 || (light-light_last)<-10)
			{
			//数据改变后亮屏
			      idle_tick = xTaskGetTickCount();
            HAL_GPIO_WritePin(GPIOF,GPIO_PIN_10,GPIO_PIN_SET);
            lcd_bl_en = 1;
			}
			
			
        temp_last=temp;
        humi_last=humi;
        light_last=light;
			
    }
}



// 更新连接状态
void page_main_update_status(uint8_t wifi, uint8_t mqtt)
{
	if(!label_status) return;																									//创建成功为1 label_status 底部部件文本
    if(!wifi)
    {
        lv_label_set_text(label_status, LV_SYMBOL_WIFI" OFF");
        lv_obj_set_style_text_color(label_status, lv_color_hex(0xFF6B6B), 0);
    }
    else if(!mqtt)
    {
        lv_label_set_text(label_status, LV_SYMBOL_WIFI" NO CLOUD");
        lv_obj_set_style_text_color(label_status, lv_color_hex(0xF6AD55), 0);
    }
    else
    {
        lv_label_set_text(label_status, LV_SYMBOL_WIFI" ONLINE");
        lv_obj_set_style_text_color(label_status, lv_color_hex(0x2ECC71), 0);
    }
}

// 更新运行时间
void page_main_update_uptime(uint32_t seconds)
{
    if(!label_uptime) return;
    uint32_t h = seconds / 3600;
    uint32_t m = (seconds % 3600) / 60;
    uint32_t s = seconds % 60;
    lv_label_set_text_fmt(label_uptime, "运行 %02u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)s);
}


