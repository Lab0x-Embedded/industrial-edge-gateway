/* Modbus RTU 从机模拟固件 —— 烧录到第二块启明欣欣 F407 板
 *
 * 通过板载 RS485（USART2 PA2/PA3，收发使能 PG6，9600-8N1）模拟网关
 * dev_list[] 里的两台变送器：
 *   从机 0x01  温湿度  保持寄存器 0x0001 ×2：温度 = reg0/10 ℃，湿度 = reg1/10 %
 *              数据源：真实 DHT11，DATA 接 PB9（模块自带 4.7k~10k 上拉，
 *              裸传感器需在 DATA 与 3.3V 之间自接），读失败沿用上次数值
 *   从机 0x02  光照    保持寄存器 0x0002 ×2：光照 = (reg0<<16|reg1)/1000 lx
 *              数据源：随机缓变的模拟值
 *
 * 与 tools/modbus_sim.py 同一套寄存器语义；光照每秒缓变，DHT11 每 2s 采样。
 * 收到合法 03 问询才应答，PE3(LED0) 每次应答翻转一次作为活动指示。
 */
#include "stm32f4xx_hal.h"
#include <stdlib.h>

#define RS485_TX()  HAL_GPIO_WritePin(GPIOG, GPIO_PIN_6, GPIO_PIN_SET)
#define RS485_RX()  HAL_GPIO_WritePin(GPIOG, GPIO_PIN_6, GPIO_PIN_RESET)
#define LED0_TGL()  HAL_GPIO_TogglePin(GPIOE, GPIO_PIN_3)

static UART_HandleTypeDef huart2;

static uint16_t temp_raw = 256;         /* 25.6℃ */
static uint16_t humi_raw = 584;         /* 58.4% */
static uint32_t light_raw32 = 12345000; /* 12345 lx × 1000 */

static uint16_t reg01[2];               /* 0x0001..0x0002 */
static uint16_t reg02[2];               /* 0x0002..0x0003 */

/* ================= DHT11（单总线，DATA=PB9） ================= */
#define DHT_GPIO        GPIOB
#define DHT_PIN         GPIO_PIN_9
#define DHT_CYC_PER_US  168     /* 168MHz HCLK */

void Error_Handler(void);
void SysTick_Handler(void) { HAL_IncTick(); }

static void DHT_DelayInit(void)         /* 用 DWT 周期计数器做微秒延时 */
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static void DHT_DelayUs(uint32_t us)
{
    uint32_t start = DWT->CYCCNT;
    while (DWT->CYCCNT - start < us * DHT_CYC_PER_US) {}
}

static void DHT_SetDir(uint32_t mode)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = DHT_PIN;
    GPIO_InitStruct.Mode = mode;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(DHT_GPIO, &GPIO_InitStruct);
}

/* 等待数据线变为 st 电平，返回等待的微秒数；超时返回 -1 */
static int32_t DHT_Wait(GPIO_PinState st, uint32_t timeout_us)
{
    uint32_t start = DWT->CYCCNT;
    while (HAL_GPIO_ReadPin(DHT_GPIO, DHT_PIN) != st)
        if (DWT->CYCCNT - start > timeout_us * DHT_CYC_PER_US) return -1;
    return (int32_t)((DWT->CYCCNT - start) / DHT_CYC_PER_US);
}

/* 读一次 DHT11：成功填 humi_raw/temp_raw(×10) 并返回 0，失败返回 -1 */
static int DHT_Read(void)
{
    uint8_t data[5] = {0};

    /* 主机起始信号：拉低 ≥18ms，释放 30µs */
    DHT_SetDir(GPIO_MODE_OUTPUT_PP);
    HAL_GPIO_WritePin(DHT_GPIO, DHT_PIN, GPIO_PIN_RESET);
    HAL_Delay(20);
    HAL_GPIO_WritePin(DHT_GPIO, DHT_PIN, GPIO_PIN_SET);
    DHT_SetDir(GPIO_MODE_INPUT);
    DHT_DelayUs(30);

    /* DHT 应答：80µs 低 + 80µs 高 */
    if (DHT_Wait(GPIO_PIN_RESET, 100) < 0) return -1;
    if (DHT_Wait(GPIO_PIN_SET, 100) < 0) return -1;
    if (DHT_Wait(GPIO_PIN_RESET, 100) < 0) return -1;

    /* 40 bit：每位 50µs 低起头，高电平 ~26µs=0 / ~70µs=1 */
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

static void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState = RCC_HSE_ON;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLM = 4;
    RCC_OscInitStruct.PLL.PLLN = 168;
    RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
    RCC_OscInitStruct.PLL.PLLQ = 4;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) Error_Handler();

    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                                | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;
    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK) Error_Handler();
}

static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();

    HAL_GPIO_WritePin(GPIOG, GPIO_PIN_6, GPIO_PIN_RESET);   /* DE 默认接收 */
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_3, GPIO_PIN_RESET);

    GPIO_InitStruct.Pin = GPIO_PIN_6;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOG, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_3;
    HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);
}

static void MX_USART2_UART_Init(void)
{
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

/* 本固件不带 Core/Src/stm32f4xx_hal_msp.c，这里提供 USART2 的 MSP 初始化 */
void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    if (huart->Instance != USART2) return;
    __HAL_RCC_USART2_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    GPIO_InitStruct.Pin = GPIO_PIN_2 | GPIO_PIN_3;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF7_USART2;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
}

/* 逐位 CRC16-MODBUS，与网关 CRC_16_MODBUS(flag=1)、tools/modbus_sim.py 一致 */
static uint16_t crc16(const uint8_t *data, uint8_t len)
{
    uint16_t crc = 0xFFFF;
    while (len--) {
        crc ^= *data++;
        for (int i = 0; i < 8; i++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    return crc;
}

/* 光照每秒缓变；温湿度由 DHT11 实测，读失败沿用上次数值 */
static void sensors_drift(void)
{
    static uint32_t last_tick = 0;
    if (HAL_GetTick() - last_tick < 1000) return;
    last_tick = HAL_GetTick();

    light_raw32 += (rand() % 300001) - 150000;
    if (light_raw32 > 20000000UL) light_raw32 = 20000000UL;
}

static void dht_poll(void)
{
    static uint32_t last_read = 0;              /* DHT11 采样周期 ≥1s，取 2s */
    if (HAL_GetTick() - last_read < 2000) return;
    last_read = HAL_GetTick();
    (void)DHT_Read();                            /* 失败时保留上次读数 */
}

/* 返回该问询对应的寄存器数组首指针，不支持的问询返回 NULL（静默不应答） */
static const uint16_t *regs_lookup(uint8_t addr, uint16_t start, uint16_t count)
{
    if (count != 2) return NULL;
    if (addr == 0x01 && start == 0x0001) {
        reg01[0] = temp_raw;
        reg01[1] = humi_raw;
        return reg01;
    }
    if (addr == 0x02 && start == 0x0002) {
        reg02[0] = (uint16_t)(light_raw32 >> 16);
        reg02[1] = (uint16_t)light_raw32;
        return reg02;
    }
    return NULL;
}

int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_USART2_UART_Init();
    DHT_DelayInit();
    DHT_SetDir(GPIO_MODE_INPUT);
    srand(HAL_GetTick() ^ 0x1234);

    uint8_t frame[8];
    for (;;) {
        sensors_drift();
        dht_poll();

        /* 等地址字节：只关心 0x01/0x02，其余视为总线噪声 */
        if (HAL_UART_Receive(&huart2, frame, 1, 20) != HAL_OK) continue;
        if (frame[0] != 0x01 && frame[0] != 0x02) continue;

        /* 补齐 8 字节问询帧（9600 下 7 字节约 7.3ms，50ms 超时足够） */
        if (HAL_UART_Receive(&huart2, &frame[1], 7, 50) != HAL_OK) continue;
        if (frame[1] != 0x03) continue;

        uint16_t crc = crc16(frame, 6);
        if (frame[6] != (crc & 0xFF) || frame[7] != ((crc >> 8) & 0xFF)) continue;

        uint16_t start = ((uint16_t)frame[2] << 8) | frame[3];
        uint16_t count = ((uint16_t)frame[4] << 8) | frame[5];
        const uint16_t *vals = regs_lookup(frame[0], start, count);
        if (vals == NULL) continue;

        uint8_t resp[9];
        uint8_t pos = 0;
        resp[pos++] = frame[0];
        resp[pos++] = 0x03;
        resp[pos++] = (uint8_t)(count * 2);
        for (uint16_t i = 0; i < count; i++) {
            resp[pos++] = vals[i] >> 8;
            resp[pos++] = vals[i] & 0xFF;
        }
        crc = crc16(resp, pos);
        resp[pos++] = crc & 0xFF;
        resp[pos++] = crc >> 8;

        RS485_TX();
        HAL_UART_Transmit(&huart2, resp, pos, 100);
        while (!__HAL_UART_GET_FLAG(&huart2, UART_FLAG_TC)) {}  /* 末字节移位完再撒总线 */
        RS485_RX();
        LED0_TGL();
        HAL_Delay(4);                                           /* 帧间隔 > 3.5 字符 */
    }
}

void Error_Handler(void)
{
    __disable_irq();
    while (1) {}
}
