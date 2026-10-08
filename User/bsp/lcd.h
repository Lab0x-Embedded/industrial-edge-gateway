#ifndef __LCD_H
#define __LCD_H

#include "stm32f4xx_hal.h"

/* FSMC NE4 (0x6C000000)，16 位并口；RS 接 FSMC_A12(PG2)：地址 bit13 区分命令/数据 */
#define LCD_CMD_BASE   (0x6C000000UL | 0x00001FFEUL)
#define LCD_DATA_BASE  (0x6C000000UL | 0x00002000UL)
#define LCD_CMD        (*(volatile uint16_t *)LCD_CMD_BASE)
#define LCD_DATA       (*(volatile uint16_t *)LCD_DATA_BASE)

#define LCD_W  320
#define LCD_H  240

void LCD_Init(void);
void LCD_Clear(uint16_t color);
void LCD_SetWindows(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);
void LCD_Fill(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color);
void LCD_DrawPoint(uint16_t x, uint16_t y, uint16_t color);
void LCD_DrawBigPoint(uint16_t x, uint16_t y, uint16_t color);
void LCD_WritePixels(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2,
                     uint16_t *color_p, uint32_t pixel_count);

#endif
