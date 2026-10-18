/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    rtc.c
  * @brief   This file provides code for the configuration
  *          of the RTC instances.
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
/* Includes ------------------------------------------------------------------*/
#include "rtc.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

RTC_HandleTypeDef hrtc;

/* RTC init function */
void MX_RTC_Init(void)
{

  /* USER CODE BEGIN RTC_Init 0 */

  /* USER CODE END RTC_Init 0 */

  RTC_TimeTypeDef sTime = {0};
  RTC_DateTypeDef sDate = {0};

  /* USER CODE BEGIN RTC_Init 1 */

  /* USER CODE END RTC_Init 1 */

  /** Initialize RTC Only
  */
  hrtc.Instance = RTC;
  hrtc.Init.HourFormat = RTC_HOURFORMAT_24;
  hrtc.Init.AsynchPrediv = 127;
  hrtc.Init.SynchPrediv = 255;
  hrtc.Init.OutPut = RTC_OUTPUT_DISABLE;
  hrtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
  hrtc.Init.OutPutType = RTC_OUTPUT_TYPE_OPENDRAIN;
  if (HAL_RTC_Init(&hrtc) != HAL_OK)
  {
    Error_Handler();
  }

  /* USER CODE BEGIN Check_RTC_BKUP */
  /* 备份寄存器标记在位 = 备份域保住了内容（有 VBAT 电池时）。
   * 注意"内容还在"不等于"时间还在走"：只有 RTC 时钟源是 LSE（在备份域）
   * 时，断电期间时间才继续前进；走 LSI 的话时间停在断电那一刻，由
   * app/clock.c 的 Clock_Init 用 RTC_UsesLse() 判掉、不当成已校时。
   * 无论哪种，这里都绝不能走 CubeMX 默认的"无条件把时间清零"（会毁掉
   * 断电保持的时间），直接保留 RTC 现有值，由 Clock_Init() 读走。
   * 常量取 User/app/clock.h 的 CLOCK_BKP_MAGIC —— 曾经两边各自定义
   * （"CLKP" vs "CLK2"）不一致，守卫永远不命中，RTC 全靠 DBP=0 的写保护
   * 兜底才没被清零；现在统一为单一事实来源。 */
  if (RTC->BKP0R == CLOCK_BKP_MAGIC) {
      /* 提前 return 会错过函数末尾 USER CODE 区的唤醒定时器关闭，这里补上。
       * RTC 寄存器写在 DBP=0 时会被静默丢弃，先放开再关。 */
      HAL_PWR_EnableBkUpAccess();
      HAL_RTCEx_DeactivateWakeUpTimer(&hrtc);
      HAL_PWR_DisableBkUpAccess();
      HAL_NVIC_DisableIRQ(RTC_WKUP_IRQn);
      return;
  }
  /* USER CODE END Check_RTC_BKUP */

  /** Initialize RTC and set the Time and Date
  */
  sTime.Hours = 0x0;
  sTime.Minutes = 0x0;
  sTime.Seconds = 0x0;
  sTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
  sTime.StoreOperation = RTC_STOREOPERATION_RESET;
  if (HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BCD) != HAL_OK)
  {
    Error_Handler();
  }
  sDate.WeekDay = RTC_WEEKDAY_MONDAY;
  sDate.Month = RTC_MONTH_JANUARY;
  sDate.Date = 0x1;
  sDate.Year = 0x0;

  if (HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BCD) != HAL_OK)
  {
    Error_Handler();
  }

  /** Enable the WakeUp
  */
  if (HAL_RTCEx_SetWakeUpTimer_IT(&hrtc, 0, RTC_WAKEUPCLOCK_RTCCLK_DIV16) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN RTC_Init 2 */
  /* CubeMX 默认开了 RTC 唤醒中断（counter=0、RTCCLK/16 ≈ 每 0.5ms 一次），
   * 全工程没有任何回调消费它 —— 纯粹烧 CPU，直接关掉。
   * IRQ 处理函数保留，将来要用唤醒定时器时有现成入口。 */
  HAL_RTCEx_DeactivateWakeUpTimer(&hrtc);
  HAL_NVIC_DisableIRQ(RTC_WKUP_IRQn);
  /* USER CODE END RTC_Init 2 */

}

void HAL_RTC_MspInit(RTC_HandleTypeDef* rtcHandle)
{

  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};
  if(rtcHandle->Instance==RTC)
  {
  /* USER CODE BEGIN RTC_MspInit 0 */

  /* USER CODE END RTC_MspInit 0 */

  /** Initializes the peripherals clock
  */
    PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_RTC;
    PeriphClkInitStruct.RTCClockSelection = RCC_RTCCLKSOURCE_LSE;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
    {
      Error_Handler();
    }

    /* RTC clock enable */
    __HAL_RCC_RTC_ENABLE();

    /* RTC interrupt Init */
    HAL_NVIC_SetPriority(RTC_WKUP_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(RTC_WKUP_IRQn);
  /* USER CODE BEGIN RTC_MspInit 1 */

  /* USER CODE END RTC_MspInit 1 */
  }
}

void HAL_RTC_MspDeInit(RTC_HandleTypeDef* rtcHandle)
{

  if(rtcHandle->Instance==RTC)
  {
  /* USER CODE BEGIN RTC_MspDeInit 0 */

  /* USER CODE END RTC_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_RTC_DISABLE();

    /* RTC interrupt Deinit */
    HAL_NVIC_DisableIRQ(RTC_WKUP_IRQn);
  /* USER CODE BEGIN RTC_MspDeInit 1 */

  /* USER CODE END RTC_MspDeInit 1 */
  }
}

/* USER CODE BEGIN 1 */

uint8_t RTC_UsesLse(void)
{
    /* RTCSEL[1:0]：00=无时钟，01=LSE，10=LSI，11=HSE。
     * 只有 LSE 在备份域里 —— 判据是"时钟源能不能在 VDD 掉电后继续供电"，
     * 不是"这个源准不准"。HSE/128 那种方案精度虽好，但同样在 VDD 域，
     * 所以这里必须返回 0，调用方才不会把停走的时间当成已校时。 */
    return (uint8_t)((RCC->BDCR & RCC_BDCR_RTCSEL) == RCC_BDCR_RTCSEL_0);
}

/* USER CODE END 1 */

