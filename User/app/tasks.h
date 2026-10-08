#ifndef TASKHANDEL_H
#define TASKHANDEL_H
#include "stdint.h"



void Dog_task();
void MQTT();
void AT();
void UI();
void Modbus();
void page_log_refresh(void);
extern volatile uint8_t wifi_connected;   // ESP8266 已连上 AP（cloud.c 维护）
extern volatile uint8_t g_mqtt_connected; // 云端 MQTT 在线（tasks.c 维护）

// 温湿度数据结构体

typedef struct {
    float temp;   // 温度
    float shi;    // 湿度
	 float light;   //02#光照
    uint8_t valid; // 数据有效性掩码：bit0=温湿度成功读到过 bit1=光照成功读到过
} Data_t;



#endif
