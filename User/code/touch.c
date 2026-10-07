/*
 * XPT2046 电阻触摸驱动（GPIO 模拟 SPI）
 * TP_Get_Calibrated 为 LVGL indev 的对接接口（原仓库缺失，此处补全）。
 */
#include "touch.h"

#define TP_CLK(x)  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, (GPIO_PinState)(x))
#define TP_DIN(x)  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1, (GPIO_PinState)(x))
#define TP_CS(x)   HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, (GPIO_PinState)(x))
#define TP_DOUT()  HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_2)
#define TP_PEN()   HAL_GPIO_ReadPin(GPIOF, GPIO_PIN_11)

static uint8_t TP_ReadWrite(uint8_t d)          /* MSB 先行，模式 0 时序 */
{
    uint8_t rx = 0;
    for (uint8_t i = 0; i < 8; i++) {
        TP_CLK(0);
        TP_DIN((d & 0x80) ? 1 : 0);
        d <<= 1;
        TP_CLK(1);
        rx <<= 1;
        if (TP_DOUT()) rx |= 1;
    }
    return rx;
}

static uint16_t TP_Read_AD(uint8_t cmd)         /* 差分模式读 12bit */
{
    uint16_t v = 0;
    TP_CS(0);
    TP_ReadWrite(cmd);
    TP_CLK(0); TP_CLK(1);                       /* 补 1 个时钟完成采样 */
    v  = TP_ReadWrite(0xFF) << 8;
    v |= TP_ReadWrite(0xFF);
    TP_CS(1);
    return (v >> 3) & 0x0FFF;
}

uint16_t TP_ReadX(void)      { return TP_Read_AD(0xD0); }
uint16_t TP_ReadY(void)      { return TP_Read_AD(0x90); }

uint16_t TP_ReadPressure(void)
{
    uint16_t z1 = TP_Read_AD(0xB1);
    uint16_t z2 = TP_Read_AD(0xC1);
    if (z1 == 0) return 0;
    return (z2 * 100) / z1;
}

void TP_Init(void)
{
    TP_CS(1);
    TP_ReadX();                                /* 丢弃首次读数 */
}

/* 返回 1=有效坐标（按下），0=未按下；坐标已映射到 320x240 */
uint8_t TP_Get_Calibrated(uint16_t *x, uint16_t *y)
{
    if (TP_PEN() != GPIO_PIN_RESET) return 0;  /* PEN 高=松开 */
    if (TP_ReadPressure() < 30)     return 0;  /* 压力过滤抖动 */

    uint16_t rx = TP_ReadX();
    uint16_t ry = TP_ReadY();
    if (rx < 100 || rx > 4000) return 0;
    if (ry < 100 || ry > 4000) return 0;

    *x = (uint16_t)((rx - 200) * 320UL / 3700UL);
    *y = (uint16_t)((ry - 200) * 240UL / 3700UL);
    if (*x > 319) *x = 319;
    if (*y > 239) *y = 239;
    return 1;
}
