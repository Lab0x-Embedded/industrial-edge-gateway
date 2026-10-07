/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
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
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define LED0_Pin GPIO_PIN_3
#define LED0_GPIO_Port GPIOE
#define LED1_Pin GPIO_PIN_4
#define LED1_GPIO_Port GPIOE
#define LCD_BL_Pin GPIO_PIN_10
#define LCD_BL_GPIO_Port GPIOF
#define TP_CLK_Pin GPIO_PIN_5
#define TP_CLK_GPIO_Port GPIOA
#define TP_CS_Pin GPIO_PIN_0
#define TP_CS_GPIO_Port GPIOB
#define TP_MOSI_Pin GPIO_PIN_1
#define TP_MOSI_GPIO_Port GPIOB
#define TP_MISO_Pin GPIO_PIN_2
#define TP_MISO_GPIO_Port GPIOB
#define TP_PEN_Pin GPIO_PIN_11
#define TP_PEN_GPIO_Port GPIOF
#define RS485_DE_Pin GPIO_PIN_6
#define RS485_DE_GPIO_Port GPIOG
#define W25Q_CS_Pin GPIO_PIN_8
#define W25Q_CS_GPIO_Port GPIOG
#define LED2_Pin GPIO_PIN_9
#define LED2_GPIO_Port GPIOG

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
