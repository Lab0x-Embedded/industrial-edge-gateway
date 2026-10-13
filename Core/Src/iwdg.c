/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    iwdg.c
  * @brief   This file provides code for the configuration
  *          of the IWDG instances.
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
#include "iwdg.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

IWDG_HandleTypeDef hiwdg;

/* IWDG init function */
void MX_IWDG_Init(void)
{

  /* USER CODE BEGIN IWDG_Init 0 */

  /* USER CODE END IWDG_Init 0 */

  /* USER CODE BEGIN IWDG_Init 1 */

  /* 超时窗口是换算出来的，代码里没有"20"这个数（CubeMX 界面上显示的 20.0s 就是它）：
   *   T = (Reload + 1) × Prescaler / f_LSI = 2500 × 256 / 32000 Hz = 20.0 s
   * （Reload 写 2500-1 是寄存器约定：写进 RLR 的值 = 重装次数-1）
   * 注意 f_LSI 是内部 RC，手册(DS8626)只保证 17~47 kHz（typ 32k）→ 实际窗口
   * 13.6~37.6 s。所以"复位周期≈20s 是看门狗在收尸"只能当量级判断；
   * 也正因 LSI 这个漂移，走时不用 RTC(LSI) 而用 HAL 时基 + SNTP 校时。
   * 喂狗在 User/app/tasks.c 的 Dog_task：每 3s 喂一次（余量 20/3≈6.7 倍）。 */

  /* USER CODE END IWDG_Init 1 */
  hiwdg.Instance = IWDG;
  hiwdg.Init.Prescaler = IWDG_PRESCALER_256;
  hiwdg.Init.Reload = 2500-1;
  if (HAL_IWDG_Init(&hiwdg) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN IWDG_Init 2 */

  /* USER CODE END IWDG_Init 2 */

}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

