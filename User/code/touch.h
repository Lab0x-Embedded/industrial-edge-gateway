#ifndef __TOUCH_H
#define __TOUCH_H

#include "stm32f4xx_hal.h"
#include "lcd.h"

/* XPT2046（GPIO 模拟 SPI，启明欣欣板走线）：
   CLK=PA5  MOSI=PB1  MISO=PB2  CS=PB0  PEN=PF11（低电平=按下） */
void     TP_Init(void);
uint16_t TP_ReadX(void);
uint16_t TP_ReadY(void);
uint16_t TP_ReadPressure(void);
uint8_t  TP_Get_Calibrated(uint16_t *x, uint16_t *y);

#endif
