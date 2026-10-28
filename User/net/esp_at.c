#include "esp_at.h"
#include "esp_uart.h"
#include "usart.h"
#include <string.h>
#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"

/* dbg_print 的声明由 esp_uart.h → main.h 提供（开关关闭时是空宏，见 main.h）；
 * 不要在这里再写 extern 函数声明，函数式宏会把 "dbg_print(" 展开导致编译错误 */

// 引用阶段一创建的全局环形缓冲区
extern uint8_t buffer[BUFFER_SIZE];

// 清空环形缓冲区（丢弃所有旧数据）
static void ESP_ClearRxBuf(void) {
    uint8_t dummy;
    while (RingBuf_Read(&dummy));
}


/* ============================================================
 *  通用 AT 指令发送并逐行解析应答
 *  cmd           : 要发送的 AT 指令（必须包含 \r\n）
 *  expected_ack  : 期望出现的应答关键词，如 "OK", "CONNECT", "WIFI GOT IP"
 *  timeout_ms    : 总超时时间（毫秒）
 *  返回          : ESP_OK(成功), ESP_TIMEOUT(超时), ESP_ERROR(错误)
 *
 *  超时语义 —— 必须是"总超时"，两头都不能走极端：
 *    不能"每收一个字节就重置超时"：RX 悬空/线路噪声持续喂垃圾字节时，
 *    等待被无限续命，15s 的指令可能等远超 15s。
 *    也不能"连续 N ms 没数据就提前退出"：CWJAP / CIPSTART 回显完之后
 *    要静默 3~15 秒，WIFI GOT IP / CONNECT 是完成时一次性吐出来的，
 *    N 只要小于这个静默窗口，指令就被掐死在半路 —— 表现是每次连接
 *    必报 wifi timeout / tcp fail，但 TCP 其实过几秒就建好了。
 *
 *  检查顺序 —— 先 FAIL/ERROR 再 expected_ack：
 *    CIPSTART 失败应答是 "CONNECT FAIL"，恰好含匹配词 "CONNECT"；
 *    顺序反了会被当成成功放行，随后 CIPSEND 必然拿不到 ">"，现场看到
 *    的就是"没有 tcp fail、只有 passthrough fail"。（CWJAP 失败应答
 *    以 "FAIL" 结尾、DNS 失败是 "DNS Fail"，顺带一并覆盖。）
 * ============================================================ */
ESP_Status_t ESP_SendCmd(const char *cmd, const char *expected_ack, uint32_t timeout_ms)
{
    // 1. 清空环形缓冲区，丢弃旧数据
    ESP_ClearRxBuf();

    // 2. 发送指令 ESP8266 uart1波特率115200
    HAL_UART_Transmit(&huart4, (uint8_t *)cmd, strlen(cmd), 1000);

    // 3. 总超时窗口内逐字节读、按行解析
    char line[512];          // 存放一行数据
    uint16_t line_pos = 0;   // 当前行写入位置
    uint32_t start_tick = HAL_GetTick();   // 总超时起点，只设置一次

    while ((HAL_GetTick() - start_tick) < timeout_ms) {
        uint8_t ch;
        if (RingBuf_Read(&ch)) {
            if (ch == '\n') {
                // 一行结束
                line[line_pos] = '\0';

                // 先判失败，再判期望（顺序原因见函数头注释）
                if (strstr(line, "FAIL") != NULL || strstr(line, "Fail") != NULL) {
                    dbg_print("[AT] fail: %s\r\n", line);
                    return ESP_ERROR;
                }
                if (strstr(line, "ERROR") != NULL) {
                    dbg_print("[AT] err: %s\r\n", line);
                    return ESP_ERROR;
                }
                if (expected_ack != NULL && strstr(line, expected_ack) != NULL) {
                    return ESP_OK;
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
        else {
            vTaskDelay(1);   // 没数据：让出 CPU 等下一个 tick（原来是纯忙等）
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
