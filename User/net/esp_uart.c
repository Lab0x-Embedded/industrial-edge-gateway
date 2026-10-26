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

extern uint8_t esp_rx_dma_buf[256];
extern DMA_HandleTypeDef hdma_uart4_rx;
// 循环缓冲区大小

/* 循环缓冲区
 *
 * 索引为什么必须 volatile：
 *   writeIndex 由中断（DMA 空闲回调）推进、任务侧读；readIndex 由任务侧推进、
 *   中断侧读。不加 volatile 时，编译器可以把索引读到寄存器里复用（尤其
 *   RingBuf_Read / Command_GetLength 会被内联进"逐字节读"的紧循环），
 *   于是任务侧会看到过期的 writeIndex —— 表现为"数据明明到了却读不到"。
 *
 * 为什么必须是 16 位：缓冲扩到 2048B 后，uint8_t 索引在 256 处发生截断回绕，
 * (255+1)%2048 会先在 uint8_t 域里变成 0 再取模——位置全错。
 *
 * 缓冲 300 → 2048 是为了装下 AT+CWLAP 的整包扫描应答（多热点 1KB+）。 */
uint8_t buffer[BUFFER_SIZE];
volatile uint16_t readIndex = 0;
volatile uint16_t writeIndex = 0;

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
 * @retval BUFFER_SIZE-1 缓冲区为空
 *
 * 为什么是 BUFFER_SIZE-1-length（原来少了这个 -1，是个真 off-by-one）：
 *   判空/判满靠"读指针是否追上写指针"，所以必须**始终空出一个槽位**。
 *   原来允许写满到 length==BUFFER_SIZE，此时 writeIndex 恰好追上 readIndex，
 *   Command_GetLength() 返回 0 → 刚写进去的这批数据和之前的未读数据
 *   一起被当成"缓冲区为空"（读侧一个字都读不到），等于把整缓冲的应答扔掉。
 *   扫描应答长、且是逐字节读的，最坏情况正好撞上这个边界。
 */
uint16_t Command_GetRemain() {
    uint16_t len = Command_GetLength();
    return (uint16_t)((len >= BUFFER_SIZE - 1) ? 0 : (BUFFER_SIZE - 1 - len));
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

    uint16_t remain = Command_GetRemain();		//剩余长度
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




extern TickType_t idle_tick;
extern uint8_t lcd_bl_en ; //1亮屏 0熄屏

/* 出错回调：**项目原本没有实现它**，这是个致命缺口。
 * UART4 一旦出现溢出(ORE)/帧错误/噪声，STM32 HAL 会中止正在进行的 DMA 接收，
 * 而且不会自动重启（HAL_UART_ErrorCallback 是弱定义空函数）。DMA 一停，
 * HAL_UARTEx_RxEventCallback 再也不会被触发，RX 就永久停摆：
 * 模块在正常说话，板子却收不到一个字节 → AT 全部超时 → "WiFi 连不上 / 扫描永远失败"，
 * 只能复位恢复（现象就是"开始好好的，某个时刻起一直连不上"）。
 * 这里做两件事：清错误标志 + 立刻重新挂上 DMA 接收（与 RxEventCallback 一致）。 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
	if (huart == &huart4)
	{
		__HAL_UART_CLEAR_OREFLAG(huart);      /* F4 上这一句同时清 PE/FE/NE/ORE */
		huart->ErrorCode = HAL_UART_ERROR_NONE;
		HAL_UARTEx_ReceiveToIdle_DMA(&huart4, esp_rx_dma_buf, sizeof(esp_rx_dma_buf));
		__HAL_DMA_DISABLE_IT(&hdma_uart4_rx, DMA_IT_HT);
	}
}

/*接收完成回调*/
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
	if(huart==&huart4)
	{
		Command_Write(esp_rx_dma_buf,Size);		//向缓冲区写入数据

		HAL_UARTEx_ReceiveToIdle_DMA(&huart4, esp_rx_dma_buf, sizeof(esp_rx_dma_buf));
		__HAL_DMA_DISABLE_IT(&hdma_uart4_rx, DMA_IT_HT);
	}
}