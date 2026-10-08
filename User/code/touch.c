/*
 * XPT2046 电阻触摸驱动（GPIO 模拟 SPI）
 * TP_Get_Calibrated 为 LVGL indev 的对接接口（原仓库缺失，此处补全）。
 */
#include "touch.h"
#include "bsp_flash.h"
#include "FreeRTOS.h"
#include "task.h"

#define TP_CLK(x)  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, (GPIO_PinState)(x))
#define TP_DIN(x)  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1, (GPIO_PinState)(x))
#define TP_CS(x)   HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, (GPIO_PinState)(x))
#define TP_DOUT()  HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_2)
#define TP_PEN()   HAL_GPIO_ReadPin(GPIOF, GPIO_PIN_11)

/* —— 时序对齐厂商例程 xpt2046.c（启明欣欣 HAL 库版本）：——
 * 每个时钟沿约 1µs、命令后 6µs 建立、busy 时钟后读 16 位取高 12 位(>>4)。
 * 之前自写版本无延时且 >>3，读数整体错位。 */

static void tp_delay_us(uint32_t us)
{
    while (us--)
        for (volatile uint32_t i = 0; i < 100; i++)
            __NOP();
}

static void TP_WriteByte(uint8_t d)             /* MSB 先行，上升沿写入 */
{
    for (uint8_t i = 0; i < 8; i++) {
        TP_CLK(0);
        TP_DIN((d & 0x80) ? 1 : 0);
        d <<= 1;
        tp_delay_us(1);
        TP_CLK(1);
        tp_delay_us(1);
    }
}

static uint16_t TP_Read_AD(uint8_t cmd)         /* 差分模式读 12bit */
{
    uint16_t v = 0;

    TP_CLK(0);
    TP_DIN(0);
    TP_CS(0);
    TP_WriteByte(cmd);
    tp_delay_us(6);                             /* 转换建立时间 */
    TP_CLK(0); tp_delay_us(1);
    TP_CLK(1); tp_delay_us(1);                  /* busy 时钟 */
    TP_CLK(0);
    for (uint8_t i = 0; i < 16; i++) {          /* 16 位，仅高 12 位有效 */
        v <<= 1;
        TP_CLK(0); tp_delay_us(1);
        TP_CLK(1); tp_delay_us(1);
        if (TP_DOUT()) v |= 1;
    }
    v >>= 4;
    TP_CS(1);
    return v;
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

/* ================= 两点校准 =================
 * 开机在屏幕 左上/右下 各画一个十字，用户依次按中心，
 * 用两次按压的原始读数推算整屏映射，镜像/量程自动适配。
 * 横屏假定 XPT 的 Y(0x90) 对应面板 X、X(0xD0) 对应面板 Y；
 * 若实测方向不对，改 TP_SWAP_XY 后重新校准即可。 */
#define TP_SWAP_XY     1

#define CAL_CX1  20                    /* 左上十字中心 */
#define CAL_CY1  16
#define CAL_CX2  (LCD_W - 21)          /* 右下十字中心 299 */
#define CAL_CY2  (LCD_H - 17)          /* 223 */

static uint16_t cal_y_tl, cal_x_tl, cal_y_br, cal_x_br; /* 十字处的原始读数 */
static uint8_t  tp_calibrated = 0;

/* 等一次按压，取 16 次读数平均；sx=rawY, sy=rawX（调用方按交换语义使用） */
static void TP_WaitSample(uint16_t *sy, uint16_t *sx)
{
    uint32_t ax = 0, ay = 0;
    uint8_t n = 0;

    while (TP_PEN() == GPIO_PIN_SET)           /* 等按下 */
        vTaskDelay(10);
    vTaskDelay(20);                            /* 膜形变稳定 */

    while (n < 16 && TP_PEN() == GPIO_PIN_RESET)
    {
        uint16_t rx = TP_ReadX();
        uint16_t ry = TP_ReadY();
        if (rx > 50 && ry > 50) { ax += rx; ay += ry; n++; }
        vTaskDelay(5);
    }
    if (n > 0) { *sx = ax / n; *sy = ay / n; }
    else       { *sx = 0;      *sy = 0; }

    while (TP_PEN() == GPIO_PIN_RESET)         /* 等松开，防止连采 */
        vTaskDelay(10);
}

static void TP_DrawCross(uint16_t cx, uint16_t cy, uint16_t color)
{
    LCD_Fill(cx - 8, cy, cx + 8, cy, color);
    LCD_Fill(cx, cy - 8, cx, cy + 8, color);
}

/* —— 校准参数持久化 ——
 * 存 W25Q128 参数区 0xF00000（日志区 <4MB、补传缓存 4~8MB，此处空闲）。
 * 仅 UI 初始化时读写一次，与运行期的日志/补传写入无并发；
 * 读回带魔数+累加和校验，坏块/擦除中断都会被拒绝并重新校准。 */
#define TP_CAL_FLASH_ADDR  0x00F00000UL
#define TP_CAL_MAGIC       0x54504331UL   /* "TPC1" */

typedef struct {
    uint32_t magic;
    uint16_t y_tl, x_tl, y_br, x_br;
    uint16_t sum;
} TPCal_t;

static uint8_t TP_TryLoadCal(void)
{
    TPCal_t c;
    SPI_FLASH_BufferRead((uint8_t *)&c, TP_CAL_FLASH_ADDR, sizeof(c));
    uint16_t sum = (uint16_t)(c.magic + c.y_tl + c.x_tl + c.y_br + c.x_br);
    if (c.magic != TP_CAL_MAGIC || c.sum != sum) return 0;
    if (c.y_tl == c.y_br || c.x_tl == c.x_br) return 0;

    cal_y_tl = c.y_tl;  cal_x_tl = c.x_tl;
    cal_y_br = c.y_br;  cal_x_br = c.x_br;
    tp_calibrated = 1;
    return 1;
}

static void TP_SaveCal(void)
{
    TPCal_t c = { TP_CAL_MAGIC, cal_y_tl, cal_x_tl, cal_y_br, cal_x_br, 0 };
    c.sum = (uint16_t)(c.magic + c.y_tl + c.x_tl + c.y_br + c.x_br);
    SPI_FLASH_SectorErase(TP_CAL_FLASH_ADDR);
    SPI_FLASH_BufferWrite((uint8_t *)&c, TP_CAL_FLASH_ADDR, sizeof(c));
}

void TP_Do_Calibration(void)
{
    uint16_t sy1, sx1, sy2, sx2;

    LCD_Clear(0xFFFF);

    /* 强制重新校准入口：开机按住屏幕（PEN 为低）→ 忽略 Flash 里的旧校准。
     * 换板/换屏/校准漂移时按住开机即可重校，不必擦 Flash。 */
    {
        uint8_t hold = 0;
        for (uint32_t i = 0; i < 50; i++)      /* 采样 500ms，滤掉上电抖动 */
        {
            if (TP_PEN() == GPIO_PIN_RESET) hold++;
            vTaskDelay(10);
        }
        if (hold < 30)                          /* 没按住 → 尝试装载旧校准 */
            if (TP_TryLoadCal()) return;        /* Flash 里有有效校准，免校准开机 */
    }

    TP_DrawCross(CAL_CX1, CAL_CY1, 0xF800);
    do { TP_WaitSample(&sy1, &sx1); } while (sx1 == 0);

    TP_DrawCross(CAL_CX1, CAL_CY1, 0xFFFF);    /* 擦掉左上 */
    TP_DrawCross(CAL_CX2, CAL_CY2, 0xF800);
    do { TP_WaitSample(&sy2, &sx2); } while (sx2 == 0);

    if (sy1 != sy2 && sx1 != sx2)
    {
        cal_y_tl = sy1;  cal_x_tl = sx1;
        cal_y_br = sy2;  cal_x_br = sx2;
        tp_calibrated = 1;
        TP_SaveCal();
    }
    LCD_Clear(0xFFFF);
}

/* 返回 1=有效坐标（按下），0=未按下；坐标已映射到 LCD_W x LCD_H */
uint8_t TP_Get_Calibrated(uint16_t *x, uint16_t *y)
{
    if (TP_PEN() != GPIO_PIN_RESET) return 0;  /* PEN 高=松开 */

    /* 抗抖（同厂商例程）：连读两对，任一轴差值超 50 视为抖动丢弃 */
    uint16_t rx1 = TP_ReadX(), ry1 = TP_ReadY();
    uint16_t rx2 = TP_ReadX(), ry2 = TP_ReadY();
    if (rx1 < 50 || ry1 < 50 || rx2 < 50 || ry2 < 50) return 0;
    uint16_t ddx = (rx1 > rx2) ? rx1 - rx2 : rx2 - rx1;
    uint16_t ddy = (ry1 > ry2) ? ry1 - ry2 : ry2 - ry1;
    if (ddx > 50 || ddy > 50) return 0;
    uint16_t rx = (rx1 + rx2) / 2;
    uint16_t ry = (ry1 + ry2) / 2;

    if (tp_calibrated)
    {
        /* 十字中心对标：TL→(20,16)，BR→(299,223)，差值小防除零已在采集时保证 */
        int32_t ix = CAL_CX1 + ((int32_t)ry - cal_y_tl) * (CAL_CX2 - CAL_CX1)
                     / ((int32_t)cal_y_br - cal_y_tl);
        int32_t iy = CAL_CY1 + ((int32_t)rx - cal_x_tl) * (CAL_CY2 - CAL_CY1)
                     / ((int32_t)cal_x_br - cal_x_tl);
        if (ix < 0) ix = 0;
        if (ix > LCD_W - 1) ix = LCD_W - 1;
        if (iy < 0) iy = 0;
        if (iy > LCD_H - 1) iy = LCD_H - 1;
        *x = (uint16_t)ix;
        *y = (uint16_t)iy;
        return 1;
    }

    /* 未校准的兜底映射（量程常数可能不准，仅用于链路验证） */
    uint32_t px = (TP_SWAP_XY ? ry : rx);
    uint32_t py = (TP_SWAP_XY ? rx : ry);
    if (px < 200) px = 200;
    if (px > 3900) px = 3900;
    if (py < 200) py = 200;
    if (py > 3900) py = 3900;
    *x = (uint16_t)((px - 200) * LCD_W / 3700);
    *y = (uint16_t)((py - 200) * LCD_H / 3700);
    return 1;
}
