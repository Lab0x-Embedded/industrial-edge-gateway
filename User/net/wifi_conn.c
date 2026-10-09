/* WiFi 连接层：凭据管理（W25Q 持久化）+ ESP8266 入网 + IP 查询
 *              + 热点扫描（异步，AT 任务执行）+ SNTP 校时 */
#include "wifi_conn.h"
#include "w25qxx.h"
#include "esp_at.h"
#include "esp_uart.h"
#include "usart.h"
#include "cloud.h"      /* dbg_print */
#include "log.h"        /* Log_Write */
#include "clock.h"      /* Clock_SetTime：SNTP 校时 */
#include "stm32f4xx.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <string.h>
#include <stdio.h>

/* —— 凭据存档：W25Q 参数扇区（0xF00000 是触摸校准，相邻互不影响）—— */
#define WIFI_CFG_ADDR   0x00F01000UL
#define WIFI_CFG_MAGIC  0x57494643UL   /* "WIFC" */

typedef struct {
    uint32_t magic;
    char     ssid[33];
    char     pass[65];
    uint32_t sum;       /* 简单累加校验 */
} WifiCfg_t;

static char wifi_ssid[33] = WIFI_SSID;
static char wifi_pass[65] = WIFI_PASS;
static uint8_t creds_loaded = 0;

volatile uint8_t wifi_connected = 0;
char g_esp_ip[16] = "--";
volatile uint8_t wifi_reconnect_request = 0;

/* 异步扫描的共享状态（UI 只写 request，AT 任务写结果与 state） */
volatile uint8_t wifi_scan_request = 0;
volatile uint8_t wifi_scan_state = WIFI_SCAN_IDLE;
volatile uint8_t wifi_ap_count = 0;
char             wifi_ap_list[WIFI_AP_MAX][33];

void wifi_conn_load(void)
{
    WifiCfg_t c;
    if (creds_loaded) return;
    creds_loaded = 1;
    SPI_FLASH_BufferRead((uint8_t *)&c, WIFI_CFG_ADDR, sizeof c);
    uint32_t sum = c.magic;
    for (size_t i = 0; i < sizeof c.ssid + sizeof c.pass; i++)
        sum += ((uint8_t *)c.ssid)[i];
    if (c.magic == WIFI_CFG_MAGIC && c.sum == sum && c.ssid[0]) {
        memcpy(wifi_ssid, c.ssid, sizeof wifi_ssid);
        memcpy(wifi_pass, c.pass, sizeof wifi_pass);
    }
}

void wifi_conn_get(char *ssid, size_t n_ssid, char *pass, size_t n_pass)
{
    wifi_conn_load();
    strncpy(ssid, wifi_ssid, n_ssid - 1); ssid[n_ssid - 1] = 0;
    strncpy(pass, wifi_pass, n_pass - 1); pass[n_pass - 1] = 0;
}

int wifi_conn_save(const char *ssid, const char *pass)
{
    size_t sl = strlen(ssid), pl = strlen(pass);
    if (sl == 0 || sl > 32 || pl > 64) return -1;

    WifiCfg_t c;
    memset(&c, 0, sizeof c);
    c.magic = WIFI_CFG_MAGIC;
    memcpy(c.ssid, ssid, sl);  c.ssid[sl] = 0;
    memcpy(c.pass, pass, pl);  c.pass[pl] = 0;
    c.sum = c.magic;
    for (size_t i = 0; i < sizeof c.ssid + sizeof c.pass; i++)
        c.sum += ((uint8_t *)c.ssid)[i];

    SPI_FLASH_SectorErase(WIFI_CFG_ADDR);
    SPI_FLASH_BufferWrite((uint8_t *)&c, WIFI_CFG_ADDR, sizeof c);

    memcpy(wifi_ssid, c.ssid, sizeof wifi_ssid);
    memcpy(wifi_pass, c.pass, sizeof wifi_pass);
    creds_loaded = 1;
    return 0;
}

/* 入网成功后查 IP（此时在 AT 模式，应答走 UART4 环形缓冲） */
static void wifi_query_ip(void)
{
    char line[96];
    int n = 0;
    ESP_SendCmd("AT+CIFSR\r\n", "STAIP", 2000);
    uint32_t t0 = HAL_GetTick();
    while ((HAL_GetTick() - t0) < 600 && n < (int)sizeof(line) - 1) {
        uint8_t ch;
        if (RingBuf_Read(&ch)) line[n++] = (char)ch;
        else                   vTaskDelay(2);
    }
    line[n] = 0;
    char *p = strstr(line, "STAIP,\"");
    if (p) {
        p += 7;
        char *q = strchr(p, '"');
        if (q && (size_t)(q - p) < sizeof g_esp_ip) {
            memcpy(g_esp_ip, p, (size_t)(q - p));
            g_esp_ip[q - p] = 0;
        }
    }
}

int wifi_join(void)
{
    char cmd[160];
    wifi_conn_load();

    snprintf(cmd, sizeof cmd, "AT+CWJAP=\"%s\",\"%s\"\r\n", wifi_ssid, wifi_pass);
    if (ESP_SendCmd(cmd, "WIFI GOT IP", 5000) == ESP_OK) {
        wifi_connected = 1;
        dbg_print("[NET] wifi OK\r\n");
        wifi_query_ip();
        return 1;
    }
    wifi_connected = 0;
    dbg_print("[NET] wifi join FAILED\r\n");
    return 0;
}

/* ============================================================
 *  AT 透传进出：扫描 / 校时都要临时退出透传去发 AT 指令
 *  （TCP 连接不断，MQTT 会话无感；离线时无害，重连流程会重建）
 *  调用前提：已持有 uart1_mutex
 *
 *  at_exit_passthrough 返回 0=确认已在 AT 指令模式；-1=没应答。
 *  为什么要重试 + 静默期：ESP 在透传模式下要求 "+++" 前后各有 20ms 静默，
 *  当时若正好有下行数据，"+++" 会被当成 TCP 数据吞掉 → 退不出透传 →
 *  后面发的 CWLAP 也被当数据 → 用户看到"扫描失败"。实测这是最常见的失败原因，
 *  所以给两次机会，并在失败时把原因写进日志页（LOG_WARN）。
 * ============================================================ */
static int at_exit_passthrough(void)
{
    for (int attempt = 0; attempt < 2; attempt++)
    {
        RingBuf_Clear();
        vTaskDelay(50);                          /* 先让线路静一下再发 +++ */
        HAL_UART_Transmit(&huart4, (uint8_t *)"+++", 3, 100);
        vTaskDelay(1000);                        /* 退透传保护时间，ESP 规范要求 */
        if (ESP_SendCmd_OK("AT\r\n", 1500) == ESP_OK)
        {
            RingBuf_Clear();
            return 0;
        }
        dbg_print("[NET] at escape retry\r\n");
    }
    return -1;
}

static void at_enter_passthrough(void)
{
    ESP_SendCmd_OK("AT+CIPMODE=1\r\n", 2000);
    ESP_SendCmd("AT+CIPSEND\r\n", ">", 3000);
    RingBuf_Clear();
}

/* ============================================================
 *  热点扫描：退透传 → AT+CWLAP（逐字节收）→ 解析去重 → 恢复透传
 *  返回：>=0 = 扫到的热点数（0 = 正常应答但没有热点）
 *        -1 = 退不出透传（模块没应答）
 *        -2 = CWLAP 一个字节都没回
 *        -3 = CWLAP 回了 ERROR
 *  三条失败路径都写日志页（LOG_WARN），现场不用接串口也能看到原因。
 * ============================================================ */
int wifi_scan_locked(void)
{
    static char buf[1024];          /* CWLAP 多热点应答可达 1KB+，不能放任务栈 */
    int n = 0, cnt = 0;

    wifi_ap_count = 0;

    if (at_exit_passthrough() != 0)
    {
        Log_Write(LOG_WARN, "scan FAIL: no AT mode");
        return -1;                  /* 没确认进入 AT 模式：此时多半还在透传，
                                     * 不能再发 CIPMODE/CIPSEND 去"恢复"（会污染 TCP 流） */
    }

    /* 必须手动逐字节收：ESP_SendCmd 是"匹配到就丢弃"的流式实现，
     * 用它会把 +CWLAP 行当匹配数据吃掉，啥也剩不下。 */
    HAL_UART_Transmit(&huart4, (uint8_t *)"AT+CWLAP\r\n", 10, 100);
    uint32_t t0 = HAL_GetTick();
    while (HAL_GetTick() - t0 < 8000 && n < (int)sizeof(buf) - 1) {
        uint8_t ch;
        if (RingBuf_Read(&ch)) {
            buf[n++] = (char)ch;
            buf[n] = 0;
            if (strstr(buf, "\r\nOK\r\n") || strstr(buf, "ERROR"))
                break;
        } else {
            vTaskDelay(5);
        }
    }
    buf[n] = 0;

    at_enter_passthrough();

    if (n == 0)
    {
        Log_Write(LOG_WARN, "scan FAIL: no reply");
        return -2;
    }
    if (strstr(buf, "ERROR") != NULL)
    {
        Log_Write(LOG_WARN, "scan FAIL: CWLAP error");
        return -3;
    }

    /* 解析 +CWLAP:(ecn,"ssid",rssi,...)，同名去重 */
    char *p = buf;
    while (cnt < WIFI_AP_MAX && (p = strstr(p, "+CWLAP:(")) != NULL) {
        char *q = strchr(p + 8, '"');
        if (!q) break;
        char *e = strchr(q + 1, '"');
        if (!e) break;
        size_t len = (size_t)(e - q - 1);
        if (len > 0 && len < 33) {
            char ssid[33];
            memcpy(ssid, q + 1, len);
            ssid[len] = 0;
            int dup = 0;
            for (int i = 0; i < cnt; i++)
                if (strcmp(wifi_ap_list[i], ssid) == 0) { dup = 1; break; }
            if (!dup) {
                memcpy(wifi_ap_list[cnt], ssid, len + 1);
                cnt++;
            }
        }
        p = e;
    }

    if (cnt == 0)
        Log_Write(LOG_INFO, "scan: 0 AP");     /* 应答正常但没有热点，不算失败 */

    wifi_ap_count = (uint8_t)cnt;
    dbg_print("[NET] scan %d AP\r\n", cnt);
    return cnt;
}

/* ============================================================
 *  SNTP 校时：退透传 → 配置服务器 → 取一次时间 → 恢复透传
 *  0 = 已校时；-1 = 无应答；-2 = 应答格式不对（AT 固件可能太老）
 *  ESP8266 AT 固件 ≥1.0 支持 CIPSNTP*。
 * ============================================================ */
#define SNTP_TIMEZONE   8              /* 东八区 */
#define SNTP_SERVER     "cn.pool.ntp.org"

int wifi_time_sync_locked(void)
{
    static char buf[192];
    int n = 0;
    char cmd[96];

    /* 退不出透传就别发校时指令：会被当成 TCP 数据吃掉，还可能污染 MQTT 流 */
    if (at_exit_passthrough() != 0)
        return -1;

    snprintf(cmd, sizeof cmd, "AT+CIPSNTPCFG=1,%d,\"%s\"\r\n", SNTP_TIMEZONE, SNTP_SERVER);
    ESP_SendCmd_OK(cmd, 2000);

    RingBuf_Clear();
    HAL_UART_Transmit(&huart4, (uint8_t *)"AT+CIPSNTPTIME?\r\n", 18, 100);
    uint32_t t0 = HAL_GetTick();
    while (HAL_GetTick() - t0 < 3000 && n < (int)sizeof(buf) - 1) {
        uint8_t ch;
        if (RingBuf_Read(&ch)) buf[n++] = (char)ch;
        else                   vTaskDelay(5);
    }
    buf[n] = 0;

    at_enter_passthrough();

    /* 应答形如： +CIPSNTPTIME:Fri Oct  9 12:34:56 2026
     * "+CIPSNTPTIME:" 之后第一个冒号就是"时:分:秒"的分隔符（星期/月份里没冒号） */
    char *p = strstr(buf, "+CIPSNTPTIME:");
    if (!p) return (n == 0) ? -1 : -2;
    char *t = strchr(p + 13, ':');
    if (!t || (t - 2) < (p + 13)) return -2;

    int hh = 0, mm = 0, ss = 0;
    if (sscanf(t - 2, "%2d:%2d:%2d", &hh, &mm, &ss) != 3) return -2;
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 59) return -2;

    Clock_SetTime((uint8_t)hh, (uint8_t)mm, (uint8_t)ss);
    dbg_print("[NET] sntp %02d:%02d:%02d\r\n", hh, mm, ss);
    return 0;
}
