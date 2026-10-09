#ifndef WIFI_CONN_H
#define WIFI_CONN_H
/* ============================================================
 *  WiFi 连接层：ESP8266 入网（AT+CWJAP）、运行时凭据（UI 设置页可改，
 *  持久化 W25Q 0xF01000）、IP 查询（AT+CIFSR）、重连请求标志、
 *  热点扫描（异步）与 SNTP 校时。与云平台（cloud.c）解耦：
 *  cloud 只管 MQTT，入网只管 WiFi。
 *
 *  ⚠ 并发约定：ESP 串口由 uart1_mutex 独占，且**不可重入**。
 *    - UI 线程只能置"请求标志"，不直接碰串口；
 *    - AT 任务在持锁上下文里调用 *_locked() 版本去真正操作串口。
 *    任何 *_locked() 函数都不得自己再去 xSemaphoreTake，否则死锁。
 * ============================================================ */
#include <stdint.h>
#include <stddef.h>

/* 编译期默认凭据（W25Q 无存档时的兜底；设置页保存后以存档为准） */
#ifndef WIFI_SSID
#define WIFI_SSID  "Magic7"
#endif
#ifndef WIFI_PASS
#define WIFI_PASS  "121qweqwe"
#endif

#define WIFI_AP_MAX   12          /* 一次扫描最多保留的热点数 */

/* 异步扫描状态机：UI 置 RUNNING+request → AT 任务扫完置 DONE/FAIL */
#define WIFI_SCAN_IDLE     0
#define WIFI_SCAN_RUNNING  1
#define WIFI_SCAN_DONE     2
#define WIFI_SCAN_FAIL     3

extern volatile uint8_t wifi_scan_request;   /* UI 置 1：请求一次扫描 */
extern volatile uint8_t wifi_scan_state;     /* 上面四态，UI 轮询 */
extern volatile uint8_t wifi_ap_count;       /* 扫描结果条数 */
extern char             wifi_ap_list[WIFI_AP_MAX][33];
extern int8_t           wifi_ap_rssi[WIFI_AP_MAX];  /* 与 wifi_ap_list 一一对应，dBm（-127=未知） */

void wifi_conn_load(void);                                        /* 上电装载存档（幂等） */
void wifi_conn_get(char *ssid, size_t n_ssid, char *pass, size_t n_pass);
int  wifi_conn_save(const char *ssid, const char *pass);          /* 0=已写入 W25Q 并生效 */

/* AT+CWJAP 入网：成功置 wifi_connected=1 并刷新 IP，失败置 0；返回 1=成功。
 * 调用时机：仍在 AT 指令模式（未进透传）。 */
int  wifi_join(void);

/* —— 只能由 AT 任务在持 uart1_mutex 的上下文调用 ——
 * wifi_scan_locked：退透传 → AT+CWLAP → 解析去重填 wifi_ap_list →（原在线则）恢复透传；
 *                   返回热点数，负数见上面的失败码。
 * wifi_time_sync_locked：退透传 → CIPSNTPCFG + CIPSNTPTIME? → 解析并 Clock_SetTime；
 *                   0=已校时，-1/-2=失败（AT 固件不支持 / 应答异常）。
 * wifi_at_escape：退出透传并**确认**进入 AT 指令模式（静默期 + 重试）；
 *                   0=已在 AT 模式，-1=没应答（多半还在透传，此时别再发 AT 指令，
 *                   否则会被当成 TCP 数据灌进连接里）。cloud.c 的连接流程也用它。
 * wifi_at_passthrough_enter：CIPMODE=1 + CIPSEND 恢复透传 —— 只有确认 TCP 在线时才调；
 *                   0=拿到 ">"（透传真的起来了），-1=没拿到（TCP 多半已断，
 *                   调用方应把 g_mqtt_connected 置 0 走重连，别再往串口灌 MQTT 字节）。 */
int  wifi_scan_locked(void);
int  wifi_time_sync_locked(void);
int  wifi_at_escape(void);
int  wifi_at_passthrough_enter(void);

extern volatile uint8_t wifi_connected;       /* 已连上 AP */
extern char             g_esp_ip[16];         /* AT+CIFSR 结果，"--"=未获取 */
extern volatile uint8_t wifi_reconnect_request; /* UI 置 1：AT 任务用新凭据全量重连 */

#endif
