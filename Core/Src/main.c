/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
#include "main.h"
#include "cmsis_os.h"
#include "dma.h"
#include "iwdg.h"
#include "rtc.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"
#include "fsmc.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include "FreeRTOS.h"
#include "semphr.h"
#include "lcd.h"
#include "touch.h"
#include "modbus.h"
#include "esp_uart.h"
#include "w25qxx.h"
#include "log.h"
#include "cache.h"
#include "clock.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"
#include "lvgl.h"
#include "esp_uart.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
void MX_FREERTOS_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
uint8_t esp_rx_dma_buf[256];

#if DBG_PRINT_ENABLE
/* UART5 调试口互斥锁：所有任务都会经 Log_Write → dbg_print 用这条串口，
 * 不加锁时两个任务的输出按字节交错成乱行。在 MX_FREERTOS_Init 里创建；
 * 创建前（内核未起的极早期打印）为 NULL，直接发送、不加锁。 */
SemaphoreHandle_t dbg_uart_mutex = NULL;

/* 调试串口（UART5）：只打关键事件，不逐条刷发布。
 *
 * 两处都是有限超时，原因：
 * 1) 发送不再用 HAL_MAX_DELAY：一行最长 128B @115200 约 11ms，给 100ms 只兜
 *    硬件挂死。本函数会被持 esp_uart_mutex 的路径（AT 心跳 / MQTT 发布里的
 *    Log_Write）调到，无限阻塞等于放大临界区，和"缩短持锁时间"对着干。
 * 2) 取锁给 50ms 短超时：拿不到宁可丢这一行日志——调试口不能反过来拖死
 *    业务，丢一行 log 的代价远小于堵住持锁任务 50ms。 */
void dbg_print(const char *fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    /* vsnprintf 的返回值是"本该写多长"，**可能大于 buf 的大小**：
     * 原来直接把这个 n 交给 HAL_UART_Transmit，越界的部分会把栈上 128 字节之外的
     * 邻近内存当数据发出去（越界读）。必须自己截断。 */
    if (n > (int)sizeof(buf) - 1)
        n = (int)sizeof(buf) - 1;
    if (dbg_uart_mutex == NULL ||
        xSemaphoreTake(dbg_uart_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        HAL_UART_Transmit(&huart5, (uint8_t *)buf, (uint16_t)n, 100);
        if (dbg_uart_mutex) xSemaphoreGive(dbg_uart_mutex);
    }
}
#endif /* DBG_PRINT_ENABLE */
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* ★ 复位原因捕获 —— 必须放在这里，两个"必须"：
   *   ① 必须在 HAL_Init() **之后**：HAL_Init() 内部会调 HAL_MspInit，
   *      而真正清标志的是下面那句 __HAL_RCC_CLEAR_RESET_FLAGS()；
   *      更早读也是可以的，但放在 HAL_Init 之后能确保 RCC 已可用。
   *   ② 必须在 LOG 初始化**之前**读、在 Log_Init() **之后**写：
   *      此刻 Log_Init() 还没跑（它在 USER CODE 2 里），直接 Log_Write
   *      会写进未初始化的日志区，所以先存进一个变量中转。
   *
   * 为什么需要它：看门狗复位和上电复位在现象上完全一样（都是"设备重启了"），
   * 没有这一行就永远不知道现场到底是被狗咬的还是掉电。日志页里能看到
   * "reset by IWDG" 和 "power-on reset" 的区别，是排查"莫名重启"的第一条线索。
   *
   * 注意读的顺序：POR/PIN/BOR 这些标志是"累积"的，不清除会一直置位；
   * 所以先把关心的都读出来，再统一清一次。 */
  uint8_t rst_iwdg = (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST) != RESET) ? 1U : 0U;
  uint8_t rst_por  = (__HAL_RCC_GET_FLAG(RCC_FLAG_PORRST)  != RESET) ? 1U : 0U;
  uint8_t rst_pin  = (__HAL_RCC_GET_FLAG(RCC_FLAG_PINRST)  != RESET) ? 1U : 0U;
  uint8_t rst_soft = (__HAL_RCC_GET_FLAG(RCC_FLAG_SFTRST)  != RESET) ? 1U : 0U;
  __HAL_RCC_CLEAR_RESET_FLAGS();

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_FSMC_Init();
  MX_SPI1_Init();
  MX_USART2_UART_Init();
  MX_UART4_Init();
  MX_TIM3_Init();
  MX_IWDG_Init();
  MX_RTC_Init();
  MX_UART5_Init();
  /* USER CODE BEGIN 2 */

  SPI_FLASH_Init();                          /* W25Q128: SPI1 + CS(PG8) */
  HAL_TIM_Base_Start_IT(&htim3);             /* TIM3 1ms: LVGL 心跳 */
  Log_Init();
  /* Log 已就绪，把上面在 Init 区捕获的复位原因落盘。放在这里而不是更早，
   * 是因为 Log_Init() 之前写日志会落到未初始化的区域。
   * 只记一条汇总：优先级 IWDG > 掉电 > 外部复位 > 软复位。
   * 为什么 IWDG 优先——它才是需要行动的那一条（说明有东西卡住了）；
   * 其余三种是正常的上电/按键复位，不需要告警。 */
  if (rst_iwdg)      Log_Write(LOG_WARN,  "reset by IWDG");
  else if (rst_por)  Log_Write(LOG_INFO,  "reset by power-on");
  else if (rst_pin)  Log_Write(LOG_INFO,  "reset by external pin");
  else if (rst_soft) Log_Write(LOG_INFO,  "reset by software");
  FlashCache_Init();                         /* 定位补传缓存写地址 */
  Clock_Init();                              /* 时间服务：RTC 备份域 / 编译时间定初值 */
  RS485_RX;                                  /* 485 默认接收 */
  HAL_UARTEx_ReceiveToIdle_DMA(&huart4, esp_rx_dma_buf, sizeof(esp_rx_dma_buf));   /* ESP8266 DMA 空闲接收 */
  __HAL_DMA_DISABLE_IT(&hdma_uart4_rx, DMA_IT_HT);
  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();  /* Call init function for freertos objects (in cmsis_os2.c) */
  MX_FREERTOS_Init();

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_LSI|RCC_OSCILLATORTYPE_HSE
                              |RCC_OSCILLATORTYPE_LSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.LSEState = RCC_LSE_ON;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 4;
  RCC_OscInitStruct.PLL.PLLN = 168;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }

  /** Enables the Clock Security System
  */
  HAL_RCC_EnableCSS();
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM4 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */
  if (htim->Instance == TIM3)
  {
    lv_tick_inc(1);                          /* LVGL 1ms 心跳 */
  }
  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM4)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
