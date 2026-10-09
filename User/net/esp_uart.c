#include "esp_uart.h"
#include "usart.h"
#include "stm32f4xx.h"
#include "dma.h"
#include "lcd.h"
#include "touch.h"
#include "stdio.h"
#include "string.h"
#include "lvgl.h"                  
#include "lv_port_disp.h"        
#include "lv_port_indev.h"       
#include "page_main.h"
#include "modbus.h"
#include "esp_uart.h"
#include "ESP_at.h"
#include "cloud.h"
#include "tasks.h"
#include "cmsis_os2.h"

extern uint8_t a[256];
extern DMA_HandleTypeDef hdma_uart4_rx;
// 循环缓冲区大小

// 循环缓冲区
uint8_t buffer[BUFFER_SIZE];
/* 索引必须是 16 位：缓冲扩到 2048B 后，uint8_t 索引在 256 处发生截断回绕，
 * (255+1)%2048 会先在 uint8_t 域里变成 0 再取模——位置全错。
 * 缓冲从 300 扩到 2048 是为了装下 AT+CWLAP 的整包扫描应答（多热点 1KB+）。 */
uint16_t readIndex = 0;
uint16_t writeIndex = 0;

/**
 * @brief  清空环形缓冲区（丢弃未读数据）
 *
 * 只把读指针追到写指针，**不再把两个指针都归零**：
 * 写指针属于中断那一侧，任务侧去改它会让两侧错位——中断接着按旧 writeIndex
 * 写，任务却从 0 开始读，读到的就是"上次残留+新数据"的混合垃圾
 * （AT 应答被读花的典型来源）。readIndex 是 16 位单次写入，Cortex-M 上原子。
 */
void RingBuf_Clear(void)
{
    readIndex = writeIndex;
}

/**
* @brief 增加读索引
* @param length 要增加的长度
*/
void Command_AddReadIndex(uint8_t length) {
    readIndex += length;
    readIndex %= BUFFER_SIZE;
}

/**
* @brief 读取第i位数据 超过缓存区长度自动循环
* @param i 要读取的数据索引
*/

uint8_t Command_Read(uint16_t i) {
    return buffer[i % BUFFER_SIZE];
}

/*获取read下标对应的数据*/
uint8_t RingBuf_Read(uint8_t *out) {
    if (readIndex == writeIndex) {
        return 0;   // 缓冲区空
    }
    *out = buffer[readIndex];
    readIndex = (readIndex + 1) % BUFFER_SIZE;
    return 1;
}


uint16_t Command_GetLength() {
    return (uint16_t)((writeIndex + BUFFER_SIZE - readIndex) % BUFFER_SIZE);
}


/**
* @brief 计算缓冲区剩余空间
* @return 剩余空间
* @retval 0 缓冲区已满
* @retval 1~BUFFER_SIZE-1 剩余空间
* @retval BUFFER_SIZE 缓冲区为空
*/
uint16_t Command_GetRemain() {
    return (uint16_t)(BUFFER_SIZE - Command_GetLength());
}

/**	不会覆盖旧数据 满后停止写入
* @brief 向缓冲区写入数据（**只能在接收中断里调用**：DMA 空闲回调）
* @param data 要写入的数据指针
* @param len  要写入的数据长度
* @return 写入的数据长度
*
* 为什么长度参数必须是 uint16_t：
*   DMA 缓冲是 256 字节，空闲回调给的 Size 可以是 256；之前这里是 uint8_t，
*   256 会被截断成 0 → 整块数据被丢（len==0 直接 return）。ESP 一口气连发
*   1KB+（CWLAP 扫描应答、MQTT 下行）时，每满 256 字节就丢一块，表现就是
*   "扫描/连接经常失败、应答收不全"。
*
* 为什么不再 osKernelLock：
*   本函数只在中断上下文跑，CMSIS-RTOS2 的 osKernelLock 在 ISR 里是无效的
*   （返回 osErrorISR，等于没加锁）。这里也不需要一个真锁：写指针只由中断
*   这一侧推进，而且是在 memcpy 完成之后才发布 writeIndex，读者只会看到
*   完整数据（单生产者/单消费者环形缓冲的标准用法）。
*/
uint16_t Command_Write(uint8_t *data, uint16_t len)
{
    // 入参校验
    if (data == NULL || len == 0)
        return 0;

    uint16_t remain = Command_GetRemain();			//剩余长度
    if (remain < len)
        return 0;


    uint16_t idx = writeIndex;
    // 分段写入环形缓冲区
    uint16_t part1 = BUFFER_SIZE - idx;					//写索引到末尾的剩余长度
    if (len <= part1)
    {
        memcpy(buffer + idx, data, len);
        writeIndex = (idx + len) % BUFFER_SIZE;
    }
    else			//绕回到开头 但并没有覆盖数据 前面的数据已经被读取
    {
        memcpy(buffer + idx, data, part1);
        memcpy(buffer, data + part1, len - part1);
        writeIndex = (len - part1) % BUFFER_SIZE;
    }

    return len;
}




extern uint8_t screen_off_flag;
/**
* @brief 从缓冲区读取所有未处理的数据（不解析指令帧）
* @param command 存储读取数据的指针
* @return 实际读取的字节数（0 表示缓冲区为空）
*/
uint16_t Command_GetCommand(uint8_t *command) {
    uint16_t len = Command_GetLength();  // 获取当前未处理的数据长度
    if (len == 0) {
        return 0;
    }
    // 顺序读取 len 个字节到 command 中
    for (uint16_t i = 0; i < len; i++) {
        command[i] = Command_Read(readIndex + i);
    }
    Command_AddReadIndex(len);  // 移动读索引，表示这些数据已被消费
    return len;
}

extern TickType_t idle_tick;
extern uint8_t lcd_bl_en ; //1亮屏 0熄屏
/*接收完成回调*/
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
	if(huart==&huart4)
	{
		Command_Write(a,Size);		//向缓冲区写入数据
		
		HAL_UARTEx_ReceiveToIdle_DMA(&huart4,a,sizeof(a));
		__HAL_DMA_DISABLE_IT(&hdma_uart4_rx,DMA_IT_HT);
	}
}