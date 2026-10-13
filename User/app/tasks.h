#ifndef TASKHANDEL_H
#define TASKHANDEL_H
#include "stdint.h"



void Dog_task();
void MQTT();
void AT();
void UI();
void Modbus();
extern volatile uint8_t wifi_connected;   // ESP8266 已连上 AP（cloud.c 维护）
extern volatile uint8_t g_mqtt_connected; // 云端 MQTT "连接已建立"（tasks.c 维护）

/* 最近一次"云端链路确实活着"的时刻（xTaskGetTickCount）。
 * 只有三种东西能刷新它：CONNACK、PUBACK、PINGRESP —— 都是云端真的回了字节。
 * 由 cloud.c（连接/发布）与 tasks.c 的 AT 任务（心跳）维护。 */
extern volatile uint32_t g_cloud_alive_tick;

/* 界面（主屏 / WiFi 页 / 日志页返回）用的"云端在线"判据。
 *
 * 为什么不直接用 g_mqtt_connected：
 *   g_mqtt_connected 是"连接建立过"的闩锁，置 1 后要等心跳**连续失败 3 次**
 *   （≈30s）才清 0；云平台那边则是 keepalive 超时（本例 120s）才判离线。
 *   于是链路真断了之后，屏幕还会理直气壮地显示 ONLINE 十几秒甚至几十秒，
 *   和平台状态对不上（现场表现就是"屏幕说在线、平台说离线"）。
 *
 * 这个判据额外要求"最近 CLOUD_ALIVE_TIMEOUT_MS 内收到过云端应答"，
 * 把那段幽灵在线窗口消掉。它只用于**显示**，不参与重连决策 ——
 * 重连仍走"心跳连败 3 次"的去抖，避免一次抖动就 AT+RST 全量重建。 */
uint8_t cloud_online_now(void);

// 温湿度数据结构体

typedef struct {
    float temp;   // 温度
    float shi;    // 湿度
	 float light;   //02#光照
    uint8_t valid; // 数据有效性掩码：bit0=温湿度成功读到过 bit1=光照成功读到过
} Data_t;



#endif
