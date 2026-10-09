#ifndef WIFI_CONN_H
#define WIFI_CONN_H
/* ============================================================
 *  WiFi 连接层：ESP8266 入网（AT+CWJAP）、运行时凭据（UI 设置页可改，
 *  持久化 W25Q 0xF01000）、IP 查询（AT+CIFSR）、重连请求标志。
 *  与云平台（cloud.c）解耦：cloud 只管 MQTT，入网只管 WiFi。
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

void wifi_conn_load(void);                                        /* 上电装载存档（幂等） */
void wifi_conn_get(char *ssid, size_t n_ssid, char *pass, size_t n_pass);
int  wifi_conn_save(const char *ssid, const char *pass);          /* 0=已写入 W25Q 并生效 */

/* AT+CWJAP 入网：成功置 wifi_connected=1 并刷新 IP，失败置 0；返回 1=成功。
 * 调用时机：仍在 AT 指令模式（未进透传）。 */
int  wifi_join(void);

extern volatile uint8_t wifi_connected;       /* 已连上 AP */
extern char             g_esp_ip[16];         /* AT+CIFSR 结果，"--"=未获取 */
extern volatile uint8_t wifi_reconnect_request; /* UI 置 1：AT 任务用新凭据全量重连 */

#endif
