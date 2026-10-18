/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    rtc.h
  * @brief   This file contains all the function prototypes for
  *          the rtc.c file
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __RTC_H__
#define __RTC_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* USER CODE BEGIN Includes */
#include "clock.h"   /* CLOCK_BKP_MAGIC：与 app/clock.c 共用的备份域信任标记 */
/* USER CODE END Includes */

extern RTC_HandleTypeDef hrtc;

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

void MX_RTC_Init(void);

/* USER CODE BEGIN Prototypes */
/* RTC 的时钟源是否在备份域（本芯片上等价于"是否 LSE 驱动"）。
 *
 * 为什么这个查询要读寄存器、而不是读某个缓存变量：
 * 时钟源由 CubeMX 在 HAL_RTC_MspInit 里写 BDCR.RTCSEL，本工程允许在
 * LSI / LSE 之间切换（见 .ioc 的 RCC_RTC_Clock_Source）。读寄存器能自动
 * 跟随任意一路，不用多维护一份状态，也就不会出现"软件以为的时钟源和
 * 硬件的实际时钟源不一致"这种不报错的静默错误。
 *
 * 调用方（app/clock.c）拿它来判断 RTC 里的时间可不可信：
 *   1 = LSE 驱动：LSE 在备份域，VDD 断开、VBAT 供电时 RTC 继续走时，
 *                 断电期间的时间是连续真实的 → RTC 数据可当"已校时"。
 *   0 = LSI/HSE：二者都在 VDD 域，主电源一断就停振。备份寄存器里的内容
 *                 （含 BKP0R 魔数）还在，但时间停在了断电那一刻，可能比
 *                 现在慢几个小时 → 这种数据只能当"大概"，不能报已校时。
 * 注意它只回答"时钟源在不在备份域"，不回答"值准不准"：
 *      LSE 也可能是从没校过时的 2000-01-01，那是 CLOCK_BKP_MAGIC 管的另一件事。 */
uint8_t RTC_UsesLse(void);
/* USER CODE END Prototypes */

#ifdef __cplusplus
}
#endif

#endif /* __RTC_H__ */

