#include "esp_at.h"
#include "command.h"
#include "usart.h"
#include <string.h>
#include <stdio.h>
#include "stdlib.h"
#include "libemqtt.h"
#include "pal.h"
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "flash.h"
#include "Taskhandel.h"

extern uint8_t a[256];

/* ESP8266 uart1 波特率115200 DMA空闲中断*/
//+++正确退出透传后 下一条语句就不会输出error
void MQTT_Init(void)
{

	
	  HAL_UART_Transmit(&huart4, (uint8_t *)"+++\r\n", 3, 100);
    vTaskDelay(1000);
	

    // 任意合法AT指令，完成模式切换（不操作TCP，安全无断连）
    ESP_SendCmd_OK("AT\r\n", 2000);
    vTaskDelay(200);
	
    ESP_SendCmd_OK("AT+CWMODE=1\r\n", 2000);
    ESP_SendCmd("AT+CWJAP=\"aaa\",\"18208901719\"\r\n", "WIFI GOT IP", 5000);
		vTaskDelay(500);
	
    // 建立TCP连接
    ESP_SendCmd("AT+CIPSTART=\"TCP\",\"bemfa.com\",9501\r\n", "CONNECT", 2000);
		vTaskDelay(500);


    // 进入透传模式
    ESP_SendCmd_OK("AT+CIPMODE=1\r\n", 2000);
		//启动透传
		ESP_SendCmd("AT+CIPSEND\r\n", ">", 5000);
    vTaskDelay(500);
    RingBuf_Clear();	

uint8_t connect_packet[] = {
        0x10, 0x2E,                // 固定报头
	
        0x00,0x06,                 // 协议名长度 = 6 		可变报头
        0x4D,0x51,0x49,0x73,0x64,0x70, // 协议名: MQIsdp (MQTT v3.1)
        0x03, 0x02, 0x00,0x78,     // 协议版本 + 连接标志 + 保活时间
	
        0x00,0x20,                 // 客户端ID长度 = 32     有效载荷
        // 32字节 客户端唯一ID
        '4','1','f','3','8','7','6','7',
        '2','0','a','d','4','a','e','8',
        '6','0','d','b','0','6','e','3',
        '5','6','3','1','a','8','5','5'
};
				// 串口发送整包
				HAL_UART_Transmit(&huart4, connect_packet, sizeof(connect_packet), 5000);
				vTaskDelay(300);
	
    // 解析CONNACK应答
    uint8_t connack[4];
		int ret = pal_tcp_recv_raw(0, connack, 4, 3000);			//读取缓冲区 对应字节数
    if (ret == 4 && connack[0] == 0x20 && connack[1] == 0x02 && connack[3] == 0x00)
    {     
       
        // 连接成功后 订阅主题
		uint8_t sub_packet[] = {
				0x82, 0x0A,          // 固定报头
			
				0x00, 0x01,          // 可变报头  报文标识符 Packet ID = 1
			
				0x00, 0x05,          // 有效载荷  主题名长度 = 5
				's','t','m','3','2', // 主题名：stm32
				0x01                 // 订阅QoS等级 = 1
		};
		HAL_UART_Transmit(&huart4, sub_packet, sizeof(sub_packet), 5000);
		vTaskDelay(300);
		g_mqtt_connected = 1;
		Log_Write(LOG_INFO, "CONNECT FINISH");
				
    } 
    else 
    {
        g_mqtt_connected = 0;
    }
}

void RE_MQTT_Init(void)
{
    HAL_UART_Transmit(&huart4, (uint8_t *)"+++\r\n", 3, 100);
    vTaskDelay(1000);
	for(int i=0;i<2;i++)
	{
		ESP_SendCmd_OK("AT+RST\r\n", 2000);
    vTaskDelay(pdMS_TO_TICKS(1000));
	}
	
    ESP_SendCmd_OK("AT+CWMODE=1\r\n", 2000);
    ESP_SendCmd("AT+CWJAP=\"aaa\",\"18208901719\"\r\n", "WIFI GOT IP", 5000);


    // 建立TCP连接
    ESP_SendCmd("AT+CIPSTART=\"TCP\",\"bemfa.com\",9501\r\n", "CONNECT", 5000);


		ESP_SendCmd_OK("AT+CIPMODE=1\r\n", 2000);
		ESP_SendCmd("AT+CIPSEND\r\n", ">", 5000);
    vTaskDelay(500);
    RingBuf_Clear();	

    // MQTT 连接报文
uint8_t connect_packet[] = {
        0x10, 0x2E,                // 固定报头
	
        0x00,0x06,                 // 协议名长度 = 6 		可变报头
        0x4D,0x51,0x49,0x73,0x64,0x70, // 协议名: MQIsdp (MQTT v3.1)
        0x03, 0x02, 0x00,0x78,     // 协议版本 + 连接标志 + 保活时间
	
        0x00,0x20,                 // 客户端ID长度 = 32     有效载荷
        // 32字节 客户端唯一ID
        '4','1','f','3','8','7','6','7',
        '2','0','a','d','4','a','e','8',
        '6','0','d','b','0','6','e','3',
        '5','6','3','1','a','8','5','5'
};
				// 串口发送整包
				HAL_UART_Transmit(&huart4, connect_packet, sizeof(connect_packet), 5000);
				vTaskDelay(300);   
  
		        // 解析CONNACK应答
    uint8_t connack[4];
    int ret = pal_tcp_recv_raw(0, connack, 4, 3000);
		    
    if (ret <1)
    {
			RingBuf_Clear();
			for(int i=0;i<30;i++)
			{
				  HAL_UART_Transmit(&huart4, connect_packet, sizeof(connect_packet), 5000);
					vTaskDelay(200);	
			}
	
        // 连接成功后 订阅主题
		uint8_t sub_packet[] = {
				0x82, 0x0A,          // 固定报头
			
				0x00, 0x01,          // 可变报头  报文标识符 Packet ID = 1
			
				0x00, 0x05,          // 有效载荷  主题名长度 = 5
				's','t','m','3','2', // 主题名：stm32
				0x01                 // 订阅QoS等级 = 1
		};
		HAL_UART_Transmit(&huart4, sub_packet, sizeof(sub_packet), 5000);
		vTaskDelay(300);

				g_mqtt_connected = 1;
				connect=1;
				Log_Write(LOG_INFO, "reconnect success");
				return ;
			}



		


}



// ==================== MQTT 心跳包 ====================
void MQTT_SendPing(void)
{
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

int pal_tcp_recv_raw(int sock, uint8_t *buf, int len, int timeout_ms)
{
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
        //HAL_Delay(1); // 避免CPU空转
				vTaskDelay(1);
    }
    return received;
}

// 在 mqtt.c 中新增
//void MQTT_SendTempHumi_NoWait(float temp, float humi,float light)
//{
//    char payload[128];
//    sprintf(payload, "{\"temperature\":%.1f,\"shidu\":%.1f,\"light\":%.0f}", temp, humi,light);

//    uint8_t buf[128];
//    int pos = 0;
//    buf[pos++] = 0x32;		//固定报头 报文类型

//		/*可变报头 主题长度：2 主题名称：5 报文标识符：2 有效载荷：*/
//    uint16_t topic_len = 5;
//    uint16_t msg_len = strlen(payload);
//    uint16_t rem = 2 + topic_len + 2 + msg_len;

//		/*变长编码计算剩余长度 小端排序（低位数据放在低地址） 低位在前  (一般低地址在前)*/
//    do {
//        uint8_t b = rem % 128;
//        rem /= 128;
//        if (rem > 0) b |= 0x80;
//        buf[pos++] = b;
//    } while (rem > 0);

//		/*可变报头  主题长度 0x00 0x05两个字节 高位在前*/
//    buf[pos++] = 0;
//    buf[pos++] = topic_len;
//    memcpy(&buf[pos], "stm32", 5);
//    pos += 5;

//		/*报文标识符 0x00 0x01*/
//    buf[pos++] = 0;
//    buf[pos++] = 1;

//		/*有效载荷*/
//    memcpy(&buf[pos], payload, msg_len);
//    pos += msg_len;

//    HAL_UART_Transmit(&huart4, buf, pos, 5000);
//    vTaskDelay(100);
//    // 不等待 PUBACK，直接返回
//}


