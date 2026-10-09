#include "esp_at.h"
#include "esp_uart.h"
#include "usart.h"
#include <string.h>
#include <stdio.h>

/* cloud.c 里定义的调试串口输出（UART5）；不 include cloud.h，只声明 */
extern void dbg_print(const char *fmt, ...);

// 引用阶段一创建的全局环形缓冲区
extern uint8_t buffer[BUFFER_SIZE];

// 清空环形缓冲区（丢弃所有旧数据）
static void ESP_ClearRxBuf(void) {
    uint8_t dummy;
    while (RingBuf_Read(&dummy));
}


/*
 * 通用 AT 指令发送并逐行解析应答       
 * cmd           : 要发送的 AT 指令（必须包含 \r\n）
 * expected_ack  : 期望出现的应答关键词，如 "OK", "CONNECT", "WIFI GOT IP"
 * timeout_ms    : 超时时间（毫秒）
 * 返回         : ESP_OK(成功), ESP_TIMEOUT(超时), ESP_ERROR(未使用但预留)
 */
//发送 AT 指令 →ESP8266返回数据-> 从环形缓冲区逐行读取应答 → 匹配期望关键词 → 返回成功或超时。
ESP_Status_t ESP_SendCmd(const char *cmd, const char *expected_ack, uint32_t timeout_ms)
{
    // 1. 清空环形缓冲区，丢弃旧数据 逻辑清空 移动读位 逻辑清空
    ESP_ClearRxBuf();

    // 2. 发送指令 ESP8266 uart1波特率115200
    HAL_UART_Transmit(&huart4, (uint8_t *)cmd, strlen(cmd), 1000);

    // 3. 在超时时间内逐字节读取，按行解析
    char line[512];          // 存放一行数据
    uint16_t line_pos = 0;   // 当前行写入位置
    uint32_t start_tick = HAL_GetTick();

    while ((HAL_GetTick() - start_tick) < timeout_ms) {
        uint8_t ch;
			while (RingBuf_Read(&ch)) {									//读缓冲区
            start_tick = HAL_GetTick();					 // 关键：每收到一个字节，就重置超时起点

            if (ch == '\n') {
                // 一行结束
                line[line_pos] = '\0';				//手动赋值结束标志
                
                // 检查是否包含期望应答 			读取完一行数据后 如果有期望应答 则退出函数返回 OK
                if (expected_ack != NULL && strstr(line, expected_ack) != NULL) {
                    return ESP_OK;			
                }
                
                // 检查是否出错
                if (strstr(line, "ERROR") != NULL) {
                    dbg_print("[AT] err: %s\r\n", line);
                    return ESP_ERROR;
                }

                /* "FAIL"/"Fail" 也算失败：CIPSTART 失败的应答是 "CONNECT FAIL"，
                 * 里面恰好含匹配词 "CONNECT" —— 不加这条，建连失败会被上面的
                 * 关键词匹配当成成功放行，随后 CIPSEND 必然拿不到 ">"，
                 * 现场看到的就是"没有 tcp fail、只有 passthrough fail"。
                 * （CWJAP 失败应答也是 "FAIL" 结尾，顺带让 wifi_join 判得又快又准；
                 *   DNS 失败是 "DNS Fail"，大小写两种都查。） */
                if (strstr(line, "FAIL") != NULL || strstr(line, "Fail") != NULL) {
                    dbg_print("[AT] fail: %s\r\n", line);
                    return ESP_ERROR;
                }
                
                // 清空 line，准备读下一行
                line_pos = 0;
            } 
            else if (ch != '\r') {
                // 非回车字符，追加到 line
                if (line_pos < sizeof(line) - 1) {
                    line[line_pos++] = ch;
                }
            }
        }
    }
    
    /* 超时也可能"手里攥着半句话"：模块回了东西但没凑成一行（或没包含关键词）。
     * 把残行打出来，现场才知道模块到底说了什么，而不是只有一句笼统的 timeout。 */
    if (line_pos > 0) {
        line[line_pos] = '\0';
        dbg_print("[AT] last: %s\r\n", line);
    }
    return ESP_TIMEOUT;
}

// 简化版：发指令，只等待 "OK"
ESP_Status_t ESP_SendCmd_OK(const char *cmd, uint32_t timeout_ms)
{
    return ESP_SendCmd(cmd, "OK", timeout_ms);
}
