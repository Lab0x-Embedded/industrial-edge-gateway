#ifndef UI_THEME_H
#define UI_THEME_H
/* ============================================================
 *  界面主题层：深色工业风的唯一色板 + 控件样式工厂
 *
 *  为什么要有这个文件：
 *   深色改版第一版把同一套色板（C_BG/C_CARD/...）复制到了 5 个页面里，
 *   改一个颜色要动 5 处；而且各页只给"自己的"控件打样式，LVGL 默认
 *   主题（浅色）的底/边/阴影就留在下拉弹出列表、列表项这些地方，
 *   表现是"关于页背景色很奇怪"。这里统一收口：
 *   页面只调 ui_* 系列函数，不再自己写色值。
 *
 *  注意：LVGL 默认主题是浅色（lv_conf.h 里 LV_THEME_DEFAULT_DARK 0），
 *  所以任何自绘容器要么在这里统一样式，要么调 lv_obj_remove_style_all()。
 * ============================================================ */
#include "lvgl.h"

/* ---- 石板深色三级灰阶 + 强调色（Tailwind slate/red/teal/amber）---- */
#define C_BG       0x0F172A   /* 屏幕背景 slate-900 */
#define C_CARD     0x1E293B   /* 卡片 slate-800 */
#define C_TRACK    0x334155   /* 轨道/边框 slate-700 */
#define C_TEXT     0xE2E8F0
#define C_DIM      0x94A3B8
#define C_BLUE     0x60A5FA   /* 主操作 */
#define C_OK       0x34D399
#define C_WARN     0xFBBF24
#define C_FAIL     0xF87171
#define C_TEMP     0xF87171   /* 温度 red-400 */
#define C_HUMI     0x2DD4BF   /* 湿度 teal-400 */
#define C_LIGHT    0xFBBF24   /* 光照 amber-400 */
#define C_DARK     0x0B1220   /* 比背景更深：用于按钮按下/键盘缝隙 */

/* ---- 中文字体（14px bpp4 抗锯齿，含界面用到的全部汉字 + ASCII）----
 * 生成方式见方案文档 Task 9：
 *   npx lv_font_conv@1.5.3 --bpp 4 --size 14 --no-compress --font alimamashuheiti.ttf
 *   --symbols "<界面汉字集>" --range 0x20-0x7E --format lvgl --lv-font-name font_cjk_14
 * 坑：这个字体**不含** LV_SYMBOL_* 图标字形（图标只在 LVGL 内置 montserrat 里），
 * 所以用 UI_FONT_CJK 的标签里绝对不能出现 LV_SYMBOL_xxx。 */
extern const lv_font_t font_cjk_14;
#define UI_FONT_CJK   (&font_cjk_14)
#define UI_FONT_ASCII (&lv_font_montserrat_14)

/* ---- 页面/控件样式工厂 ---- */
void ui_style_screen(lv_obj_t *scr);                       /* 背景色 + 禁滚动 + 关滚动条 */
void ui_title_bar(lv_obj_t *scr, const char *title,
                  void (*back_cb)(void), const lv_font_t *font);
void ui_style_card(lv_obj_t *obj, bool border);            /* 卡片底 + 圆角 8（可选 1px 描边） */
void ui_style_btn(lv_obj_t *btn, uint32_t bg, uint32_t fg);/* 圆角 6 按钮，含按下/禁用态 */
void ui_style_textarea(lv_obj_t *ta);                      /* 输入框 */
void ui_style_keyboard(lv_obj_t *kb);                       /* 键盘（含按键/按下/大写态） */
void ui_style_dropdown(lv_obj_t *dd);                      /* 下拉本体 **和弹出列表** */
lv_obj_t *ui_add_kv_row(lv_obj_t *parent, const char *key,
                        const char *value, uint32_t value_color);  /* 返回右值标签 */

#endif
