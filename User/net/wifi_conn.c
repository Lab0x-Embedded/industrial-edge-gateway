/* WiFi 连接层：凭据管理（W25Q 持久化）+ ESP8266 入网 + IP 查询 */
#include "wifi_conn.h"
#include "w25qxx.h"
#include "esp_at.h"
#include "esp_uart.h"
#include "usart.h"
#include "cloud.h"      /* dbg_print */
#include "log.h"        /* Log_Write */
#include "stm32f4xx.h"
#include "FreeRTOS.h"
#include "task.h"
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
