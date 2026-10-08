#ifndef PAL_H
#define PAL_H

#include "main.h"
#include "stdio.h"
#include "stm32f4xx.h"
#include "usart.h"
#include <string.h>
#include "Taskhandel.h"     /* Data_t */

/* ============================================================
 *  云端/连接配置 —— 唯一真值源（改这里，别动 pal.c）
 * ============================================================ */
/* WiFi */
#define WIFI_SSID  "Magic7"
#define WIFI_PASS  "121qweqwe"

/* 云平台选择：CLOUD_E2 / CLOUD_ONENET
 * 也可以构建时覆盖：-DCLOUD_TARGET=CLOUD_ONENET */
#define CLOUD_E2      0
#define CLOUD_ONENET  1
#ifndef CLOUD_TARGET
#define CLOUD_TARGET  CLOUD_ONENET
#endif

/* E2 平台（CLOUD_TARGET=CLOUD_E2 时生效；设备需先在 E2 注册，凭证必须匹配） */
#ifndef E2_HOST
#define E2_HOST          "192.168.0.49"
#endif
#ifndef E2_PRODUCT_KEY
#define E2_PRODUCT_KEY   "factory_sensor"
#endif
#ifndef E2_DEVICE_ID
#define E2_DEVICE_ID     "dev_gateway_001"
#endif
#ifndef E2_DEVICE_SECRET
#define E2_DEVICE_SECRET "CHANGE_ME"
#endif

/* OneNET Studio（CLOUD_TARGET=CLOUD_ONENET 时生效）
 * token 在控制台生成；注意 et= 是过期时间戳，到期全部设备掉线，需重新生成 */
#ifndef ONENET_HOST
#define ONENET_HOST       "mqtts.heclouds.com"
#endif
#ifndef ONENET_PRODUCT_ID
#define ONENET_PRODUCT_ID "WW0f6843I6" // Username
#endif
#ifndef ONENET_DEVICE_ID
#define ONENET_DEVICE_ID  "humi_temp" // ClientID
#endif
#ifndef ONENET_TOKEN
#define ONENET_TOKEN      "version=2018-10-31&res=products%2FWW0f6843I6%2Fdevices%2Fhumi_temp&et=1822724100&method=sha1&sign=PtFUppXWHY6iMg%2BEMqieAJyGZLo%3D"
#endif

/* 透传模式下从 ESP8266 接收原始 TCP 数据 */
int pal_tcp_recv_raw(int sock, uint8_t *buf, int len, int timeout_ms);


void MQTT_Init(void);
void RE_MQTT_Init(void);
void MQTT_SendPing(void);

/* 统一云端发布接口：topic/QoS/payload 格式由 CLOUD_TARGET 的 profile 决定。
 * QoS1 平台（E2/OneNET）以收到 PUBACK 为成功依据，返回 0=成功。 */
void pal_mqtt_build_payload(char *out, size_t n, const Data_t *d);
int  pal_mqtt_publish(const char *payload);

#endif
