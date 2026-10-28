/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
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
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "tasks.h"
#include "queue.h"
#include "semphr.h"
/* 这些全局定义在 tasks.c */
extern TickType_t idle_tick;
extern TickType_t power_tick;
extern uint8_t    lcd_bl_en;
extern QueueHandle_t data_queue;
extern QueueHandle_t lvgl_data_queue;
extern SemaphoreHandle_t esp_uart_mutex;
extern SemaphoreHandle_t flash_mutex;   /* flash.c 定义，保护 SPI Flash 跨任务访问 */
#if DBG_PRINT_ENABLE
extern SemaphoreHandle_t dbg_uart_mutex; /* main.c 定义，保护 UART5 调试口（dbg_print） */
#endif
#include "lvgl.h"
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
/* USER CODE BEGIN Variables */

/* USER CODE END Variables */
/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/* USER CODE BEGIN VPORT_SUPPORT_TICKS_AND_SLEEP */
__weak void vPortSuppressTicksAndSleep( TickType_t xExpectedIdleTime )
{
  // Generated when configUSE_TICKLESS_IDLE == 2.
  // Function called in tasks.c (in portTASK_FUNCTION).
  // TO BE COMPLETED or TO BE REPLACED by a user one, overriding that weak one.
}
/* USER CODE END VPORT_SUPPORT_TICKS_AND_SLEEP */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  esp_uart_mutex = xSemaphoreCreateMutex();
  flash_mutex = xSemaphoreCreateMutex();
#if DBG_PRINT_ENABLE   /* 关日志时 dbg_print 是空宏，锁没人用，不创建 */
  dbg_uart_mutex = xSemaphoreCreateMutex();   /* UART5 调试口：多任务 Log_Write 共用 */
#endif
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  data_queue      = xQueueCreate(1, sizeof(Data_t));   /* Modbus 用 xQueueOverwrite 写，长度必须为 1 */
  lvgl_data_queue = xQueueCreate(1, sizeof(Data_t));
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* USER CODE BEGIN StartDefaultTask */
        /* 创建 5 个业务任务后自删（优先级沿用原工程 CMSIS 数值）。
         * MQTT 任务的栈给 1024（原 512）：-fstack-usage 量最坏链
         * 「发布 → PUBACK 后的下行解析 → Log_Write → dbg_print → printf」约 2.9KB，
         * 实测 uxTaskGetSystemState 的剩余栈最小值只剩 2112B/4096 = 峰值用掉 1984B，
         * 即原来 512(word)=2KB 已到 97%，必须加。 */
        xTaskCreate(UI,        "UITask",     1024, NULL, osPriorityAboveNormal,     NULL);
        xTaskCreate(Modbus,    "SensorTask",  512, NULL, osPriorityNormal + 1,      NULL);
        xTaskCreate(AT,        "NetworkTask", 512, NULL, osPriorityNormal,          NULL);
        xTaskCreate(MQTT,      "MQTTTask",    1024, NULL, osPriorityNormal,          NULL);
        xTaskCreate(Dog_task,  "Dog_task",    512, NULL, osPriorityAboveNormal + 2, NULL);

        power_tick = xTaskGetTickCount();
        idle_tick  = xTaskGetTickCount();
        lcd_bl_en  = 1;

        vTaskDelete(NULL);
        for(;;) { osDelay(1); }
  /* USER CODE END StartDefaultTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* USER CODE END Application */

