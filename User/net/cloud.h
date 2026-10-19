#ifndef CLOUD_H
#define CLOUD_H

#include "main.h"
#include "stdio.h"
#include "stm32f4xx.h"
#include "usart.h"
#include <string.h>
#include "tasks.h"     /* Data_t */
#include <stdarg.h>
/* ============================================================
 *  云端/连接配置 —— 凭据不再写死在源码里
 *  真值来源：仓库根目录的 .env（已被 .gitignore 忽略，模板
 *  .env.example）。CMake 配置时解析该文件并以编译宏 -D 注入，
 *  覆盖下面的占位默认值。找不到 .env 时用占位值（连接会被
 *  平台拒绝，属预期）。
 * ============================================================ */
/* 云平台选择：CLOUD_IOTSYSTEM / CLOUD_ONENET
 * 优先级：.env 的 CLOUD_TARGET > 下面的默认值 */
#define CLOUD_IOTSYSTEM 0
#define CLOUD_ONENET    1
#ifndef CLOUD_TARGET
#define CLOUD_TARGET    CLOUD_ONENET
#endif

/* IOTSystem 平台（自建，CLOUD_TARGET=CLOUD_IOTSYSTEM 时生效；设备需先在 IOTSystem 注册，凭证必须匹配）
 * 真实值写在 .env，这里只留占位 */
#ifndef IOTSYSTEM_HOST
#define IOTSYSTEM_HOST          "set-in-.env"
#endif
#ifndef IOTSYSTEM_PRODUCT_KEY
#define IOTSYSTEM_PRODUCT_KEY   "set-in-.env"
#endif
#ifndef IOTSYSTEM_DEVICE_ID
#define IOTSYSTEM_DEVICE_ID     "set-in-.env"
#endif
#ifndef IOTSYSTEM_DEVICE_SECRET
#define IOTSYSTEM_DEVICE_SECRET "set-in-.env"
#endif

/* OneNET Studio（CLOUD_TARGET=CLOUD_ONENET 时生效）
 * token 在控制台生成；注意 et= 是过期时间戳，到期全部设备掉线，需重新生成 */
#ifndef ONENET_HOST
#define ONENET_HOST       "mqtts.heclouds.com"
#endif
#ifndef ONENET_PRODUCT_ID
#define ONENET_PRODUCT_ID "set-in-.env" /* Username */
#endif
#ifndef ONENET_DEVICE_ID
#define ONENET_DEVICE_ID  "set-in-.env" /* ClientID */
#endif
#ifndef ONENET_TOKEN
#define ONENET_TOKEN      "set-in-.env"
#endif

/* 当前云平台名（关于页显示） */
#if CLOUD_TARGET == CLOUD_IOTSYSTEM
#define CLOUD_PROFILE_NAME  "IOTSystem"
#else
#define CLOUD_PROFILE_NAME  "OneNET"
#endif


#include "mqtt_frame.h"

/* 从透传字节流里读一条完整 MQTT 报文（按长度字段对齐，不丢字节）。
 *
 * 返回 报文总字节数（>0）；-1 超时没数据；-2 单条报文超过 buf_max。
 * 无论返回什么，**已消费的字节都已从环形缓冲取走**，调用方必须按 frame
 * 的类型决定后续动作，不能不处理就继续读下一条。
 *
 * 心跳用它而不是裸读 2 字节：缓冲里可能先来一条平台下发的 PUBLISH，
 * 裸读 2 字节会把它的头部当成 PINGRESP 判定失败（假离线）。 */
int mqtt_read_frame(uint8_t *buf, uint32_t buf_max,
                    MqttFrame *out, uint32_t timeout_ms);

/* 透传模式下从 ESP8266 接收原始 TCP 数据。
 *
 * 返回**实际读到的字节数**（0..len）；只有参数非法才返回 -1。
 * ★ 超时也会返回已读到的部分（而不是 -1）——已读字节不允许丢弃，
 *   否则字节流错位。判断成功请用 `ret == len`，不要用 `ret > 0`。 */
int esp_recv_raw(int sock, uint8_t *buf, int len, int timeout_ms);


void MQTT_Init(void);
void RE_MQTT_Init(void);
void MQTT_SendPing(void);

/* 统一云端发布接口：topic/QoS/payload 格式由 CLOUD_TARGET 的 profile 决定。
 * QoS1 平台（IOTSystem/OneNET）以收到 PUBACK 为成功依据，返回 0=成功。 */
void cloud_build_payload(char *out, size_t n, const Data_t *d);
int  cloud_publish(const char *payload);

#endif
