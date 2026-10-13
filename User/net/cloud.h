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


/* 透传模式下从 ESP8266 接收原始 TCP 数据 */
int esp_recv_raw(int sock, uint8_t *buf, int len, int timeout_ms);


void MQTT_Init(void);
void RE_MQTT_Init(void);
void MQTT_SendPing(void);

/* 统一云端发布接口：topic/QoS/payload 格式由 CLOUD_TARGET 的 profile 决定。
 * QoS1 平台（IOTSystem/OneNET）以收到 PUBACK 为成功依据，返回 0=成功。 */
void cloud_build_payload(char *out, size_t n, const Data_t *d);
int  cloud_publish(const char *payload);

#endif
