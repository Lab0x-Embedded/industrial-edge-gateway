#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "cmsis_os.h"
#include "dma.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"
#include "lcd.h"
#include "touch.h"
#include "stdio.h"
#include "string.h"
#include "lvgl.h"               
#include "lv_port_disp.h"       
#include "lv_port_indev.h"      
#include "page_main.h"
#include "modbus.h"
#include "esp_uart.h"
#include "ESP_at.h"
#include "cloud.h"
#include "wifi_conn.h"
#include "tasks.h"
#include "w25qxx.h"
#include "log.h"
#include "clock.h"
#include "cache.h"
#include <stdlib.h>


// 空闲熄屏相关
TickType_t idle_tick;
uint8_t lcd_bl_en = 1; //1亮屏 0熄屏
uint8_t power_on_lock = 1;
TickType_t power_tick;
lv_obj_t *main_scr;

 uint8_t fail_count = 0; // 记录连续失败次数
 uint8_t count;
QueueHandle_t     data_queue;
SemaphoreHandle_t esp_uart_mutex;

QueueHandle_t lvgl_data_queue;   
uint32_t last_ping = 0;

/* 最近一次收到云端有效应答的时刻（CONNACK / PUBACK / PINGRESP）。
 * 定义在此，刷新点在 cloud.c 和下面的 AT 任务；用途见 tasks.h 里
 * cloud_online_now() 的说明。32 位对齐读写，任务间无需加锁。 */
volatile uint32_t g_cloud_alive_tick = 0;


/* 在线显示的新鲜度窗口：心跳周期 10s，容忍连丢 2 次。
 * 与"心跳连败 3 次才置离线"同量级 —— 但界面不会再多等那 20 秒。 */
#define CLOUD_ALIVE_TIMEOUT_MS  25000u

uint8_t cloud_online_now(void)
{
    if (!g_mqtt_connected)
        return 0;
    return (uint32_t)(xTaskGetTickCount() - g_cloud_alive_tick) <
           (uint32_t)pdMS_TO_TICKS(CLOUD_ALIVE_TIMEOUT_MS);
}

extern IWDG_HandleTypeDef hiwdg;
 volatile uint8_t g_mqtt_connected;

// ==============================================
// Modbus 采集
// ==============================================
//void Modbus(void)
//{
//    Data_t data;				//结构体变量

//    while(1)
//    {
//        Modbus_Get_Temp(&data.temp, &data.shi);		//采集温湿度            
//				xQueueOverwrite(data_queue, &data);					// 永远只保留最新一次采集结果，旧值被直接覆盖 该队列长度为1
//        vTaskDelay(500);
//    }
//}

void Modbus(void)
{
    while(1)
    {
			
        Modbus_Poll_AllDev();					//读到的数据赋值给结构体数组
        
        Modbus_Upload_Json();					//将采集到的数据赋值给结构体 然后写入队列
				
        vTaskDelay(pdMS_TO_TICKS(800)); //0.8秒一轮轮询所有设备（时间短点测数据就更灵敏）
    }
}

/*补传D2亮 正常D0亮 离线存储D1亮*/
/*
D0:PE3 D1:PE4 D2:PG9

正常在线：先补传历史数据，再发送实时数据，通知 UI 更新。

断网期间：把数据存入 SPI Flash，等待恢复。

重连后：自动补传缓存数据（每次最多 5 条，分批补传）。

从数组里获取数据保证数据是最新的 
*/
void MQTT(void)
{
    vTaskDelay(5000);
    Data_t data_q;
    while (1)
    {
				 
		//	xSemaphoreTake(esp_uart_mutex, portMAX_DELAY);				//获取锁
			
			xQueueReceive(data_queue, &data_q,portMAX_DELAY);
			

            if (g_mqtt_connected)			//在线
            {
                char cached[512];
                uint16_t len;
                int count = 0;
xSemaphoreTake(esp_uart_mutex, portMAX_DELAY);				//获取锁
                // 补传（读完就销毁，绝对不重复）
							while (FlashCache_HasData() && count < 5)					//如果读指针落后写指针，说明有数据 返回1
							{
								if (FlashCache_Read(cached, &len) == 0)					/*从读指针位置读一条缓存数据*/
									{
											HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_SET);   //D0灭
											HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, GPIO_PIN_RESET); //D2亮
											if (MQTT_PublishJson(cached) != 0) break;   /* 未收到 PUBACK 不标记已发，下轮重试同一条 */
											FlashCache_MarkSent();				//只改魔数，不擦除、改魔数标记已补传
									        Log_Write(LOG_WARN,"BUCHUAN*1");
								count++;
								if (count > 4)
								{
									Log_Write(LOG_INFO,"BUCHUAN Finish");
								}
								vTaskDelay(800);
									}
									else
									{
										
											my_clear();		//补传五次数据完成后 清空缓存 让读地址等于写地址
											break;
									}
							}

							// 发实时数据 并且给ui写队列 （从数组里获取数据）
								MQTT_SendAllDev_NoWait();
               // MQTT_SendTempHumi_NoWait(data_q.temp, data_q.shi,data_q.light);
								HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_RESET);   //D0亮            
								HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_SET);	  //D1灭		
								xSemaphoreGive(esp_uart_mutex);					//释放锁							
//                xQueueOverwrite(lvgl_data_queue, &data_q);			// 写队列 永远只保留最新一次采集结果，旧值被直接覆盖 该队列长度为1
            }
            else	//断线中
            {
                // 离线缓存
                char json[128];
                cloud_build_payload(json, sizeof(json), &data_q);   /* payload 格式随 CLOUD_TARGET */
							HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_RESET);
if (data_q.valid != 0)   /* 无效数据不进断网缓存，避免重连后补传全 0 */
                {
                    /* ★ 必须检查返回值：这个函数会因三种原因失败，且都不报错——
                     *   -1 未初始化（FlashCache_Init 没跑，正常流程不会）
                     *   -2 JSON 超过条目容量（128-12 = 116 字节）
                     *   -3 回绕时还有未读数据挡着（宁可丢新数据，不覆盖未发的）
                     * 历史：IOTSystem 平台「温湿度+光照」三字段 payload 是 121 字节，
                     * 超过 116，写入失败但返回值被忽略 → 断网补传**全部静默丢失**。
                     * OneNET 是 103 字节，恰好能存，所以只在一个平台上暴露。 */
                    int cw = FlashCache_Write(json, strlen(json));
                    if (cw != 0)
                        Log_Write(LOG_WARN, (cw == -2) ? "cache: payload too big"
                                                       : "cache: write failed");
                }
            }
						
           
						
        /* 【必须留在循环体内】原来这里多了一个 '}'，把 while(1) 提前关掉了、
         * 这句 vTaskDelay 成了死代码 —— 后果不是"刷得快"，而是**锁车队饿死**：
         * MQTT 任务「放锁 →(几微秒)→ 重新取锁」，同优先级的 AT 任务在锁被放开
         * 那一刻被唤醒，但要等下一个 tick 才能跑，那时锁已经被抢回去 → 它重新阻塞，
         * 如此循环，AT 任务可以几分钟一次都跑不到（心跳/判离线/重连全部失效 = 僵尸板）。
         * 现场实测：UI 看门狗里 at step=4（停在取锁前）+ u1=MQTTTask。
         * 有这 2 秒窗口，AT 任务每次都能拿到锁。 */
        vTaskDelay(2000);
    }
}



/*
每10秒发心跳检测 然后读心跳应答 看是否正常
连续3次心跳应答错误或失败则判断离线
离线后 每10秒执行重连函数直到连接成功
*/
void AT(void)
{
	/* 开机首次连接也必须持锁：MQTT 任务这时可能已经在跑（例如上电后立刻要补传缓存），
	 * 它会拿同一把锁去读写 UART4 —— 不持锁就是两边抢同一份 AT 应答，
	 * 结果是"开机随机连不上，非要等下一轮 10s 重连才好"。 */
	xSemaphoreTake(esp_uart_mutex, portMAX_DELAY);
	MQTT_Init();						//连接成功后 g_mqtt_connected为1 否则为0
	xSemaphoreGive(esp_uart_mutex);

    uint8_t heart_fail = 0;  // 心跳连续失败次数
    static uint8_t sntp_tries = 0;   // SNTP 尝试次数（封顶用）
    for (;;)
    {
        /* 分段等待：WiFi 设置页的重连请求 / 扫描请求 / 校时请求可即时打断 10s 心跳间隔 */
        for (int i = 0; i < 10 && !wifi_reconnect_request && !wifi_scan_request
                       && !wifi_timesync_request; i++)
            vTaskDelay(1000);
        /* 【自愈】拿锁之前先确认 UART4 接收还挂着。
         * 这一步不碰串口、不需要 esp_uart_mutex，所以哪怕本任务马上就要在锁上
         * 等很久，自愈也已经生效了 —— 这正是原设计"只在 escape 失败时自检"
         * 的盲区：那条路径自己也要先拿到锁，AT 任务一被卡住就永远自愈不了。
         * 手册 #22：ORE 打停 RX DMA 后 HAL 不会自动重启，此后板子对模块彻底
         * "聋了"，现场表现极具欺骗性 —— 云平台看得到设备在线（PUBLISH 发得出去），
         * 本机却永远收不到 PUBACK/PINGRESP，屏幕一直 NO CLOUD。 */
        if (wifi_rx_selfcheck())
            dbg_print("[NET] uart4 rx rearmed\r\n");

        xSemaphoreTake(esp_uart_mutex, portMAX_DELAY);	//阻塞获取互斥锁

        /* WiFi 设置页的点名扫描：在持锁的 AT 语境里做（UI 线程只置标志，不阻塞界面）。
         * 只在"退不出透传"（-1）时重试一次 —— 这是最常见的瞬时失败，且这种失败
         * 本身很便宜（~6s）；而 CWLAP 无应答/报错要占满 12s 窗口，再重试一次就
         * 是 ~36s 独占串口，会把 MQTT 发布和心跳一起挤掉，不如等用户再点一次刷新。 */
        uint8_t serial_hogged = 0;   /* 本轮是否整段占用了串口（供后面"心跳豁免"用） */
        if (wifi_scan_request)
        {
            wifi_scan_request = 0;
            int ap = wifi_scan_locked();
            if (ap == -1)
            {
                dbg_print("[NET] scan retry\r\n");
                ap = wifi_scan_locked();
            }
            wifi_scan_state = (ap < 0) ? WIFI_SCAN_FAIL : WIFI_SCAN_DONE;
            serial_hogged = 1;
        }

        /* 设置页"同步当前时间"：手动立即校一次（已校过也允许——重校正是按钮的意义；
         * 模块当前在透传还是 AT 态，由 locked 函数自己收拾）。结果进日志页，
         * 校上后主屏时钟/日期立刻变亮变准，就是最直观的确认。 */
        if (wifi_timesync_request)
        {
            wifi_timesync_request = 0;
            if (wifi_time_sync_locked() == 0) {
                wifi_timesync_result = 1;       /* 设置页轮询它显示"校时成功/失败" */
                Log_Write(LOG_INFO, "time synced");
            } else {
                wifi_timesync_result = 2;
                Log_Write(LOG_WARN, "timesync fail");
            }
            serial_hogged = 1;
        }

        /* 扫描 / 校时全程独占串口（退透传 → AT 指令 → 恢复透传），这期间收不到
         * PINGRESP 是必然的，不该算作心跳失败，也不该扣掉在线新鲜度 ——
         * 否则用户连点几次刷新，就被自己的扫描"挤"成了离线 + 全量重连。
         * 豁免是安全的：这两个函数在恢复透传失败（TCP 真断了）时会自己把
         * g_mqtt_connected 置 0，走不到这里；能走到这里说明链路还是通的。 */
        if (serial_hogged && g_mqtt_connected)
        {
            heart_fail = 0;
            g_cloud_alive_tick = xTaskGetTickCount();
        }

        /* WiFi 设置页请求重连：置离线走 RE 全量重连（新凭据已由 wifi_conn_save 生效）。
         * 标志必须等重连尝试完整跑完才清——UI 靠它显示 Connecting 全程。 */
        uint8_t wifi_req = wifi_reconnect_request;
        if (wifi_req) {
            g_mqtt_connected = 0;             // 设置页保存了新凭据：强制全量重连
        }
				RingBuf_Clear();											//关键点 先清空缓冲区 否则有脏数据影响判断
        if (g_mqtt_connected)
        {
            /* SNTP 校时：封顶 5 次。
             * 为什么封顶——校时全程要独占 ESP 串口（退透传+配置+等对时+恢复，单次可到
             * 十几秒），若 AT 固件不支持 CIPSNTP 或 NTP 被墙，无限重试会把心跳和 MQTT
             * 发布全挤掉，表现出来像"连上了却发不出数据/经常掉线"。
             * （"模块还没对上 NTP"的等待已在 wifi_time_sync_locked 内部轮询吸收，不耗次数） */
            if (!Clock_IsSynced() && sntp_tries < 5)
            {
                sntp_tries++;
                if (wifi_time_sync_locked() != 0)
                    Log_Write(LOG_WARN, "sntp fail (clock stays dim)");
            }

            // 发送 PINGREQ
            MQTT_SendPing();

            /* 等 PINGRESP。
             *
             * ★ 不能用"裸读 2 字节再比 0xD0 0x00"：缓冲里可能先到一条平台下发的
             *   PUBLISH（QoS1 属性设置），裸读会把它当成 PINGRESP 判失败 →
             *   心跳误计数 → 三次后假离线 + 全量重连。
             *   现在按 MQTT 帧解析：读到的不是 PINGRESP 就先消费掉，
             *   继续等；期间收到任何合法报文都说明链路是活的。 */
            uint8_t frbuf[300];
            MqttFrame fr;
            int got_pong = 0;
            int link_alive = 0;
            uint32_t pong_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
            while ((int32_t)(pong_deadline - xTaskGetTickCount()) > 0)
            {
                uint32_t remain = (uint32_t)(pong_deadline - xTaskGetTickCount());
                int n = mqtt_read_frame(frbuf, sizeof(frbuf), &fr, remain);
                if (n <= 0) break;                    /* 超时 / 异常 */
                link_alive = 1;                        /* 收到任何完整报文都算链路活 */
                if (fr.type == MQTT_PKT_PINGRESP) { got_pong = 1; break; }
                /* 其它报文（PUBLISH/SUBACK 等）已在 mqtt_read_frame 里整段消费，
                 * 这里直接继续等 PINGRESP —— 不丢字节、不错位。 */
            }

            if (got_pong)
            {
                // 心跳正常
                heart_fail = 0;
                g_cloud_alive_tick = xTaskGetTickCount();   /* 收到 PINGRESP = 链路活着的硬证据 */
            }
            else if (link_alive)
            {
                /* 没等到 PINGRESP 但收到了别的报文：链路大概率是好的
                 * （可能 PINGRESP 还没排到 / 被平台回执挤后）。
                 * 只刷新新鲜度、不重置 heart_fail 也不累加 —— 避免一次
                 * 排队延迟就判离线。连续多次拿不到 PINGRESP 仍会累加。 */
                g_cloud_alive_tick = xTaskGetTickCount();
                heart_fail++;
                if (heart_fail >= 3)
                {
                    g_mqtt_connected = 0;
                    Log_Write(LOG_WARN, "MQTT disconnect");
                    HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_RESET);
                    HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, GPIO_PIN_SET);
                    heart_fail = 0;
                }
            }
            else							// 心跳超时或数据错误
            {
                heart_fail++;
                if (heart_fail >= 3)  // 连续 3 次失败，约 30 秒
                {
                    g_mqtt_connected = 0;					//断网
						Log_Write(LOG_WARN, "MQTT disconnect");
						HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_RESET);
						HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, GPIO_PIN_SET);
                    heart_fail = 0;
                }
            }
        }
				else
				{				//重连
                RE_MQTT_Init();
				}

        if (wifi_req)
            wifi_reconnect_request = 0;   // 尝试完整结束才清，UI 全程显示 Connecting
        xSemaphoreGive(esp_uart_mutex);					//释放锁
    }
}


// ==============================================
// UI 显示
// ==============================================
void UI(void)
{
    Data_t disp_data;

    LCD_Init();
    TP_Init();
    TP_Do_Calibration();   /* 开机两点校准：按提示依次按住 左上/右下 十字中心 */
    lv_init();
    lv_port_disp_init();
    lv_port_indev_init();

    main_scr = page_main_create();		//屏幕界面
    lv_scr_load(main_scr);				//加载界面

    Log_Write(LOG_INFO, "SYSTEM BOOT");  /* 开机标记：保证日志页至少有一条 */

    uint32_t start_tick = xTaskGetTickCount();
    
    uint8_t  last_status = 0xFF;      // 记录上一次的 MQTT 状态
    uint8_t  last_wifi   = 0xFF;      // 记录上一次的 WiFi 状态
    uint32_t last_uptime  = 0xFFFFFFFF; // 记录上一次的运行时间    魔法数
		

	while (1)				//任务函数 不会退出该函数 故局部变量不会被清空
    {
		
				// 触摸按下：重置空闲计时；黑屏中则点亮（唤醒的这次触摸不当作点击）
        if (HAL_GPIO_ReadPin(TP_PEN_GPIO_Port, TP_PEN_Pin) == GPIO_PIN_RESET)
        {
            idle_tick = xTaskGetTickCount();
            if (lcd_bl_en == 0)
            {
                HAL_GPIO_WritePin(LCD_BL_GPIO_Port, LCD_BL_Pin, GPIO_PIN_SET);
                lcd_bl_en = 1;
                lv_indev_wait_release(lv_indev_get_act());
                dbg_print("触摸点亮屏幕,lcd_bl_en = %d\n", lcd_bl_en);
            }
        }

		// 空闲10s无动作 → 息屏
        if(lcd_bl_en == 1 && (xTaskGetTickCount() - idle_tick >= pdMS_TO_TICKS(10000)))
        {
            HAL_GPIO_WritePin(LCD_BL_GPIO_Port, LCD_BL_Pin, GPIO_PIN_RESET);
            lcd_bl_en = 0;
            dbg_print("息屏,lcd_bl_en = %d\n", lcd_bl_en);
        }

        // 数据变化 则亮屏 + 重置计时   
        if (xQueueReceive(lvgl_data_queue, &disp_data, 0) == pdPASS)
        {
            page_main_update_data(disp_data.temp, disp_data.shi,disp_data.light);
        }

        /* WiFi/MQTT 状态变更也刷新亮屏。
         * 在线显示走 cloud_online_now()：不是"连接建立过"，而是"最近 25s 收到过
         * 云端应答"。原来直接看 g_mqtt_connected，链路已断、心跳还没连败 3 次的那
         * 二十几秒里屏幕会一直显示 ONLINE，而平台侧早判离线 —— 两边对不上，
         * 就是现场看到的"屏幕说在线、云平台说离线"。 */
        uint8_t mqtt_online = cloud_online_now();
        if (wifi_connected != last_wifi || mqtt_online != last_status)
        {
            last_wifi   = wifi_connected;
            last_status = mqtt_online;
						page_main_update_status(wifi_connected, mqtt_online);	//更新ui状态显示

            /* 只更新显示，不点亮屏幕：亮屏只认触摸（用户意图）。
             * cloud_online_now() 的 25s 新鲜度窗口在扫描/校时/重连独占串口、
             * 链路抖动时会来回翻转——原来"状态变化即亮屏"，无人操作也会
             * 周期性亮屏（现场："板子一会儿就亮"）。息屏期间的变化由下次
             * 点亮后的重绘自然呈现。 */
        }

				
        //更新运行时间 + 实时时间（同一个每秒节奏，省一次 tick 比较）
        uint32_t uptime = (xTaskGetTickCount() - start_tick) / configTICK_RATE_HZ;			//秒数
        if (uptime != last_uptime)
        {
            last_uptime = uptime;
            page_main_update_uptime(uptime);					//更新ui时间显示
            uint16_t yr; uint8_t mo, dd, hh, mm, ss;
            uint8_t synced = Clock_IsSynced();
            Clock_GetDateTime(&yr, &mo, &dd, &hh, &mm, &ss);
            page_main_update_clock(hh, mm, ss, synced);   //实时时间（未校时显示暗色）
            page_main_update_date(yr, mo, dd, synced);    //左上角日期（未校时显示 --）
        }

        lv_timer_handler();			//5毫秒刷新函数
        vTaskDelay(5);
    }
}


/*看门狗任务 3秒喂一次狗 溢出时间是20秒*/
void Dog_task()
{
	
	while(1)
	{
		HAL_IWDG_Refresh(&hiwdg);			//3秒喂一次狗 溢出时间是20秒     俗称喂狗：将倒计时计数器重置为初始值，阻止看门狗复位。
		vTaskDelay(3000);
	}
}
