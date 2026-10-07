#include "pal.h"
#include "FreeRTOS.h"
#include "Taskhandel.h"
#include "command.h"
#include "esp_at.h"
#include "flash.h"
#include "libemqtt.h"
#include "stdlib.h"
#include "task.h"
#include "usart.h"
#include <stdio.h>
#include <string.h>

extern uint8_t a[256];

volatile uint8_t wifi_connected =
    0; /* ESP8266 已连上 AP（CWJAP 成功），UI 状态栏据此显示 */

/* —— WiFi 凭据 ——
 * 不要把真实密码明文提交进仓库：这里只是默认占位，构建时可覆盖：
 *   在 CMake 里加 target_compile_definitions(... WIFI_SSID=\"xxx\"
 * WIFI_PASS=\"yyy\") 旧密码如果已经泄露（提交过历史），请到路由器上改掉。 */
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif

/* MQTT CONNECT 报文（v3.1 协议名 MQIsdp，保活 120s，客户端 ID 32 字节） */
static const uint8_t mqtt_connect_packet[] = {
    0x10, 0x2E, // 固定报头

    0x00, 0x06,                         // 协议名长度 = 6 		可变报头
    0x4D, 0x51, 0x49, 0x73, 0x64, 0x70, // 协议名: MQIsdp (MQTT v3.1)
    0x03, 0x02, 0x00, 0x78,             // 协议版本 + 连接标志 + 保活时间

    0x00, 0x20, // 客户端ID长度 = 32     有效载荷
    // 32字节 客户端唯一ID
    '4', '1', 'f', '3', '8', '7', '6', '7', '2', '0', 'a', 'd', '4', 'a', 'e',
    '8', '6', '0', 'd', 'b', '0', '6', 'e', '3', '5', '6', '3', '1', 'a', '8',
    '5', '5'};

/* SUBSCRIBE 报文：订阅主题 stm32，QoS1 */
static const uint8_t mqtt_subscribe_packet[] = {
    0x82, 0x0A, // 固定报头

    0x00, 0x01, // 可变报头  报文标识符 Packet ID = 1

    0x00, 0x05,                // 有效载荷  主题名长度 = 5
    's',  't',  'm', '3', '2', // 主题名：stm32
    0x01                       // 订阅QoS等级 = 1
};

/* 等待并校验 CONNACK：0x20 0x02 xx 0x00（返回码 0 = 接受连接）。
 * 只有这一步通过才允许置在线。此前 RE_MQTT_Init 在"一个字节都没收到"
 * 时盲发 30 次连接包后无条件置在线，是无网时状态被锁死在线的根源。 */
static int mqtt_wait_connack(void) {
  uint8_t connack[4];
  int ret = pal_tcp_recv_raw(0, connack, 4, 3000);
  return (ret == 4 && connack[0] == 0x20 && connack[1] == 0x02 &&
          connack[3] == 0x00);
}

/* ESP8266 uart1 波特率115200 DMA空闲中断*/
//+++正确退出透传后 下一条语句就不会输出error
void MQTT_Init(void) {
  HAL_UART_Transmit(&huart4, (uint8_t *)"+++\r\n", 3, 100);
  vTaskDelay(1000);

  // 任意合法AT指令，完成模式切换（不操作TCP，安全无断连）
  ESP_SendCmd_OK("AT\r\n", 2000);
  vTaskDelay(200);

  ESP_SendCmd_OK("AT+CWMODE=1\r\n", 2000);
  if (ESP_SendCmd("AT+CWJAP=\"" WIFI_SSID "\",\"" WIFI_PASS "\"\r\n",
                  "WIFI GOT IP", 5000) == ESP_OK)
    wifi_connected = 1;
  else
    wifi_connected = 0;
  vTaskDelay(500);

  // 建立TCP连接
  ESP_SendCmd("AT+CIPSTART=\"TCP\",\"bemfa.com\",9501\r\n", "CONNECT", 2000);
  vTaskDelay(500);

  // 进入透传模式
  ESP_SendCmd_OK("AT+CIPMODE=1\r\n", 2000);
  // 启动透传
  ESP_SendCmd("AT+CIPSEND\r\n", ">", 5000);
  vTaskDelay(500);
  RingBuf_Clear();

  HAL_UART_Transmit(&huart4, (uint8_t *)mqtt_connect_packet,
                    sizeof(mqtt_connect_packet), 5000);
  vTaskDelay(300);

  if (mqtt_wait_connack()) {
    // 连接成功后 订阅主题
    HAL_UART_Transmit(&huart4, (uint8_t *)mqtt_subscribe_packet,
                      sizeof(mqtt_subscribe_packet), 5000);
    vTaskDelay(300);
    g_mqtt_connected = 1;
    Log_Write(LOG_INFO, "CONNECT FINISH");
  } else {
    g_mqtt_connected = 0;
  }
}

/* 断线重连：与 MQTT_Init 同一套验证标准，收到合法 CONNACK 才置在线；
 * 失败则保持 0，等下一轮 10s 重试。旧版"盲发 30 次 + 无条件置在线 + 置
 * connect=1 锁死离线判定"的逻辑已删除。 */
void RE_MQTT_Init(void) {
  HAL_UART_Transmit(&huart4, (uint8_t *)"+++\r\n", 3, 100);
  vTaskDelay(1000);

  ESP_SendCmd_OK("AT+RST\r\n", 2000); /* 复位，退出异常透传/半连接状态 */
  vTaskDelay(pdMS_TO_TICKS(1000));
  ESP_SendCmd_OK("AT+RST\r\n", 2000);
  vTaskDelay(pdMS_TO_TICKS(1000));

  ESP_SendCmd_OK("AT+CWMODE=1\r\n", 2000);
  if (ESP_SendCmd("AT+CWJAP=\"" WIFI_SSID "\",\"" WIFI_PASS "\"\r\n",
                  "WIFI GOT IP", 5000) == ESP_OK)
    wifi_connected = 1;
  else
    wifi_connected = 0;
  ESP_SendCmd("AT+CIPSTART=\"TCP\",\"bemfa.com\",9501\r\n", "CONNECT", 5000);
  ESP_SendCmd_OK("AT+CIPMODE=1\r\n", 2000);
  ESP_SendCmd("AT+CIPSEND\r\n", ">", 5000);
  vTaskDelay(500);
  RingBuf_Clear();

  HAL_UART_Transmit(&huart4, (uint8_t *)mqtt_connect_packet,
                    sizeof(mqtt_connect_packet), 5000);
  vTaskDelay(300);

  if (mqtt_wait_connack()) {
    HAL_UART_Transmit(&huart4, (uint8_t *)mqtt_subscribe_packet,
                      sizeof(mqtt_subscribe_packet), 5000);
    vTaskDelay(300);
    g_mqtt_connected = 1;
    Log_Write(LOG_INFO, "reconnect success");
  } else {
    g_mqtt_connected = 0;
    Log_Write(LOG_WARN, "reconnect failed");
  }
}

// ==================== MQTT 心跳包 ====================
void MQTT_SendPing(void) {
  uint8_t ping[] = {0xC0, 0x00};
  HAL_UART_Transmit(&huart4, ping, 2, 2000);
}

/**
 * @brief  透传模式下从 ESP8266 接收原始 TCP 数据
 * @param  sock       未使用（保留参数，兼容之前的接口设计）
 * @param  buf        接收缓冲区指针
 * @param  len        期望接收的字节数
 * @param  timeout_ms 超时时间（毫秒）
 * @return 实际接收到的字节数，超时或失败返回 -1
 */

int pal_tcp_recv_raw(int sock, uint8_t *buf, int len, int timeout_ms) {
  uint32_t start = HAL_GetTick();
  int received = 0;
  while (received < len) {
    if (HAL_GetTick() - start > timeout_ms) {
      return -1;
    }
    uint8_t ch;
    while (RingBuf_Read(&ch) && received < len) {
      buf[received++] = ch;
    }
    // HAL_Delay(1); // 避免CPU空转
    vTaskDelay(1);
  }
  return received;
}