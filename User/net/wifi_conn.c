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
#include "tasks.h"      /* g_mqtt_connected：扫描前后判断是否在线 */
#include "stm32f4xx.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>     /* atoi：解析 CWLAP 应答里的 rssi */

/* UART4(ESP8266) 的 DMA 接收环境：自检/自愈时要重新挂载接收 */
extern uint8_t a[256];
extern DMA_HandleTypeDef hdma_uart4_rx;

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
int8_t           wifi_ap_rssi[WIFI_AP_MAX];
volatile int8_t  wifi_scan_code = 0;
char             wifi_scan_raw[64] = {0};
volatile uint8_t wifi_rx_armed = 1;    /* 失败自检：当时 UART4 RX DMA 还挂着吗 */
volatile uint8_t wifi_tx_err   = 0;    /* 失败自检：发送是否报错（HAL_UART_Transmit 返回值） */

void wifi_conn_load(void)
{
    WifiCfg_t c;
    if (creds_loaded) return;
    creds_loaded = 1;
    /* 必须持 flash_mutex：SPI 事务（CS低→命令→数据→CS高）被别的任务从中间
     * 打断就会双方报废（日志/缓存/触摸校准都在用同一颗 W25Q，见手册 #18）。
     * 这里漏了锁，后果很隐蔽：读回的是脏数据 → magic/校验不过 → 静默回落到
     * 编译期默认凭据 → "WiFi 怎么都连不上"，因为设备一直在试一个不存在的 AP。 */
    if (flash_mutex) xSemaphoreTake(flash_mutex, portMAX_DELAY);
    SPI_FLASH_BufferRead((uint8_t *)&c, WIFI_CFG_ADDR, sizeof c);
    if (flash_mutex) xSemaphoreGive(flash_mutex);
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
    char msg[80];
    if (sl == 0 || sl > 32 || pl > 64) return -1;

    WifiCfg_t c, back;
    memset(&c, 0, sizeof c);
    c.magic = WIFI_CFG_MAGIC;
    memcpy(c.ssid, ssid, sl);  c.ssid[sl] = 0;
    memcpy(c.pass, pass, pl);  c.pass[pl] = 0;
    c.sum = c.magic;
    for (size_t i = 0; i < sizeof c.ssid + sizeof c.pass; i++)
        c.sum += ((uint8_t *)c.ssid)[i];

    /* 整段擦写都持 flash_mutex（与日志/缓存/触摸校准共用一颗 W25Q，手册 #18） */
    if (flash_mutex) xSemaphoreTake(flash_mutex, portMAX_DELAY);
    SPI_FLASH_SectorErase(WIFI_CFG_ADDR);
    SPI_FLASH_BufferWrite((uint8_t *)&c, WIFI_CFG_ADDR, sizeof c);
    SPI_FLASH_BufferRead((uint8_t *)&back, WIFI_CFG_ADDR, sizeof back);   /* 顺手回读 */
    if (flash_mutex) xSemaphoreGive(flash_mutex);

    /* 写完立刻校验：W25Q 的写失败是**静默**的（手册 #15：忙等超时 + 超时路径
     * 不释放 CS → 后续写全被吞）。不回读就会出现"界面提示保存成功、设备其实
     * 还在用编译期默认凭据"这种鬼故事 —— 现象就是怎么都连不上。 */
    if (back.magic != WIFI_CFG_MAGIC || back.ssid[0] == 0 ||
        back.sum != c.sum || strcmp(back.ssid, c.ssid) != 0 ||
        strcmp(back.pass, c.pass) != 0)
    {
        Log_Write(LOG_ERROR, "wifi save verify FAIL");
        return -2;
    }

    memcpy(wifi_ssid, c.ssid, sizeof wifi_ssid);
    memcpy(wifi_pass, c.pass, sizeof wifi_pass);
    creds_loaded = 1;
    snprintf(msg, sizeof msg, "wifi saved: %s", c.ssid);
    Log_Write(LOG_INFO, msg);
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
    char msg[80];
    ESP_Status_t r;
    wifi_conn_load();

    snprintf(cmd, sizeof cmd, "AT+CWJAP=\"%s\",\"%s\"\r\n", wifi_ssid, wifi_pass);
    /* 超时 5s → 15s：CWJAP 要扫描+关联+DHCP，正常 3~8s，信号弱/路由器 DHCP 慢
     * 时更久。5s 会在正常慢的情况下误判失败，而失败之后紧接着的 CIPSTART
     * 必然连不上 —— 表现就是"偶尔/经常连不上"，且日志只有一句 join failed。 */
    r = ESP_SendCmd(cmd, "WIFI GOT IP", 15000);
    if (r == ESP_OK) {
        wifi_connected = 1;
        dbg_print("[NET] wifi OK\r\n");
        wifi_query_ip();
        return 1;
    }
    wifi_connected = 0;
    /* 区分两种失败，现场能直接看日志页判断是"密码/SSID 错"还是"信号/超时" */
    snprintf(msg, sizeof msg, r == ESP_ERROR ? "wifi denied: %s" : "wifi timeout: %s",
             wifi_ssid);
    Log_Write(LOG_WARN, msg);
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
 *  后面发的 CWLAP 也被当数据 → 用户看到"扫描失败"。这是最常见的失败原因，
 *  所以：先等出真正的静默窗口（wait_line_idle）再发，给三次机会、一次比一次
 *  等更久，失败时把原因写进日志页（LOG_WARN）。
 * ============================================================ */
/* 等串口静默 ≥quiet_ms：靠"环形缓冲里未读字节数连续 quiet_ms 不变"来判断。
 * 最多等 4 倍时间就放弃（不能为了等静默把 AT 任务卡死）。 */
static void wait_line_idle(uint32_t quiet_ms)
{
    uint16_t last = Command_GetLength();
    uint32_t t0   = HAL_GetTick();

    while (HAL_GetTick() - t0 < quiet_ms * 4u)
    {
        vTaskDelay(5);
        uint16_t now = Command_GetLength();
        if (now != last) { last = now; t0 = HAL_GetTick(); continue; }  /* 还有数据，重新计时 */
        if (HAL_GetTick() - t0 >= quiet_ms) return;                     /* 安静够了 */
    }
}

/* 自检 + 自愈：UART4 的 RX DMA 还挂着吗？
 * 为什么必须有这个函数：一旦 UART4 出现溢出(ORE)/帧错误/噪声，STM32 HAL 会**中止**
 * 正在进行的 DMA 接收，而且不会自动重启（HAL_UART_ErrorCallback 是弱定义空函数，
 * 本项目一直没实现）。DMA 一停，RxEventCallback 就再也不会被触发，接收永久停摆 ——
 * 模块在正常说话，板子却收不到一个字节：AT 全部超时、WiFi 连不上、扫描永远失败，
 * 只能复位恢复。这正好解释了"时好时坏、后来一直坏"。
 * 这里返回 1=还挂着；0=刚掉了（顺手重新挂上 + 记日志）。 */
static uint8_t rx_selfcheck(void)
{
    uint8_t armed = (huart4.RxState == HAL_UART_STATE_BUSY_RX) ? 1 : 0;
    if (!armed)
    {
        __HAL_UART_CLEAR_OREFLAG(&huart4);      /* F4 上这一句同时清 PE/FE/NE/ORE */
        huart4.ErrorCode = HAL_UART_ERROR_NONE;
        if (HAL_UARTEx_ReceiveToIdle_DMA(&huart4, a, sizeof(a)) != HAL_OK)
            uart4_rx_rst_fail++;
        __HAL_DMA_DISABLE_IT(&hdma_uart4_rx, DMA_IT_HT);
        Log_Write(LOG_WARN, "uart4 rx DEAD -> rearm");
    }
    return armed;
}

static void probe_capture(uint32_t ms);   /* 前向声明：退透传失败时要用它抓原始字节 */

static int at_exit_passthrough_impl(void)
{
    /* 三次机会，且一次比一次给更长的保护时间。
     * ESP 只认"+++ 前后各 20ms 都安静"的那种 +++；透传模式下随时可能有下行数据
     * 把 +++ 吞掉，所以先用 wait_line_idle() 等出一个真正的静默窗口再发，
     * 而不是盲等 50ms。 */
    for (int attempt = 0; attempt < 3; attempt++)
    {
        RingBuf_Clear();
        wait_line_idle(60);                      /* 等线路安静下来（最多等 240ms） */
        RingBuf_Clear();
        if (HAL_UART_Transmit(&huart4, (uint8_t *)"+++", 3, 100) != HAL_OK)
            wifi_tx_err = 1;                     /* 发送都失败 → 自检行会写 TX err */
        vTaskDelay(1000 + attempt * 400);        /* 退透传保护时间：1.0 / 1.4 / 1.8s */
        if (ESP_SendCmd_OK("AT\r\n", 2000) == ESP_OK)
        {
            RingBuf_Clear();
            return 0;
        }
        dbg_print("[NET] at escape retry\r\n");
    }

    /* 三次都没应答：先自检 RX 是否掉线（掉了就重新挂上），再抓一段原始字节 —— 
     * 屏幕上就能看到"板子还能不能听见模块 / 模块到底在说什么" */
    wifi_rx_armed = rx_selfcheck();
    probe_capture(400);
    return -1;
}

int wifi_at_escape(void)
{
    return at_exit_passthrough_impl();
}

/* 恢复透传。返回 0=拿到 ">" 提示、透传真的起来了；-1=没拿到（多半是 TCP 已经
 * 断了 —— 此时模块停在"CIPMODE=1 但没在发送"的中间态，后续裸发的 MQTT 字节
 * 会被模块当 AT 指令吃成 ERROR）。调用方拿到 -1 应该把 g_mqtt_connected 置 0，
 * 让重连流程（RE_MQTT_Init 会 AT+RST）去重建，而不是继续往串口灌数据。 */
int wifi_at_passthrough_enter(void)
{
    if (ESP_SendCmd_OK("AT+CIPMODE=1\r\n", 2000) != ESP_OK)
        return -1;
    if (ESP_SendCmd("AT+CIPSEND\r\n", ">", 3000) != ESP_OK)
        return -1;
    RingBuf_Clear();
    return 0;
}

/* 把模块原始应答的可打印部分同时落到两处：
 *  - wifi_scan_raw[]：给屏幕自检显示（WiFi 列表弹窗里会显示 RAW 行）
 *  - 日志页：Log_Write 一条
 * 目的：扫描失败时不接串口也能看到模块到底回了什么（busy p... / ERROR / 乱码）。 */
static void log_scan_snippet(const char *b, int n)
{
    int k = 0;
    for (int i = 0; i < n && k < (int)sizeof(wifi_scan_raw) - 1; i++) {
        char c = b[i];
        if (c == '\r' || c == '\n') c = ' ';
        wifi_scan_raw[k++] = (c >= 0x20 && c < 0x7F) ? c : '.';
    }
    wifi_scan_raw[k] = 0;

    char m[80];
    snprintf(m, sizeof(m), "resp:%s", wifi_scan_raw);
    Log_Write(LOG_WARN, m);
}

/* 探测抓包：静等 ms 毫秒，把模块这段时间吐出来的原始字节记进 wifi_scan_raw。
 * 这是"模块到底在说什么"的关键证据（也是本项目一贯的自检风格：把原始字节显示出来）：
 *   空          → 模块根本没在跟我们说话（没模块/没上电/波特率不对/AT 固件没跑）
 *   busy p...   → 模块在忙（上一次操作没收尾）
 *   二进制乱码   → 波特率不对
 *   +IPD/二进制  → 模块还在透传（+++ 没生效） */
static void probe_capture(uint32_t ms)
{
    int k = 0;
    uint32_t t0 = HAL_GetTick();
    while (HAL_GetTick() - t0 < ms * 3u && k < (int)sizeof(wifi_scan_raw) - 1)
    {
        uint8_t ch;
        if (RingBuf_Read(&ch))
        {
            char c = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '.';
            if (c != ' ' || k > 0)                       /* 前导换行别占位置 */
                wifi_scan_raw[k++] = c;
            t0 = HAL_GetTick();                          /* 有字节就继续等 */
        }
        else
            vTaskDelay(5);
    }
    wifi_scan_raw[k] = 0;
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
    uint8_t was_online = g_mqtt_connected;   /* 扫描前是否在线：决定要不要恢复透传 */

    wifi_ap_count = 0;
    wifi_tx_err   = 0;

    if (wifi_at_escape() != 0)
    {
        Log_Write(LOG_WARN, "scan FAIL: no AT mode");
        wifi_scan_code = -1;
        return -1;                  /* 没确认进入 AT 模式：此时多半还在透传，
                                     * 不能再发 CIPMODE/CIPSEND 去"恢复"（会污染 TCP 流） */
    }

    /* 顺手把模块拉回 station 模式：CWLAP 只在 station / station+AP 下可用，
     * 模块若停在 SoftAP（CWMODE=2，出厂默认就是 2 的固件）会直接对 CWLAP 回
     * ERROR —— 现象就是"扫描永远失败"。幂等且便宜，一次 2s 上限。 */
    ESP_SendCmd_OK("AT+CWMODE=1\r\n", 2000);

    /* 必须手动逐字节收：ESP_SendCmd 是"匹配到就丢弃"的流式实现，
     * 用它会把 +CWLAP 行当匹配数据吃掉，啥也剩不下。 */
    if (HAL_UART_Transmit(&huart4, (uint8_t *)"AT+CWLAP\r\n", 10, 100) != HAL_OK)
        wifi_tx_err = 1;
    uint32_t t0 = HAL_GetTick();
    /* 12s（原 8s）：密环境下 CWLAP 要扫全部信道，慢的时候 8s 收不完会只剩半份
     * 列表（看着像"少了好几个热点"）；正常情况收到 \r\nOK\r\n 就提前退出了。 */
    while (HAL_GetTick() - t0 < 12000 && n < (int)sizeof(buf) - 1) {
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

    /* 只有"扫描前在线"才恢复透传。离线时模块本来就该留在 AT 模式，
     * 这里若发 CIPMODE=1 + CIPSEND，模块进了透传却没有 TCP 连接，
     * 之后 cloud.c 的 AT+CWJAP / AT+CIPSTART 会被当成 TCP 数据发出去，
     * 模块再也收不到 AT 指令 —— 表现就是"怎么都连不上"（只能靠 AT+RST 救）。 */
    if (was_online)
    {
        /* 恢复失败 = TCP 其实已经断了：立刻置离线，让重连流程（AT+RST）重建，
         * 否则模块停在"CIPMODE=1 但没在发送"的中间态，心跳裸发的字节会被
         * 模块当 AT 指令吃成 ERROR，白折腾三轮心跳（~30s）才发现。 */
        if (wifi_at_passthrough_enter() != 0)
        {
            g_mqtt_connected = 0;
            Log_Write(LOG_WARN, "scan: link lost");
        }
    }
    else
        RingBuf_Clear();

    if (n == 0)
    {
        /* 一个字节没回：模块要么没收到 CWLAP，要么根本没在 AT 模式应答 */
        wifi_scan_raw[0] = 0;
        Log_Write(LOG_WARN, "scan FAIL: no reply");
        wifi_scan_code = -2;
        return -2;
    }
    if (strstr(buf, "ERROR") != NULL)
    {
        /* 模块明确回 ERROR（如 "busy p..."、CWMODE 不对）。
         * 把原始应答的可打印部分落到日志页 —— 不接串口也能看到模块到底说了什么。 */
        log_scan_snippet(buf, n);
        wifi_scan_code = -3;
        return -3;
    }

    /* 解析 +CWLAP:(ecn,"ssid",rssi,mac,ch,freq)，同名去重 + 取强的那条 */
    char *p = buf;
    while (cnt < WIFI_AP_MAX && (p = strstr(p, "+CWLAP:(")) != NULL) {
        char *q = strchr(p + 8, '"');
        if (!q) break;
        char *e = strchr(q + 1, '"');
        if (!e) break;

        /* 闭引号后面紧跟 ",rssi" —— 顺手取信号强度：面板里要按强度排序、
         * 显示 dBm。解析不到（脏数据/固件格式不同）记 -127，排到最后。 */
        int rssi = -127;
        if (*(e + 1) == ',')
            rssi = atoi(e + 2);
        if (rssi > 0 || rssi < -127)
            rssi = -127;

        size_t len = (size_t)(e - q - 1);
        if (len > 0 && len < 33) {
            char ssid[33];
            memcpy(ssid, q + 1, len);
            ssid[len] = 0;

            int dup = -1;
            for (int i = 0; i < cnt; i++)
                if (strcmp(wifi_ap_list[i], ssid) == 0) { dup = i; break; }

            if (dup < 0) {
                memcpy(wifi_ap_list[cnt], ssid, len + 1);
                wifi_ap_rssi[cnt] = (int8_t)rssi;
                cnt++;
            } else if (rssi > wifi_ap_rssi[dup]) {
                wifi_ap_rssi[dup] = (int8_t)rssi;
            }
        }
        p = e;
    }

    /* 按信号强度从强到弱排序（最多 12 条，插入排序足够）：
     * 用户挑热点时最强的一般就是自己家那个，强的排前面省事。 */
    for (int i = 1; i < cnt; i++) {
        char  key[33];
        int8_t key_rssi = wifi_ap_rssi[i];
        int   j;
        memcpy(key, wifi_ap_list[i], sizeof key);
        for (j = i - 1; j >= 0 && wifi_ap_rssi[j] < key_rssi; j--) {
            memcpy(wifi_ap_list[j + 1], wifi_ap_list[j], sizeof key);
            wifi_ap_rssi[j + 1] = wifi_ap_rssi[j];
        }
        memcpy(wifi_ap_list[j + 1], key, sizeof key);
        wifi_ap_rssi[j + 1] = key_rssi;
    }

    if (cnt == 0)
        Log_Write(LOG_INFO, "scan: 0 AP");     /* 应答正常但没有热点，不算失败 */

    wifi_ap_count = (uint8_t)cnt;
    wifi_scan_code = (int8_t)cnt;
    wifi_scan_raw[0] = 0;                     /* 成功就别留上次失败的原始字节 */
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
    if (wifi_at_escape() != 0)
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

    /* 本函数只在线时被调用：恢复失败说明 TCP 已断，置离线让重连重建 */
    if (wifi_at_passthrough_enter() != 0)
    {
        g_mqtt_connected = 0;
        Log_Write(LOG_WARN, "sntp: link lost");
    }

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
