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
/* 接收侧自检计数（esp_uart.c）：drop=缓冲满被整批丢弃的字节数；rst_fail=DMA 重启失败次数；
 * rx_bytes=开机以来从 UART4 收到的**总字节数**（判断"板子到底有没有听见过模块"的决定性依据） */
extern volatile uint32_t uart4_rx_drop_bytes;
extern volatile uint32_t uart4_rx_rst_fail;
extern volatile uint32_t uart4_rx_bytes;
/* 片内自检：写假应答进环形缓冲→逐字节读回来比对。1=读写路径正常。
 * 用来把"我们这侧的数据通路"和"串口线上到底有没有数据"分开。 */
uint8_t RingBuf_SelfTest(void);

uint16_t Command_GetCommand(uint8_t *command);
uint16_t Command_GetLength(void);   /* 环形缓冲里未读字节数：用来判断"串口是否静默" */
uint8_t RingBuf_Read(uint8_t *out) ;
void RingBuf_Clear(void);
#endif /* INC_COMMAND_H_ */
