/* Modbus RTU 从机固件 —— 通过宏定义切换从机类型
 *
 * 使用方法：
 *   编译温湿度从机：把 SLAVE_TYPE 改成 SLAVE_TYPE_HUMIDITY
 *   编译光照从机：  把 SLAVE_TYPE 改成 SLAVE_TYPE_LIGHT
 * 两块板子分别烧录不同配置即可，公共代码完全一致。
 *
 * 硬件：启明欣欣 F407
 * 通信：USART2 (PA2/PA3)，RS485 收发使能 PG6，9600-8N1
 */

#include "stm32f4xx_hal.h"
#include "stm32f4xx_hal_adc.h"

/* ================================================================
 *                  ★★★ 用户配置区（只改这里） ★★★
 * ================================================================ */

/* 从机类型：二选一 */
#define SLAVE_TYPE_HUMIDITY  1
#define SLAVE_TYPE_LIGHT     2
#define SLAVE_TYPE           SLAVE_TYPE_LIGHT   /* ← 改这里 */

/* 根据类型自动派生从机地址和寄存器起始地址 */
#if (SLAVE_TYPE == SLAVE_TYPE_HUMIDITY)
  #define SLAVE_ADDR         0x01
  #define REG_START_ADDR     0x0001
#elif (SLAVE_TYPE == SLAVE_TYPE_LIGHT)
  #define SLAVE_ADDR         0x02
  #define REG_START_ADDR     0x0002
#else
  #error "未知的 SLAVE_TYPE"
#endif

/* DHT11 引脚（仅温湿度从机使用） */
#define DHT_GPIO_PORT        GPIOG
#define DHT_GPIO_PIN         GPIO_PIN_9
#define DHT_CYC_PER_US       168

/* 光照 ADC 引脚（仅光照从机使用） */
#define LIGHT_ADC_GPIO_PORT  GPIOF
#define LIGHT_ADC_GPIO_PIN   GPIO_PIN_7
#define LIGHT_ADC_CHANNEL    ADC_CHANNEL_5

/* RS485 收发方向控制 */
#define RS485_GPIO_PORT      GPIOG
#define RS485_GPIO_PIN       GPIO_PIN_8
#define RS485_TX()  HAL_GPIO_WritePin(RS485_GPIO_PORT, RS485_GPIO_PIN, GPIO_PIN_SET)
#define RS485_RX()  HAL_GPIO_WritePin(RS485_GPIO_PORT, RS485_GPIO_PIN, GPIO_PIN_RESET)

/* 活动指示灯 */
#define LED0_GPIO_PORT       GPIOF
#define LED0_GPIO_PIN        GPIO_PIN_9
#define LED0_TGL()  HAL_GPIO_TogglePin(LED0_GPIO_PORT, LED0_GPIO_PIN)

/* ================================================================ */

static UART_HandleTypeDef huart2;
static uint16_t reg_buf[2];   /* 保持寄存器缓存（两个从机共用） */

void Error_Handler(void);
void SysTick_Handler(void) { HAL_IncTick(); }

/* ================================================================
 *           温湿度从机专属：DHT11 驱动（条件编译）
 * ================================================================ */
#if (SLAVE_TYPE == SLAVE_TYPE_HUMIDITY)

static uint16_t temp_raw = 256;   /* 25.6℃ */
static uint16_t humi_raw = 584;   /* 58.4% */

static void DHT_DelayInit(void) {
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}
static void DHT_DelayUs(uint32_t us) {
  uint32_t start = DWT->CYCCNT;
  while (DWT->CYCCNT - start < us * DHT_CYC_PER_US);
}
static void DHT_SetDir(uint32_t mode) {
  GPIO_InitTypeDef g = {0};
  g.Pin = DHT_GPIO_PIN;
  g.Mode = mode;
  g.Pull = GPIO_PULLUP;
  g.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(DHT_GPIO_PORT, &g);
}
static int32_t DHT_Wait(GPIO_PinState st, uint32_t timeout_us) {
  uint32_t start = DWT->CYCCNT;
  while (HAL_GPIO_ReadPin(DHT_GPIO_PORT, DHT_GPIO_PIN) != st)
    if (DWT->CYCCNT - start > timeout_us * DHT_CYC_PER_US) return -1;
  return (int32_t)((DWT->CYCCNT - start) / DHT_CYC_PER_US);
}
static int DHT_Read(void) {
  uint8_t data[5] = {0};
  DHT_SetDir(GPIO_MODE_OUTPUT_PP);
  HAL_GPIO_WritePin(DHT_GPIO_PORT, DHT_GPIO_PIN, GPIO_PIN_RESET);
  HAL_Delay(20);
  HAL_GPIO_WritePin(DHT_GPIO_PORT, DHT_GPIO_PIN, GPIO_PIN_SET);
  DHT_SetDir(GPIO_MODE_INPUT);
  DHT_DelayUs(30);
  if (DHT_Wait(GPIO_PIN_RESET, 100) < 0) return -1;
  if (DHT_Wait(GPIO_PIN_SET, 100) < 0) return -1;
  if (DHT_Wait(GPIO_PIN_RESET, 100) < 0) return -1;
  for (int i = 0; i < 40; i++) {
    if (DHT_Wait(GPIO_PIN_SET, 100) < 0) return -1;
    int32_t high_us = DHT_Wait(GPIO_PIN_RESET, 100);
    if (high_us < 0) return -1;
    data[i / 8] <<= 1;
    if (high_us > 45) data[i / 8] |= 1;
  }
  if ((uint8_t)(data[0] + data[1] + data[2] + data[3]) != data[4]) return -1;
  humi_raw = (uint16_t)data[0] * 10 + data[1];
  temp_raw = (uint16_t)data[2] * 10 + data[3];
  return 0;
}
static void sensor_poll(void) {
  static uint32_t last = 0;
  if (HAL_GetTick() - last < 2000) return;
  last = HAL_GetTick();
  (void)DHT_Read();
}
static void sensor_fill_regs(void) {
  reg_buf[0] = temp_raw;
  reg_buf[1] = humi_raw;
}

#endif /* SLAVE_TYPE_HUMIDITY */

/* ================================================================
 *           光照从机专属：ADC3 采样（条件编译）
 * ================================================================ */
#if (SLAVE_TYPE == SLAVE_TYPE_LIGHT)

static ADC_HandleTypeDef hadc3;
static uint32_t light_raw32 = 0;   /* 上电首采样立即覆盖，见 sensor_poll */

static void MX_ADC3_Init(void) {
  ADC_ChannelConfTypeDef sConfig = {0};
  GPIO_InitTypeDef g = {0};
  __HAL_RCC_ADC3_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();
  g.Pin = LIGHT_ADC_GPIO_PIN;
  g.Mode = GPIO_MODE_ANALOG;
  g.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(LIGHT_ADC_GPIO_PORT, &g);
  hadc3.Instance = ADC3;
  hadc3.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc3.Init.Resolution = ADC_RESOLUTION_12B;
  hadc3.Init.ScanConvMode = DISABLE;
  hadc3.Init.ContinuousConvMode = DISABLE;
  hadc3.Init.DiscontinuousConvMode = DISABLE;
  hadc3.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc3.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc3.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc3.Init.NbrOfConversion = 1;
  hadc3.Init.DMAContinuousRequests = DISABLE;
  hadc3.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  if (HAL_ADC_Init(&hadc3) != HAL_OK) Error_Handler();
  sConfig.Channel = LIGHT_ADC_CHANNEL;
  sConfig.Rank = 1;
  sConfig.SamplingTime = ADC_SAMPLETIME_480CYCLES;
  if (HAL_ADC_ConfigChannel(&hadc3, &sConfig) != HAL_OK) Error_Handler();
}
static uint32_t light_read(void) {
  uint32_t adc, lx;
  HAL_ADC_Start(&hadc3);
  if (HAL_ADC_PollForConversion(&hadc3, 10) != HAL_OK) {
    HAL_ADC_Stop(&hadc3);
    return light_raw32;
  }
  adc = HAL_ADC_GetValue(&hadc3);
  HAL_ADC_Stop(&hadc3);
  lx = (adc * 20000000UL) / 4095UL;
  if (lx < 1000UL) lx = 1000UL;
  return lx;
}
static void sensor_poll(void) {
  static uint32_t last = 0;
  static uint8_t first = 1;
  if (first) {   /* 首采样直接作为初值：上电即真实值，避免假斜坡 */
    light_raw32 = light_read();
    first = 0;
    last = HAL_GetTick();
    return;
  }
  if (HAL_GetTick() - last < 1000) return;
  last = HAL_GetTick();
  uint32_t nv = light_read();
  light_raw32 = (light_raw32 * 3 + nv) / 4;
}
static void sensor_fill_regs(void) {
  reg_buf[0] = (uint16_t)(light_raw32 >> 16);
  reg_buf[1] = (uint16_t)light_raw32;
}

#endif /* SLAVE_TYPE_LIGHT */

/* ================================================================
 *                        公共代码
 * ================================================================ */

static void SystemClock_Config(void) {
  RCC_OscInitTypeDef osc = {0};
  RCC_ClkInitTypeDef clk = {0};
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
  osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  osc.HSEState = RCC_HSE_ON;
  osc.PLL.PLLState = RCC_PLL_ON;
  osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  osc.PLL.PLLM = 4;
  osc.PLL.PLLN = 168;
  osc.PLL.PLLP = RCC_PLLP_DIV2;
  osc.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&osc) != HAL_OK) Error_Handler();
  clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                  RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
  clk.APB1CLKDivider = RCC_HCLK_DIV4;
  clk.APB2CLKDivider = RCC_HCLK_DIV2;
  if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_5) != HAL_OK) Error_Handler();
}

static void MX_GPIO_Init(void) {
  GPIO_InitTypeDef g = {0};
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();

  /* RS485 方向控制 */
  HAL_GPIO_WritePin(RS485_GPIO_PORT, RS485_GPIO_PIN, GPIO_PIN_RESET);
  g.Pin = RS485_GPIO_PIN;
  g.Mode = GPIO_MODE_OUTPUT_PP;
  g.Pull = GPIO_NOPULL;
  g.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(RS485_GPIO_PORT, &g);

  /* LED0 */
  HAL_GPIO_WritePin(LED0_GPIO_PORT, LED0_GPIO_PIN, GPIO_PIN_RESET);
  g.Pin = LED0_GPIO_PIN;
  HAL_GPIO_Init(LED0_GPIO_PORT, &g);
}

static void MX_USART2_UART_Init(void) {
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 9600;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart2) != HAL_OK) Error_Handler();
}

void HAL_UART_MspInit(UART_HandleTypeDef *huart) {
  GPIO_InitTypeDef g = {0};
  if (huart->Instance != USART2) return;
  __HAL_RCC_USART2_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  g.Pin = GPIO_PIN_2 | GPIO_PIN_3;
  g.Mode = GPIO_MODE_AF_PP;
  g.Pull = GPIO_PULLUP;
  g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  g.Alternate = GPIO_AF7_USART2;
  HAL_GPIO_Init(GPIOA, &g);
}

static uint16_t crc16(const uint8_t *data, uint8_t len) {
  uint16_t crc = 0xFFFF;
  while (len--) {
    crc ^= *data++;
    for (int i = 0; i < 8; i++)
      crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
  }
  return crc;
}

/* ================================================================
 *                          主循环
 * ================================================================ */
int main(void) {
  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();
  MX_USART2_UART_Init();

#if (SLAVE_TYPE == SLAVE_TYPE_HUMIDITY)
  DHT_DelayInit();
  DHT_SetDir(GPIO_MODE_INPUT);
#elif (SLAVE_TYPE == SLAVE_TYPE_LIGHT)
  MX_ADC3_Init();
#endif

  uint8_t frame[8];
  for (;;) {
    sensor_poll();

    /* 1. 等总线静默：收走残留字节，直到 5ms 收不到任何数据。
     *    9600 波特下帧内字节间隔 ≤1.1ms、主站帧间隔 ≥30ms，取 5ms 居中——
     *    保证随后收到的第一个字节必然是帧头，帧中间的数据字节（如温湿度
     *    应答里恒为 0x02 的湿度高字节）不会再被误认成本机地址。 */
    while (HAL_UART_Receive(&huart2, frame, 1, 5) == HAL_OK) {}

    /* 2. 静默后的第一个字节才是从机地址 */
    if (HAL_UART_Receive(&huart2, frame, 1, 20) != HAL_OK) continue;
    if (frame[0] != SLAVE_ADDR) continue;   /* 不是本机：剩余字节由下轮静默清理吃掉 */

    /* 3. 补齐 8 字节问询帧。整帧 8.3ms、主站帧间隔 ≥30ms：15ms 既够收完
     *    本帧，也绝跨不进下一帧——超时吞帧头的老问题根源就在这里。 */
    if (HAL_UART_Receive(&huart2, &frame[1], 7, 15) != HAL_OK) continue;
    if (frame[1] != 0x03) continue;

    uint16_t crc = crc16(frame, 6);
    if (frame[6] != (crc & 0xFF) || frame[7] != ((crc >> 8) & 0xFF)) continue;

    uint16_t start = ((uint16_t)frame[2] << 8) | frame[3];
    uint16_t count = ((uint16_t)frame[4] << 8) | frame[5];
    if (start != REG_START_ADDR || count != 2) continue;

    sensor_fill_regs();

    uint8_t resp[9];
    uint8_t pos = 0;
    resp[pos++] = SLAVE_ADDR;
    resp[pos++] = 0x03;
    resp[pos++] = (uint8_t)(count * 2);
    for (uint16_t i = 0; i < count; i++) {
      resp[pos++] = reg_buf[i] >> 8;
      resp[pos++] = reg_buf[i] & 0xFF;
    }
    crc = crc16(resp, pos);
    resp[pos++] = crc & 0xFF;
    resp[pos++] = crc >> 8;

    RS485_TX();
    HAL_UART_Transmit(&huart2, resp, pos, 100);
    while (!__HAL_UART_GET_FLAG(&huart2, UART_FLAG_TC)) {}
    RS485_RX();
    LED0_TGL();
    HAL_Delay(4);
  }
}

void Error_Handler(void) { __disable_irq(); while (1) {} }