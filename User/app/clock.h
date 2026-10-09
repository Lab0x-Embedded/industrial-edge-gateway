#ifndef CLOCK_H
#define CLOCK_H
/* ============================================================
 *  时间服务：给界面提供"现在几点"
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

void    Clock_Init(void);                                  /* 上电调用一次：定初值 */
void    Clock_SetTime(uint8_t h, uint8_t m, uint8_t s);    /* 校时：更新走时 + 写 RTC + 打备份标记 */
void    Clock_GetTime(uint8_t *h, uint8_t *m, uint8_t *s);
uint8_t Clock_IsSynced(void);                              /* 1 = 已经校过时（SNTP/手动） */

#endif
