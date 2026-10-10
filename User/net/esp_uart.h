#ifndef COMMAND_H
#define COMMAND_H

#include "main.h"
#include <string.h>
#include "usart.h"
#include "stm32f4xx.h"
#include "stdio.h"

#define BUFFER_SIZE 2048   /* CWLAP 扫描应答很长（AT+CWLAP 多热点可达 1KB+） */
#define COMMAND_MIN_LENGTH 4
/* 长度必须是 uint16_t：DMA 缓冲 256 字节，空闲回调的 Size 可能正好 256，
 * 用 uint8_t 会被截断成 0 → 整块数据被丢。只能从接收中断调用。 */
uint16_t Command_Write(uint8_t *data, uint16_t length);

uint16_t Command_GetCommand(uint8_t *command);
uint16_t Command_GetLength(void);   /* 环形缓冲里未读字节数：用来判断"串口是否静默" */
uint8_t RingBuf_Read(uint8_t *out) ;
void RingBuf_Clear(void);
#endif /* INC_COMMAND_H_ */
