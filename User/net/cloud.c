#include "cloud.h"
#include "wifi_conn.h"
#include "w25qxx.h"
#include "esp_uart.h"
#include "esp_at.h"
#include "log.h"
#include "task.h"
#include "usart.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "FreeRTOS.h"
#include "tasks.h"

extern uint8_t a[256];

/* ============================================================
 *  云平台 profile —— 两个平台都是"明文 TCP + 标准 MQTT"，
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
void cloud_build_payload(char *out, size_t n, const Data_t *d) {
#if CLOUD_TARGET == CLOUD_E2
  /* E2 数据点格式：datapoints[{metric,value}]，metric 需与平台定义一致；
   * 只上报有效字段（valid 掩码），全 0 假数据不上云 */
  size_t k = (size_t)snprintf(out, n, "{\"datapoints\":[");
  int first = 1;
  if ((d->valid & 1) && k < n) {
    k += (size_t)snprintf(out + k, n - k,
        "%s{\"metric\":\"temperature\",\"value\":%.1f},"
        "{\"metric\":\"humidity\",\"value\":%.1f}",
        first ? "" : ",", d->temp, d->shi);
    first = 0;
  }
  if ((d->valid & 2) && k < n) {
    k += (size_t)snprintf(out + k, n - k,
        "%s{\"metric\":\"light\",\"value\":%.0f}",
        first ? "" : ",", d->light);
    first = 0;
  }
  if (k < n)
    snprintf(out + k, n - k, "]}");
#elif CLOUD_TARGET == CLOUD_ONENET
  /* OneNET Studio 物模型属性上报：params 里每个属性是 {identifier:{value:..}}；
   * 属性名与控制台物模型逐字一致，否则整条被拒（code 10411）；
   * 只上报有效字段（valid 掩码），全 0 假数据不上云 */
  size_t k = (size_t)snprintf(out, n, "{\"id\":\"%u\",\"params\":{",
                              (unsigned)(HAL_GetTick() / 1000));
  int first = 1;
  if ((d->valid & 1) && k < n) {
    k += (size_t)snprintf(out + k, n - k,
        "%s\"temperature\":{\"value\":%.1f},"
        "\"humidity\":{\"value\":%.1f}",
        first ? "" : ",", d->temp, d->shi);
    first = 0;
  }
  if ((d->valid & 2) && k < n) {
    k += (size_t)snprintf(out + k, n - k,
        "%s\"light\":{\"value\":%.0f}",
        first ? "" : ",", d->light);
    first = 0;
  }
  if (k < n)
    snprintf(out + k, n - k, "}}");
#else
#error "未知 CLOUD_TARGET（见 pal.h：CLOUD_E2 / CLOUD_ONENET）"
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
 * 只有这一步通过才允许置在线。失败时把实际收到的字节打到调试口：
 * 第 4 字节是返回码（4=用户名/密码被拒、5=未授权），
 * 一个字节都没有则说明 CONNECT 没到平台（模块没在真透传/链路已断）。 */
static int mqtt_wait_connack(void) {
  uint8_t connack[4];
  int ret = esp_recv_raw(0, connack, 4, 3000);
  if (ret == 4 && connack[0] == 0x20 && connack[1] == 0x02 &&
      connack[3] == 0x00)
    return 1;
  if (ret > 0) {
    dbg_print("[MQTT] connack(%d):", ret);
    for (int i = 0; i < ret && i < 4; i++)
      dbg_print(" %02X", connack[i]);
    dbg_print("\r\n");
  } else {
    dbg_print("[MQTT] connack: no reply\r\n");
  }
  return 0;
}

/* ESP8266 uart1 波特率115200 DMA空闲中断*/
/* 关掉模块"上电自动连存储 AP"（CWAUTOCONN 默认 1，CWJAP 成功过一次就会写进
 * 模块 flash）。必须重试到 OK 为止：发令时模块常正忙于自己的自动重连
 * （busy p... 把指令丢掉），单发一次被丢后，之后每次 AT+RST 都会重新触发
 * 自动连 —— 我们发的 CWJAP（无论新网络还是切回旧网络）全部撞死在 busy 窗口里，
 * 现场表现就是"点连接永远 wifi timeout，复位才好"（手册 #27）。 */
static void esp_disable_autoconn(void) {
  for (int i = 0; i < 10; i++) {
    if (ESP_SendCmd_OK("AT+CWAUTOCONN=0\r\n", 2000) == ESP_OK) return;
    vTaskDelay(pdMS_TO_TICKS(300));
  }
  Log_Write(LOG_WARN, "cwautoconn=0 FAIL");
}

//+++正确退出透传后 下一条语句就不会输出error
void MQTT_Init(void) {
  const CloudProfile *p = &profiles[CLOUD_TARGET];
  char cmd[96];

  /* 退透传：带静默期 + 确认 + 重试（wifi_at_escape 内部实现）。
   * 退不出来就**别往下发**——后面每条 AT 指令都会被当成 TCP 数据灌进连接，
   * 模块再也收不到指令，表现就是"怎么都连不上"。 */
  if (wifi_at_escape() != 0) {
    Log_Write(LOG_WARN, "init: no AT mode");
    g_mqtt_connected = 0;
    return;
  }

  ESP_SendCmd_OK("AT+CWMODE=1\r\n", 2000);
  /* 必须重试到 OK（见 esp_disable_autoconn 注释）：以前这里单发一次且不看
   * 返回值，恰逢模块自动重连的 busy 窗口时被静默丢弃，等于从没关掉过。 */
  esp_disable_autoconn();
  if (!wifi_join()) {               /* 失败原因由 wifi_join 自己写日志页 */
    /* 入网没成就别往下走：TCP/透传必然全失败，还把模块留在
     * CIPMODE=1 却无连接的中间态；等下一轮重连再来 */
    g_mqtt_connected = 0;
    return;
  }
  vTaskDelay(500);

  /* 透传前置（Espressif 透传流程的既定顺序）：CIPMUX=0 是 CIPMODE=1 的前提；
   * CIPMODE 必须在 CIPSTART 之前设 —— 部分固件对"TCP 已建立后再设 CIPMODE"
   * 回 ERROR，那正是日志里"没有 tcp fail、只有 passthrough fail"的一种成因。
   * CIPMODE 不进 flash，AT+RST 后清零，所以 RE_MQTT_Init 也要各设一遍。 */
  ESP_SendCmd_OK("AT+CIPMUX=0\r\n", 2000);
  ESP_SendCmd_OK("AT+CIPMODE=1\r\n", 2000);

  snprintf(cmd, sizeof cmd, "AT+CIPSTART=\"TCP\",\"%s\",%u\r\n", p->host,
           (unsigned)p->port);
  /* 5s（原 2s 太短）：域名解析 + TCP 握手正常就要 1~3s，超时后照样往下走，
   * 最后只能看到一句笼统的 connack failed，分不清是 TCP 没起来还是 MQTT 被拒。 */
  if (ESP_SendCmd(cmd, "CONNECT", 5000) != ESP_OK)
    Log_Write(LOG_WARN, "init: tcp fail");
  vTaskDelay(500);

  if (wifi_at_passthrough_enter() != 0) {   /* CIPMODE=1 + CIPSEND，确认拿到 ">" */
    Log_Write(LOG_WARN, "init: passthrough fail");
    g_mqtt_connected = 0;
    return;
  }
  vTaskDelay(500);
  RingBuf_Clear();

  mqtt_send_connect();
  if (mqtt_wait_connack()) {
    mqtt_send_subscribe();
    g_mqtt_connected = 1;
    Log_Write(LOG_INFO, "CONNECT FINISH");
    /* 连上就当场校一次时：原来挂在 10s 心跳循环里且"校过就不再跑"——等得久，
     * 且 RTC 走 LSI 会慢慢漂。这里顺手对表的代价只有几秒，失败不影响在线
     * 状态，心跳循环里还留着"未校时兜底重试"。 */
    if (wifi_time_sync_locked() != 0)
      Log_Write(LOG_WARN, "sntp fail (clock stays dim)");
  } else {
    g_mqtt_connected = 0;
    Log_Write(LOG_WARN, "connack failed");
  }
}

/* 断线重连：与 MQTT_Init 同一套验证标准，收到合法 CONNACK 才置在线；
 * 失败则保持 0，等下一轮 10s 重试。 */
void RE_MQTT_Init(void) {
  const CloudProfile *p = &profiles[CLOUD_TARGET];
  char cmd[96];

  /* 先确认在 AT 指令模式：这里原本是裸发 "+++"（1s 保护时间，不确认是否退成功），
   * 一旦退出失败，后面的 AT+RST 也会被当成透传数据 —— 重连彻底卡死，
   * 每 10s 重复一次同样的失败，现场看就是"怎么都连不上"。 */
  if (wifi_at_escape() != 0) {
    Log_Write(LOG_WARN, "reconnect: no AT mode");
    g_mqtt_connected = 0;
    return;
  }

  ESP_SendCmd_OK("AT+RST\r\n", 2000);      /* 复位：退出异常透传/半连接状态 */
  /* 复位后必须等模块真的起来（AT 固件 1~3s）。原来这里盲等 1s 就发 CWMODE/CWJAP，
   * 指令落在启动窗口里被丢掉 → CWJAP 入不了网 → "点连接永远失败"。 */
  if (wifi_at_wait_ready(8000) != 0) {
    Log_Write(LOG_WARN, "reconnect: no AT after reset");
    g_mqtt_connected = 0;
    return;
  }

  /* RST 会唤醒模块"上电自动连存储 AP"（CWJAP 成功过的那个，= 用户切走前的
   * 旧网络）：不先关掉，它和下面的 CWJAP 撞死在同一 busy 窗口里 —— 开机路径
   * 没有 RST 所以没事，运行时切网/切回永远 "wifi timeout" 的根源（手册 #27）。 */
  esp_disable_autoconn();
  ESP_SendCmd_OK("AT+CWMODE=1\r\n", 2000);
  if (!wifi_join()) {               /* 失败原因由 wifi_join 自己写日志页 */
    g_mqtt_connected = 0;
    return;                         /* 入网没成，TCP/透传必然全失败，别浪费时间 */
  }

  /* 同 MQTT_Init：CIPMUX/CIPMODE 必须在 CIPSTART 之前设好（AT+RST 后 CIPMODE 清零） */
  ESP_SendCmd_OK("AT+CIPMUX=0\r\n", 2000);
  ESP_SendCmd_OK("AT+CIPMODE=1\r\n", 2000);

  snprintf(cmd, sizeof cmd, "AT+CIPSTART=\"TCP\",\"%s\",%u\r\n", p->host,
           (unsigned)p->port);
  if (ESP_SendCmd(cmd, "CONNECT", 5000) != ESP_OK) {
    Log_Write(LOG_WARN, "reconnect: tcp failed");
  }
  if (wifi_at_passthrough_enter() != 0) {
    Log_Write(LOG_WARN, "reconnect: passthrough fail");
    g_mqtt_connected = 0;
    return;
  }
  vTaskDelay(500);
  RingBuf_Clear();

  mqtt_send_connect();
  if (mqtt_wait_connack()) {
    mqtt_send_subscribe();
    g_mqtt_connected = 1;
    Log_Write(LOG_INFO, "reconnect success");
    /* 同 MQTT_Init：连上即校时，别等心跳循环 */
    if (wifi_time_sync_locked() != 0)
      Log_Write(LOG_WARN, "sntp fail (clock stays dim)");
  } else {
    g_mqtt_connected = 0;
    Log_Write(LOG_WARN, "reconnect: connack failed");
  }
}

// ==================== MQTT 心跳包 ====================
void MQTT_SendPing(void) {
  uint8_t ping[] = {0xC0, 0x00};
  HAL_UART_Transmit(&huart4, ping, 2, 2000);
}

/* 消费一条平台下发/回执的 PUBLISH（保持透传字节流对齐）。
 * OneNET 的属性上报回执带 "code"：0=成功，10411=物模型标识符不存在等；
 * 解析到的 payload 会写进日志页，方便在屏幕上直接看平台回了什么。 */
static void mqtt_drain_downlink(void) {
  uint8_t hdr;
  char pl[96];
  size_t plen = 0;

  if (esp_recv_raw(0, &hdr, 1, 600) != 1)
    return; /* 超时=本次没有下行 */
  if ((hdr >> 4) != 0x3)
    return; /* 不是 PUBLISH，丢弃该字节 */

  /* remaining length（变长编码，1~4 字节） */
  uint32_t rem = 0;
  int mult = 1;
  for (int i = 0; i < 4; i++) {
    uint8_t b;
    if (esp_recv_raw(0, &b, 1, 500) != 1)
      return;
    rem += (uint32_t)(b & 0x7F) * mult;
    mult *= 128;
    if (!(b & 0x80))
      break;
  }

  /* QoS1 下行 body = [tlen2][topic][pid2][payload] */
  uint8_t body[240];
  if (rem < 2 || rem > sizeof body) {
    for (uint32_t i = 0; i < rem; i++) {
      uint8_t t;
      esp_recv_raw(0, &t, 1, 10);
    }
    return;
  }
  if (esp_recv_raw(0, body, rem, 500) != (int)rem)
    return;

  uint16_t tlen = ((uint16_t)body[0] << 8) | body[1];
  size_t off = 2 + tlen;
  if (off + 2 > rem)
    return;
  off += 2; /* QoS1 Packet ID */
  plen = rem - off;
  if (plen > sizeof pl - 1)
    plen = sizeof pl - 1;
  memcpy(pl, &body[off], plen);
  pl[plen] = '\0';

  /* 是平台回执（带 code）才写日志，避免刷屏 */
  if (strstr(pl, "\"code\"") != NULL) {
    Log_Write(strstr(pl, "\"code\":0") != NULL ? LOG_INFO : LOG_WARN, pl);
  }
}

/* 统一发布入口：PUBLISH 到 profile 的 topic_pub；
 * QoS1 平台（E2/OneNET）等 PUBACK，收到才返回 0。 */
int cloud_publish(const char *payload) {
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
    if (esp_recv_raw(0, ack, 4, 2000) != 4 || ack[0] != 0x40) {
      return -1; /* 未收到 PUBACK */
    }
    mqtt_drain_downlink(); /* 平台回执（code）进日志页，同时保持字节流对齐 */
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

int esp_recv_raw(int sock, uint8_t *buf, int len, int timeout_ms) {
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
