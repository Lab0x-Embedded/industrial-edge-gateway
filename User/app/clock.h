#ifndef CLOCK_H
#define CLOCK_H
/* ============================================================
 *  时间服务：给界面提供"现在几点、今天几号"
 *
 *  为什么不用 RTC 直接走时：RTC 的时钟源是 LSI（见 Core/Src/rtc.c 与 .ioc
 *  里的 RCC_RTC_Clock_Source=LSI），LSI 典型误差 ±5%，做分钟级正确的表
 *  几十分钟就会偏出分钟。而 HAL 时基来自 HSE 8MHz 晶振（ppm 级），
 *  所以这里的做法是：
 *    走时 = HAL_GetTick() 相对某个基准点的偏移（HSE 精度）
 *    绝对零点 = ESP8266 SNTP 联网校时（wifi_conn.c 的 wifi_time_sync_locked）
 *    RTC = 只用做"断电期间还行不行"的兜底 + 上电初值（板上有 VBAT 电池时）
 * ============================================================ */
#include <stdint.h>

/* 备份寄存器标记：写在 RTC 备份域 BKP0R 里，标记"RTC 里是校过时写入的成套数据"。
 * Core/Src/rtc.c 的 MX_RTC_Init 也用它做"保留 RTC 现有时间、跳过默认时间写入"的
 * 守卫 —— 两边必须同一个值（历史上曾各自定义 "CLKP"/"CLK2" 不一致，守卫永远不
 * 命中，RTC 全靠 DBP=0 写保护兜底才没被清零）。
 * 为什么是 v3（"CLK2"）：v1/v2 时代 DBP 从未被打开（见 SetDateTime 里的说明），
 * RTC 寄存器其实从没被真正写过（里面是备份域默认值 2000-01-01）但魔数却打上了
 * —— 那种备份一律不认，强制重新校时。 */
#define CLOCK_BKP_MAGIC  0x324B4C43UL   /* "CLK2"，v3 = DBP 放开后写入的成套数据 */

void    Clock_Init(void);                                  /* 上电调用一次：定初值 */
/* 校时：日期+时间一起给（SNTP 应答本来就带全量），更新走时 + 写 RTC 日期/时间 + 打备份标记 */
void    Clock_SetDateTime(uint16_t y, uint8_t mo, uint8_t d,
                          uint8_t h, uint8_t m, uint8_t s);
void    Clock_GetTime(uint8_t *h, uint8_t *m, uint8_t *s);
void    Clock_GetDateTime(uint16_t *y, uint8_t *mo, uint8_t *d,
                          uint8_t *h, uint8_t *m, uint8_t *s);
uint8_t Clock_IsSynced(void);                              /* 1 = 已经校过时（SNTP/手动） */

#endif
