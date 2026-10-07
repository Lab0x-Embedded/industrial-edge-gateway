#include "pal.h"
#include "command.h"
#include "esp_at.h"
#include "flash.h"
#include "task.h"
#include "usart.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "FreeRTOS.h"
#include "Taskhandel.h"

extern uint8_t a[256];

volatile uint8_t wifi_connected =
    0; /* ESP8266 已连上 AP（CWJAP 成功），UI 状态栏据此显示 */

/* ============================================================
 *  云平台 profile —— 三个平台都是"明文 TCP + 标准 MQTT"，
 *  差异全部收敛在这张表里；新增平台 = 加一行表项 + payload 分支。
 *  （OneNET token 在 pal.h 配置，et 过期后需重新生成并替换）
 * ============================================================ */
typedef struct {
  const char *host;         /* CIPSTART 目标 */
  uint16_t port;
  const char *proto_name;   /* "MQIsdp"=MQTT v3.1，"MQTT"=v3.1.1 */
  uint8_t proto_level;
  uint8_t connect_flags;    /* bit1 clean-session, bit7 username, bit6 password */
  const char *client_id;
  const char *username;     /* 无认证填 "" */
  const char *password;
  uint16_t keepalive;
  const char *topic_pub;
  const char *topic_sub;
  uint8_t qos1;             /* 1 = 发布后等 PUBACK 才算成功 */
} CloudProfile;

static const CloudProfile profiles[] = {
    [CLOUD_BEMFA] =
        {
            .host = BEMFA_BROKER,
            .port = BEMFA_PORT,
            .proto_name = "MQIsdp",
            .proto_level = 3,
            .connect_flags = 0x02,
            .client_id = BEMFA_UID,
            .username = "",
            .password = "",
            .keepalive = 120,
            .topic_pub = TOPIC_PUB,
            .topic_sub = TOPIC_PUB,
            .qos1 = 0,
        },
    [CLOUD_E2] =
        {
            .host = E2_HOST,
            .port = 1883,
            .proto_name = "MQTT",
            .proto_level = 4,
            .connect_flags = 0xC2,
            .client_id = E2_DEVICE_ID,
            .username = E2_PRODUCT_KEY,
            .password = E2_DEVICE_SECRET,
            .keepalive = 60,
            .topic_pub = "devices/" E2_DEVICE_ID "/data",
            .topic_sub = "cmd/" E2_DEVICE_ID "/exec",
            .qos1 = 1,
        },
    [CLOUD_ONENET] =
        {
            .host = ONENET_HOST,
            .port = 1883,
            .proto_name = "MQTT",
            .proto_level = 4,
            .connect_flags = 0xC2,
            .client_id = ONENET_DEVICE_ID,
            .username = ONENET_PRODUCT_ID,
            .password = ONENET_TOKEN,
            .keepalive = 120,
            .topic_pub =
                "$sys/" ONENET_PRODUCT_ID "/" ONENET_DEVICE_ID
                "/thing/property/post",
            .topic_sub = "$sys/" ONENET_PRODUCT_ID "/" ONENET_DEVICE_ID
                         "/thing/property/set",
            .qos1 = 1,
        },
};

/* payload 格式按平台出（键名/信封不同），业务层只给 Data_t */
void pal_mqtt_build_payload(char *out, size_t n, const Data_t *d) {
#if CLOUD_TARGET == CLOUD_E2
  /* E2 数据点格式：datapoints[{metric,value}]，metric 需与平台定义一致 */
  snprintf(out, n,
           "{\"datapoints\":[{\"metric\":\"temperature\",\"value\":%.1f},"
           "{\"metric\":\"humidity\",\"value\":%.1f},"
           "{\"metric\":\"light\",\"value\":%.0f}]}",
           d->temp, d->shi, d->light);
#elif CLOUD_TARGET == CLOUD_ONENET
  /* OneNET 物模型属性上报信封；属性名与控制台物模型逐字一致 */
  snprintf(out, n,
           "{\"id\":\"%u\",\"params\":{\"temperature\":%.1f,"
           "\"humidity\":%.1f,\"light\":%.0f}}",
           (unsigned)(HAL_GetTick() / 1000), d->temp, d->shi, d->light);
#else /* CLOUD_BEMFA */
  snprintf(out, n, "{\"temp\":%.1f,\"humi\":%.1f,\"light\":%.0f}", d->temp,
           d->shi, d->light);
#endif
}

/* ============ MQTT 报文工具（全部按 profile 动态拼装） ============ */

/* CONNECT：固定头 + 变长剩余长度 + [proto][level][flags][keepalive]
 *          + [clientID][username?][password?] */
static int mqtt_send_connect(void) {
  const CloudProfile *p = &profiles[CLOUD_TARGET];
  uint8_t pkt[320];
  uint8_t *w = pkt;
  size_t nl = strlen(p->proto_name);
  size_t cl = strlen(p->client_id);
  size_t ul = strlen(p->username);
  size_t pl = strlen(p->password);
  size_t rem = 2 + nl + 4 + 2 + cl +
               ((p->connect_flags & 0x80) ? 2 + ul : 0) +
               ((p->connect_flags & 0x40) ? 2 + pl : 0);

  if (rem + 5 > sizeof pkt)
    return -1; /* token 过长保护 */
  *w++ = 0x10;
  {
    size_t r = rem;
    do {
      uint8_t b = r % 128;
      r /= 128;
      *w++ = r ? b | 0x80 : b;
    } while (r);
  }

  *w++ = (uint8_t)(nl >> 8);
  *w++ = (uint8_t)nl;
  memcpy(w, p->proto_name, nl);
  w += nl;
  *w++ = p->proto_level;
  *w++ = p->connect_flags;
  *w++ = (uint8_t)(p->keepalive >> 8);
  *w++ = (uint8_t)p->keepalive;
  *w++ = (uint8_t)(cl >> 8);
  *w++ = (uint8_t)cl;
  memcpy(w, p->client_id, cl);
  w += cl;
  if (p->connect_flags & 0x80) {
    *w++ = (uint8_t)(ul >> 8);
    *w++ = (uint8_t)ul;
    memcpy(w, p->username, ul);
    w += ul;
  }
  if (p->connect_flags & 0x40) {
    *w++ = (uint8_t)(pl >> 8);
    *w++ = (uint8_t)pl;
    memcpy(w, p->password, pl);
    w += pl;
  }

  HAL_UART_Transmit(&huart4, pkt, (uint16_t)(w - pkt), 5000);
  vTaskDelay(300);
  return 0;
}

/* SUBSCRIBE：订 profile 的下行主题，QoS1，报文标识符固定 1 */
static int mqtt_send_subscribe(void) {
  const CloudProfile *p = &profiles[CLOUD_TARGET];
  uint8_t pkt[128];
  uint8_t *w = pkt;
  size_t tl = strlen(p->topic_sub);
  size_t rem = 2 + 2 + tl + 1;

  if (rem + 2 > sizeof pkt)
    return -1;
  *w++ = 0x82;
  *w++ = (uint8_t)rem;
  *w++ = 0x00;
  *w++ = 0x01; /* Packet ID */
  *w++ = (uint8_t)(tl >> 8);
  *w++ = (uint8_t)tl;
  memcpy(w, p->topic_sub, tl);
  w += tl;
  *w++ = 0x01; /* 请求 QoS1 */

  HAL_UART_Transmit(&huart4, pkt, (uint16_t)(w - pkt), 5000);
  vTaskDelay(300);
  return 0;
}

/* 等待并校验 CONNACK：0x20 0x02 xx 0x00（返回码 0 = 接受连接）。
 * 只有这一步通过才允许置在线。 */
static int mqtt_wait_connack(void) {
  uint8_t connack[4];
  int ret = pal_tcp_recv_raw(0, connack, 4, 3000);
  return (ret == 4 && connack[0] == 0x20 && connack[1] == 0x02 &&
          connack[3] == 0x00);
}

/* ESP8266 uart1 波特率115200 DMA空闲中断*/
//+++正确退出透传后 下一条语句就不会输出error
void MQTT_Init(void) {
  const CloudProfile *p = &profiles[CLOUD_TARGET];
  char cmd[96];

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

  snprintf(cmd, sizeof cmd, "AT+CIPSTART=\"TCP\",\"%s\",%u\r\n", p->host,
           (unsigned)p->port);
  ESP_SendCmd(cmd, "CONNECT", 2000);
  vTaskDelay(500);

  // 进入透传模式
  ESP_SendCmd_OK("AT+CIPMODE=1\r\n", 2000);
  // 启动透传
  ESP_SendCmd("AT+CIPSEND\r\n", ">", 5000);
  vTaskDelay(500);
  RingBuf_Clear();

  mqtt_send_connect();
  if (mqtt_wait_connack()) {
    mqtt_send_subscribe();
    g_mqtt_connected = 1;
    Log_Write(LOG_INFO, "CONNECT FINISH");
  } else {
    g_mqtt_connected = 0;
  }
}

/* 断线重连：与 MQTT_Init 同一套验证标准，收到合法 CONNACK 才置在线；
 * 失败则保持 0，等下一轮 10s 重试。 */
void RE_MQTT_Init(void) {
  const CloudProfile *p = &profiles[CLOUD_TARGET];
  char cmd[96];

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
  snprintf(cmd, sizeof cmd, "AT+CIPSTART=\"TCP\",\"%s\",%u\r\n", p->host,
           (unsigned)p->port);
  ESP_SendCmd(cmd, "CONNECT", 5000);
  ESP_SendCmd_OK("AT+CIPMODE=1\r\n", 2000);
  ESP_SendCmd("AT+CIPSEND\r\n", ">", 5000);
  vTaskDelay(500);
  RingBuf_Clear();

  mqtt_send_connect();
  if (mqtt_wait_connack()) {
    mqtt_send_subscribe();
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

/* 统一发布入口：PUBLISH 到 profile 的 topic_pub；
 * QoS1 平台（E2/OneNET）等 PUBACK，收到才返回 0。 */
int pal_mqtt_publish(const char *payload) {
  const CloudProfile *p = &profiles[CLOUD_TARGET];
  uint8_t pkt[512];
  uint8_t *w = pkt;
  size_t tl = strlen(p->topic_pub);
  size_t pl = strlen(payload);
  size_t rem = 2 + tl + (p->qos1 ? 2 : 0) + pl;

  if (rem + 5 > sizeof pkt)
    return -1; /* payload 超长（上限约 480 字节） */
  *w++ = p->qos1 ? 0x32 : 0x30; /* QoS1: 0x32，QoS0: 0x30 */
  {
    size_t r = rem;
    do {
      uint8_t b = r % 128;
      r /= 128;
      *w++ = r ? b | 0x80 : b;
    } while (r);
  }
  *w++ = (uint8_t)(tl >> 8);
  *w++ = (uint8_t)tl;
  memcpy(w, p->topic_pub, tl);
  w += tl;
  if (p->qos1) {
    *w++ = 0x00;
    *w++ = 0x01; /* Packet ID */
  }
  memcpy(w, payload, pl);
  w += pl;

  HAL_UART_Transmit(&huart4, pkt, (uint16_t)(w - pkt), 5000);

  if (p->qos1) {
    uint8_t ack[4];
    if (pal_tcp_recv_raw(0, ack, 4, 2000) != 4 || ack[0] != 0x40)
      return -1; /* 未收到 PUBACK */
  }
  return 0;
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
