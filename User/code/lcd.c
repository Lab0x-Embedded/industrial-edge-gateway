/*
 * 2.8 寸 ILI9341 TFT（16 位 8080 并口，FSMC NE4）驱动
 * 初始化序列移植自原工程（SPI 版），读写改为 FSMC 直访；
 * LCD_WritePixels 为 LVGL flush 的对接接口（原仓库缺失，此处补全）。
 */
#include "lcd.h"
#include "gpio.h"

#define ILI9341_ID 0x9341

static uint16_t lcd_id = 0;

static void LCD_WR_REG(uint16_t cmd) { LCD_CMD = cmd; }
static void LCD_WR_DATA(uint16_t dat) { LCD_DATA = dat; }

void LCD_SetWindows(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    LCD_WR_REG(0x2A);
    LCD_WR_DATA(x1 >> 8); LCD_WR_DATA(x1);
    LCD_WR_DATA(x2 >> 8); LCD_WR_DATA(x2);

    LCD_WR_REG(0x2B);
    LCD_WR_DATA(y1 >> 8); LCD_WR_DATA(y1);
    LCD_WR_DATA(y2 >> 8); LCD_WR_DATA(y2);

    LCD_WR_REG(0x2C);   /* 开始写 GRAM */
}

/* LVGL disp_flush 的批量写像素接口（FSMC 并口一次一个 16bit） */
void LCD_WritePixels(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2,
                     uint16_t *color_p, uint32_t pixel_count)
{
    LCD_SetWindows(x1, y1, x2, y2);
    while (pixel_count--)
        LCD_DATA = *color_p++;
}

void LCD_DrawPoint(uint16_t x, uint16_t y, uint16_t color)
{
    LCD_SetWindows(x, y, x, y);
    LCD_DATA = color;
}

void LCD_DrawBigPoint(uint16_t x, uint16_t y, uint16_t color)
{
    LCD_Fill(x - 1, y - 1, x + 1, y + 1, color);
}

void LCD_Fill(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color)
{
    uint32_t n = (uint32_t)(x2 - x1 + 1) * (y2 - y1 + 1);
    LCD_SetWindows(x1, y1, x2, y2);
    while (n--)
        LCD_DATA = color;
}

void LCD_Clear(uint16_t color)
{
    LCD_Fill(0, 0, LCD_W - 1, LCD_H - 1, color);
}

static uint16_t LCD_ReadID(void)
{
    uint16_t id;
    LCD_WR_REG(0xD3);
    id = LCD_DATA;              /* 0x00 */
    id = LCD_DATA;              /* 0x93 */
    id <<= 8;
    id |= LCD_DATA;             /* 0x41 */
    return id;
}

void LCD_Init(void)
{
    /* FSMC 与引脚已由 CubeMX 的 MX_FSMC_Init/MX_GPIO_Init 完成，这里只做屏上电复位与初始化 */
    HAL_Delay(50);

    lcd_id = LCD_ReadID();

    LCD_WR_REG(0xCF);
    LCD_WR_DATA(0x00); LCD_WR_DATA(0xC1); LCD_WR_DATA(0x30);
    LCD_WR_REG(0xED);
    LCD_WR_DATA(0x64); LCD_WR_DATA(0x03); LCD_WR_DATA(0x12); LCD_WR_DATA(0x81);
    LCD_WR_REG(0xE8);
    LCD_WR_DATA(0x85); LCD_WR_DATA(0x00); LCD_WR_DATA(0x7A);
    LCD_WR_REG(0xCB);
    LCD_WR_DATA(0x39); LCD_WR_DATA(0x2C); LCD_WR_DATA(0x00);
    LCD_WR_DATA(0x34); LCD_WR_DATA(0x02);
    LCD_WR_REG(0xF7);
    LCD_WR_DATA(0x20);
    LCD_WR_REG(0xEA);
    LCD_WR_DATA(0x00); LCD_WR_DATA(0x00);

    /* 电源控制 */
    LCD_WR_REG(0xC0); LCD_WR_DATA(0x10);
    LCD_WR_REG(0xC1); LCD_WR_DATA(0x10);
    LCD_WR_REG(0xC5);
    LCD_WR_DATA(0x45); LCD_WR_DATA(0x15);
    LCD_WR_REG(0xC7); LCD_WR_DATA(0x90);

    /* 横屏 320x240（沿用原工程方向寄存器值） */
    LCD_WR_REG(0x36); LCD_WR_DATA(0x68);

    /* 像素格式 RGB565 */
    LCD_WR_REG(0x3A); LCD_WR_DATA(0x55);

    /* 帧率 */
    LCD_WR_REG(0xB1);
    LCD_WR_DATA(0x00); LCD_WR_DATA(0x1A);

    /* 显示功能 */
    LCD_WR_REG(0xB6);
    LCD_WR_DATA(0x0A); LCD_WR_DATA(0xA2);

    /* 伽马校正 */
    LCD_WR_REG(0xF2); LCD_WR_DATA(0x00);
    LCD_WR_REG(0x26); LCD_WR_DATA(0x01);
    LCD_WR_REG(0xE0);
    LCD_WR_DATA(0x0F); LCD_WR_DATA(0x2A); LCD_WR_DATA(0x28); LCD_WR_DATA(0x08);
    LCD_WR_DATA(0x0E); LCD_WR_DATA(0x08); LCD_WR_DATA(0x54); LCD_WR_DATA(0xA9);
    LCD_WR_DATA(0x43); LCD_WR_DATA(0x0A); LCD_WR_DATA(0x0F); LCD_WR_DATA(0x00);
    LCD_WR_DATA(0x00); LCD_WR_DATA(0x00); LCD_WR_DATA(0x00);
    LCD_WR_REG(0xE1);
    LCD_WR_DATA(0x00); LCD_WR_DATA(0x15); LCD_WR_DATA(0x17); LCD_WR_DATA(0x07);
    LCD_WR_DATA(0x11); LCD_WR_DATA(0x06); LCD_WR_DATA(0x2B); LCD_WR_DATA(0x56);
    LCD_WR_DATA(0x3C); LCD_WR_DATA(0x05); LCD_WR_DATA(0x10); LCD_WR_DATA(0x0F);
    LCD_WR_DATA(0x3F); LCD_WR_DATA(0x3F); LCD_WR_DATA(0x0F);

    LCD_WR_REG(0x21);   /* 反显（原工程使用，白底界面） */
    LCD_WR_REG(0x11);   /* 退出睡眠 */
    HAL_Delay(120);
    LCD_WR_REG(0x29);   /* 开显示 */

    (void)lcd_id;
}
