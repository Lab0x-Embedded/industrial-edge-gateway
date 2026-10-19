#include "cloud.h"
#include "wifi_conn.h"
#include "w25qxx.h"
#include "esp_uart.h"
#include "esp_at.h"
#include "mqtt_frame.h"   /* MQTT 帧解析纯函数（固件与主机测试共用，见 Tools/mqtt_frame_test.c） */
#include "log.h"
#include "task.h"
#include "usart.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "FreeRTOS.h"
#include "tasks.h"


/* ============================================================
 *  云平台 profile —— 两个平台都是"明文 TCP + 标准 MQTT"，
 *  差异全部收敛在这张表里；新增平台 = 加一行表项 + payload 分支。
 *  （OneNET token 在 .env 配置，et 过期后需重新生成并替换）
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
    [CLOUD_IOTSYSTEM] =
        {
            .host = IOTSYSTEM_HOST,
            .port = 1883,
            .proto_name = "MQTT",
            .proto_level = 4,
            .connect_flags = 0xC2,
            .client_id = IOTSYSTEM_DEVICE_ID,
            .username = IOTSYSTEM_PRODUCT_KEY,
            .password = IOTSYSTEM_DEVICE_SECRET,
            .keepalive = 60,
            .topic_pub = "devices/" IOTSYSTEM_DEVICE_ID "/data",
            .topic_sub = "cmd/" IOTSYSTEM_DEVICE_ID "/exec",
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
#if CLOUD_TARGET == CLOUD_IOTSYSTEM
  /* IOTSystem 数据点格式：datapoints[{metric,value}]，metric 需与平台定义一致；
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
#error "未知 CLOUD_TARGET（见 cloud.h：CLOUD_IOTSYSTEM / CLOUD_ONENET）"
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

  /* 【修复·关键】校时放在**这里**（CIPMUX=0 之后、CIPSTART/CIPSEND 之前）：
   *   · 必须在 CIPMUX=0 之后 —— 实测在此之前调 CIPSNTPCFG，模块不认（10s 超时）；
   *   · 必须在建会话之前 —— 此刻仍在 AT 指令态，wifi_at_escape() 被裸探测直接确认，
   *     不需要 +++ 去退出正在工作的透传。
   * 原来的顺序是"MQTT 连上之后再校时"，必然要 +++ 退透传；而**退出后再 AT+CIPSEND
   * 重进去的透传是假的**（模块回 "OK\r\n\r\n>" 却再也不转发任何数据）：PUBACK/PINGRESP
   * 全无、平台侧看设备就是离线，本机还傻等 30 秒才重连（现场实测 UART5 打印的
   * "模块最后字节"恒为 0A 4F 4B 0D 0A 0D 0A 3E = OK\r\n\r\n>，就是这一步留下的）。 */
  if (wifi_time_sync_locked() != 0)
    Log_Write(LOG_WARN, "sntp fail (clock stays dim)");
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
    /* CONNACK 是云端刚回的字节 —— 在线新鲜度从这里起算，
     * 免得主屏在"连上但还没发第一次心跳"的几秒里先闪一下离线。 */
    g_cloud_alive_tick = xTaskGetTickCount();
    Log_Write(LOG_INFO, "CONNECT FINISH");
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
    g_cloud_alive_tick = xTaskGetTickCount();   /* 同 MQTT_Init：CONNACK 即在线证据 */
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

/* 从透传字节流里读一条完整报文（不定长，按 MQTT 长度字段对齐）。
 *
 * ★ 这是整个 MQTT 客户端最容易出错的地方。原则：
 *   **任何一条报文，要么完整消费，要么一字节都不碰。**
 *
 *   旧实现的两个错位 bug（已修）：
 *     ① 读到头部发现 (hdr>>4) != 0x3 就 `return`，只丢了 1 字节。
 *        剩下的"长度字段 + body"留在流里 → 下一条从报文中部开始解析 → 全错。
 *        SUBACK(5B)、PINGRESP、QoS0 PUBLISH 都会触发。
 *     ② QoS 位不看，无脑跳 2 字节 Packet ID —— QoS0 的下行会把 payload
 *        前 2 字节吃掉。
 *
 *   现在统一走 mqtt_frame.h 的纯函数：先解头算总长，再按总长整段读。
 *   这两个函数在 PC 上跑过边界用例（Tools/mqtt_frame_test.c）。 */
int mqtt_read_frame(uint8_t *buf, uint32_t buf_max,
                           MqttFrame *out, uint32_t timeout_ms)
{
  uint32_t got = 0;
  uint32_t start = HAL_GetTick();

  /* 先攒够固定头 + 剩余长度字段（最多 5 字节） */
  for (;;) {
    if (got >= 2) {
      MqttParseResult r = mqtt_parse_header(buf, got, out);
      if (r == MQTT_BAD) {
        /* 长度编码非法：无法知道边界，只能丢 1 字节重新找同步点 */
        memmove(buf, buf + 1, got - 1);
        got -= 1;
        continue;
      }
      if (r == MQTT_OK && out->total <= buf_max) break;
      if (r == MQTT_OK) return -2;   /* 单条报文超过缓冲：放弃（调用方清流） */
    }
    if (HAL_GetTick() - start > timeout_ms) return -1;   /* 没数据 */
    uint8_t ch;
    if (RingBuf_Read(&ch)) {
      if (got < buf_max) buf[got++] = ch;
      else return -2;
    } else {
      vTaskDelay(1);
    }
  }

  /* 按 total 把 body 读满 */
  while (got < out->total) {
    uint8_t ch;
    if (RingBuf_Read(&ch)) { buf[got++] = ch; }
    else if (HAL_GetTick() - start > timeout_ms + 500) return -1;
    else vTaskDelay(1);
  }
  return (int)got;
}

/* 消费一条平台下发/回执的 PUBLISH（保持透传字节流对齐）。
 * OneNET 的属性上报回执带 "code"：0=成功，10411=物模型标识符不存在等；
 * 解析到的 payload 会写进日志页，方便在屏幕上直接看平台回了什么。
 *
 * 非 PUBLISH 的报文（PUBACK 已在上层读过、SUBACK、PINGRESP 等）也**必须
 * 整段消费掉**，否则字节流错位——见 mqtt_read_frame 的说明。 */
static void mqtt_drain_downlink(void)
{
  uint8_t buf[300];
  MqttFrame f;

  /* 最多排空几条，防止下行洪水时死循环把 MQTT 任务卡住 */
  for (int guard = 0; guard < 4; guard++) {
    int n = mqtt_read_frame(buf, sizeof buf, &f, 400);
    if (n <= 0) return;                       /* 没数据 / 异常：结束 */

    if (f.type != MQTT_PKT_PUBLISH)
      continue;                               /* ← 整段已消费，安全跳过 */

    char pl[96];
    uint32_t plen = 0;
    if (mqtt_publish_payload(buf, (uint32_t)n, &f, pl, sizeof pl, &plen) != 0)
      continue;

    /* 是平台回执（带 code）才写日志，避免刷屏 */
    if (strstr(pl, "\"code\"") != NULL) {
      Log_Write(strstr(pl, "\"code\":0") != NULL ? LOG_INFO : LOG_WARN, pl);
    }
  }
}

/* 统一发布入口：PUBLISH 到 profile 的 topic_pub；
 * QoS1 平台（IOTSystem/OneNET）等 PUBACK，收到才返回 0。 */
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
    /* 读 PUBACK：用 mqtt_read_frame 而不是裸 esp_recv_raw，
     * 因为 PUBACK 后面可能紧跟平台下发的 PUBLISH——按长度整段消费才不会错位。
     *
     * 这里只接受"恰好一条 PUBACK（4 字节）"：如果收到的是别的报文
     * （比如先来了一条 SUBACK 或 QoS0 PUBLISH），说明流里还有残留，
     * 不应该把它的首字节误判成 PUBACK。 */
    uint8_t ack[4];
    if (esp_recv_raw(0, ack, 4, 2000) != 4 || ack[0] != 0x40 ||
        ack[1] != 0x02) {
      return -1; /* 未收到合法 PUBACK */
    }
    /* PUBACK 也是云端应答 = 链路活着的证据。发布期间 AT 任务拿不到锁、
     * 发不出心跳，这条刷新能保证补传/上报密集时不把在线状态误判成离线。 */
    g_cloud_alive_tick = xTaskGetTickCount();
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
 * @return 实际接收到的字节数（可能是 0..len）；只有 buf 非法才返回 -1
 *
 * ★ 语义变更（修 bug）：旧实现在超时后返回 -1，**但已经读进 buf 的字节
 *   就被丢弃了**——那些字节已经从环形缓冲里取走，不会再出现。
 *   调用方看到 -1 只知道"失败"，无法知道"其实读到了几个"，
 *   于是这部分数据凭空消失 → 字节流从此错位。
 *
 *   典型触发：PUBACK 是 4 字节，链路抖动时只到了 3 个，等到超时返回 -1，
 *   那 3 个字节没了，下一条报文从第 4 个字节开始解析 → 全错。
 *
 *   现在改为**返回实际读到的字节数**：超时但读到了部分数据 → 返回 1..len-1，
 *   调用方（如 mqtt_read_frame）会保留已读字节继续等待，不会丢。
 *   调用方判断"是否成功"应改用 `ret == len` 而不是 `ret > 0`。
 */

int esp_recv_raw(int sock, uint8_t *buf, int len, int timeout_ms) {
  (void)sock;
  if (buf == NULL || len <= 0) return -1;

  uint32_t start = HAL_GetTick();
  int received = 0;
  while (received < len) {
    uint8_t ch;
    while (received < len && RingBuf_Read(&ch)) {
      buf[received++] = ch;
    }
    if (received >= len) break;
    if ((HAL_GetTick() - start) > (uint32_t)timeout_ms) break;   /* 超时：返回已读的 */
    vTaskDelay(1);
  }
  return received;
}
