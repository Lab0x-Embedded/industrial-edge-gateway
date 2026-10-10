/* WiFi 连接层：凭据管理（W25Q 持久化）+ ESP8266 入网 + IP 查询
 *              + 热点扫描（异步，AT 任务执行）+ SNTP 校时 */
#include "wifi_conn.h"
#include "w25qxx.h"
#include "esp_at.h"
#include "ipv4_parse.h"   /* 抓最后一个 IPv4 字面量（纯函数，主机侧有单测） */
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
extern uint8_t esp_rx_dma_buf[256];
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
volatile uint8_t wifi_timesync_request = 0;
volatile uint8_t wifi_timesync_result = 0;
volatile uint8_t wifi_scan_state = WIFI_SCAN_IDLE;
volatile uint8_t wifi_ap_count = 0;
char             wifi_ap_list[WIFI_AP_MAX][33];
int8_t           wifi_ap_rssi[WIFI_AP_MAX];

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

/* 入网成功后查 IP。
 * 为什么不能再用 ESP_SendCmd：它是"匹配到就丢弃"的流式实现（CWLAP 那次就
 * 踩过，手册 #23 同款），等 "STAIP" 关键字命中时整行——IP 就在这一行——
 * 已经被吃进局部 line 缓冲丢掉了。原实现命中后再抢读 600ms，读到的只有
 * STAMAC/OK，所以 g_esp_ip 实际永远停在 "--"。
 * 这里照 CWLAP 的收法：整段应答收完（OK/ERROR/超时），再用纯函数解析器
 * 抓**最后一个** IPv4 字面量（ipv4_parse.h，主机侧有单测）。 */
static void wifi_query_ip(void)
{
    static char buf[256];       /* CIFSR 应答含回显不到 128B；静态免任务栈压力 */
    int n = 0;

    RingBuf_Clear();            /* 入网刚成功，缓冲里可能有残留杂字节 */
    HAL_UART_Transmit(&huart4, (uint8_t *)"AT+CIFSR\r\n", 10, 100);
    uint32_t t0 = HAL_GetTick();
    while ((HAL_GetTick() - t0) < 2000 && n < (int)sizeof(buf) - 1) {
        uint8_t ch;
        if (RingBuf_Read(&ch)) {
            buf[n++] = (char)ch;
            buf[n] = 0;
            if (strstr(buf, "\r\nOK\r\n") || strstr(buf, "ERROR"))
                break;          /* 应答收完（与 wifi_scan_locked 同款退出条件） */
        } else {
            vTaskDelay(2);
        }
    }
    buf[n] = 0;

    g_esp_ip[0] = '-';
    g_esp_ip[1] = '-';
    g_esp_ip[2] = 0;
    if (ipv4_find_last(buf, (uint16_t)n, g_esp_ip, sizeof g_esp_ip) > 0)
        dbg_print("[NET] ip %s\r\n", g_esp_ip);
}

int wifi_join(void)
{
    char cmd[192];              /* 160→192：SSID 32 + PASS 64 + 转义 + 固定部分，留余量 */
    char msg[128];              /* 80→128：SSID 是 UTF-8 中文时 32 字节能占满 80 */
    ESP_Status_t r;
    wifi_conn_load();

    /* 显式检查 snprintf 返回值：SSID/密码里若含需要转义的字符（" \），
     * 或者以后密码改成 64 位且含转义，cmd 可能被截断。截断后发出去的是
     * 半截 AT 命令，模块回 ERROR，会被下面误判成"密码错"——现场排查跑偏。 */
    int n = snprintf(cmd, sizeof cmd, "AT+CWJAP=\"%s\",\"%s\"\r\n",
                     wifi_ssid, wifi_pass);
    if (n < 0 || n >= (int)sizeof cmd) {
        Log_Write(LOG_WARN, "wifi cmd too long");
        dbg_print("[NET] wifi join FAILED: cmd overflow\r\n");
        wifi_connected = 0;
        return 0;
    }

    /* 超时 5s → 15s：CWJAP 要扫描+关联+DHCP，正常 3~8s，信号弱/路由器 DHCP 慢
     * 时更久。5s 会在正常慢的情况下误判失败，而失败之后紧接着的 CIPSTART
     * 必然连不上 —— 表现就是"偶尔/经常连不上"，且日志只有一句 join failed。 */
    r = ESP_SendCmd(cmd, "WIFI GOT IP", 15000);

    /* 重试策略（"运行时切网永远 wifi timeout、复位才好"的修复，手册 #27）：
     * - TIMEOUT：CWJAP 撞上模块自动重连存储 AP 的 busy 窗口被 "busy p..." 丢弃
     *   （该行不含 ERROR/FAIL 关键词，ESP_SendCmd 只能等满 15s 超时）。窗口有
     *   3~8s，而 RST 把每次尝试都钉在同一相位上，只发一次必死 —— 2s 间隔再试，
     *   最多 4 次，把整个窗口吸收掉。 */
    for (int attempt = 1; r == ESP_TIMEOUT && attempt < 4; attempt++) {
        snprintf(msg, sizeof msg, "wifi join timeout, retry #%d", attempt + 1);
        Log_Write(LOG_WARN, msg);
        vTaskDelay(pdMS_TO_TICKS(2000));
        r = ESP_SendCmd(cmd, "WIFI GOT IP", 15000);
    }
    /* - ERROR/FAIL：模块明确拒绝（多半密码/SSID 错）→ 补试一次就定论。
     *   真实拒绝重试 4 轮只是白等，别把"密码错"拖成一分多钟的 Connecting。 */
    if (r == ESP_ERROR) {
        vTaskDelay(pdMS_TO_TICKS(500));
        r = ESP_SendCmd(cmd, "WIFI GOT IP", 15000);
    }

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

/* 自愈：UART4 的 RX DMA 还挂着吗？掉了就重新挂上。
 * 为什么留着它：一旦 UART4 出现溢出(ORE)/帧错误/噪声，STM32 HAL 会**中止**正在
 * 进行的 DMA 接收且不会自动重启（HAL_UART_ErrorCallback 是弱定义空函数）。
 * 虽然 esp_uart.c 已经补上了 ErrorCallback（出错时重挂），但那里若重挂失败
 * （HAL_BUSY 等）就没救了 —— 这里再兜一层，检测到停摆就重挂并记日志。 */
static void rx_selfcheck(void)
{
    if (huart4.RxState == HAL_UART_STATE_BUSY_RX)
        return;
    __HAL_UART_CLEAR_OREFLAG(&huart4);          /* F4 上这一句同时清 PE/FE/NE/ORE */
    huart4.ErrorCode = HAL_UART_ERROR_NONE;
    HAL_UARTEx_ReceiveToIdle_DMA(&huart4, esp_rx_dma_buf, sizeof(esp_rx_dma_buf));
    __HAL_DMA_DISABLE_IT(&hdma_uart4_rx, DMA_IT_HT);
    Log_Write(LOG_WARN, "uart4 rx DEAD -> rearm");
}

/* 裸探测一次 AT：OK=确认在 AT 指令模式；模块若行缓冲里有垃圾（或被 +++ 毒过），
 * 第一发 AT 会把它拼成脏行回 ERROR —— 那条脏行已被这次 AT 消费掉，紧跟着再补
 * 一发裸 AT 就该答 OK。两发都没 OK 才算"无应答"。 */
static int at_probe_direct(uint32_t timeout_ms)
{
    if (ESP_SendCmd_OK("AT\r\n", timeout_ms) == ESP_OK)
        return 0;
    return (ESP_SendCmd_OK("AT\r\n", timeout_ms) == ESP_OK) ? 0 : -1;
}

static int at_exit_passthrough_impl(void)
{
    /* ① 先裸探测，绝不先发 +++：
     *    模块在 AT 指令模式（上电后/自发重启后/从未进过透传）时，+++ 没有终止符，
     *    会一直挂在模块的行缓冲里；随后的 AT 被拼成 "+++AT" → ERROR —— 每轮重连
     *    都重演，表现是模块活着、err=0、却永远回 ERROR："no AT mode" 死锁，
     *    后面真正能救场的 AT+RST 永远走不到（现场：rx=288、err=0、次次 ERROR）。
     *    只有裸探测无应答 —— 那是透传模式的指纹 —— 才走 +++ 流程。 */
    RingBuf_Clear();
    if (at_probe_direct(600) == 0)
    {
        RingBuf_Clear();
        return 0;
    }

    /* ② 透传退出：三次机会，且一次比一次给更长的保护时间。
     * ESP 只认"+++ 前后各 20ms 都安静"的那种 +++；透传模式下随时可能有下行数据
     * 把 +++ 吞掉，所以先用 wait_line_idle() 等出一个真正的静默窗口再发，
     * 而不是盲等 50ms。 */
    for (int attempt = 0; attempt < 3; attempt++)
    {
        RingBuf_Clear();
        wait_line_idle(60);                      /* 等线路安静下来（最多等 240ms） */
        RingBuf_Clear();
        HAL_UART_Transmit(&huart4, (uint8_t *)"+++", 3, 100);
        vTaskDelay(1000 + attempt * 400);        /* 退透传保护时间：1.0 / 1.4 / 1.8s */
        if (ESP_SendCmd_OK("AT\r\n", 2000) == ESP_OK)
        {
            RingBuf_Clear();
            return 0;
        }
        dbg_print("[NET] at escape retry\r\n");
    }

    rx_selfcheck();                              /* 三次都没应答：确认接收没停摆 */
    return -1;
}

int wifi_at_escape(void)
{
    return at_exit_passthrough_impl();
}

/* AT+RST 之后必须等模块**真的起来**再发后续指令。
 * AT 固件复位要 1~3s 才重新应答（启动时先打印 "ready"）。原来的重连流程只盲等
 * 1s 就发 CWMODE/CWJAP：模块还在启动，指令被丢（或回 busy），CWJAP 自然入不了网，
 * 现象就是"点连接永远失败"——而开机那条路没有 AT+RST，所以反而不受影响。
 * 做法：反复发 AT，直到收到 OK 或超时。返回 0=已就绪。 */
int wifi_at_wait_ready(uint32_t timeout_ms)
{
    uint32_t t0 = HAL_GetTick();
    while (HAL_GetTick() - t0 < timeout_ms)
    {
        if (ESP_SendCmd_OK("AT\r\n", 400) == ESP_OK)
            return 0;
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    return -1;
}

/* 恢复透传。返回 0=拿到 ">" 提示、透传真的起来了；-1=没拿到（多半是 TCP 已经
 * 断了 —— 此时模块停在"CIPMODE=1 但没在发送"的中间态，后续裸发的 MQTT 字节
 * 会被模块当 AT 指令吃成 ERROR）。调用方拿到 -1 应该把 g_mqtt_connected 置 0，
 * 让重连流程（RE_MQTT_Init 会 AT+RST）去重建，而不是继续往串口灌数据。
 *
 * 这里只发 CIPSEND、不再发 CIPMODE=1：所有调用点都保证 CIPMODE 已经是 1 ——
 * 冷启动/重连在 CIPSTART 之前设好（cloud.c），扫描/校时恢复路径里 +++
 * 只退出发送、不清 CIPMODE；连接还挂着时对部分固件再设 CIPMODE 反而回 ERROR。 */
int wifi_at_passthrough_enter(void)
{
    /* ">" 提示符是流式字节，不保证后面跟 \r\n；ESP_SendCmd 按行匹配可能永远
     * 凑不满一行。这里按字节流等 '>'，echo/OK/杂散应答天然被跳过。 */
    HAL_UART_Transmit(&huart4, (uint8_t *)"AT+CIPSEND\r\n", 12, 100);
    uint32_t t0 = HAL_GetTick();
    for (;;) {
        uint8_t ch;
        while (RingBuf_Read(&ch)) {
            if (ch == '>') {
                RingBuf_Clear();
                return 0;
            }
        }
        if (HAL_GetTick() - t0 > 3000) {
            dbg_print("[NET] cipsend: no '>' prompt\r\n");
            return -1;
        }
        vTaskDelay(1);
    }
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

    if (wifi_at_escape() != 0)
    {
        Log_Write(LOG_WARN, "scan FAIL: no AT mode");
        return -1;                  /* 没确认进入 AT 模式：此时多半还在透传，
                                     * 不能再发 CIPMODE/CIPSEND 去"恢复"（会污染 TCP 流） */
    }

    /* 顺手把模块拉回 station 模式：CWLAP 只在 station / station+AP 下可用，
     * 模块若停在 SoftAP（CWMODE=2，出厂默认就是 2 的固件）会直接对 CWLAP 回
     * ERROR —— 现象就是"扫描永远失败"。幂等且便宜，一次 2s 上限。 */
    ESP_SendCmd_OK("AT+CWMODE=1\r\n", 2000);

    /* 必须手动逐字节收：ESP_SendCmd 是"匹配到就丢弃"的流式实现，
     * 用它会把 +CWLAP 行当匹配数据吃掉，啥也剩不下。 */
    HAL_UART_Transmit(&huart4, (uint8_t *)"AT+CWLAP\r\n", 10, 100);
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
        Log_Write(LOG_WARN, "scan FAIL: no reply");
        return -2;
    }
    if (strstr(buf, "ERROR") != NULL)
    {
        /* 模块明确回 ERROR（如 "busy p..."、CWMODE 不对） */
        Log_Write(LOG_WARN, "scan FAIL: CWLAP error");
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
    char cmd[96];

    /* 退不出透传就别发校时指令：会被当成 TCP 数据吃掉，还可能污染 MQTT 流 */
    if (wifi_at_escape() != 0)
        return -1;

    snprintf(cmd, sizeof cmd, "AT+CIPSNTPCFG=1,%d,\"%s\"\r\n", SNTP_TIMEZONE, SNTP_SERVER);
    /* 返回值不能无视：固件若不认数字时区会在这 ERROR（[AT] err 会亮原话），
     * 配置失败时下面的查询会永远 1970 —— 早知道早排查 */
    if (ESP_SendCmd_OK(cmd, 2000) != ESP_OK)
        Log_Write(LOG_WARN, "sntp: cfg fail");

    /* CIPSNTPCFG 只是"配置"，模块首次真正对上 NTP 要几秒（解析服务器域名+发包）。
     * 配置完立刻查多半还是 1970 —— 在这里轮询到年份像样为止（≤10s），
     * 把"首查太早"吸收进一次调用，而不是烧掉 tasks.c 里那点重试次数。 */
    uint32_t t0 = HAL_GetTick();
    while (HAL_GetTick() - t0 < 10000)
    {
        int n = 0;
        RingBuf_Clear();
        HAL_UART_Transmit(&huart4, (uint8_t *)"AT+CIPSNTPTIME?\r\n", 18, 100);
        uint32_t tq = HAL_GetTick();
        while (HAL_GetTick() - tq < 2000 && n < (int)sizeof(buf) - 1) {
            uint8_t ch;
            if (RingBuf_Read(&ch)) buf[n++] = (char)ch;
            else                   vTaskDelay(5);
        }
        buf[n] = 0;

        /* 应答形如： +CIPSNTPTIME:Fri Oct  9 12:34:56 2026（ctime 格式，日/时补空格）
         * 一把解析 星期 月 日 时:分:秒 年 —— 日期也要：主屏左上角显示，RTC 日期寄存器存档。
         * 年份 <2000（典型 1970）= 模块还没对上 NTP，不算失败，等 500ms 再查 */
        char mon[4] = {0};
        int day = 0, hh = 0, mm = 0, ss = 0, yr = 0;
        char *p = strstr(buf, "+CIPSNTPTIME:");
        if (p == NULL ||
            sscanf(p + 13, "%*3s %3s %d %d:%d:%d %d", mon, &day, &hh, &mm, &ss, &yr) != 6)
        {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        /* 月名转号：十二个缩写在这张串里各出现一次，位置/3+1 即月份；
         * %3 对齐检查是防脏数据卡进非月名子串 */
        static const char mon_names[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
        const char *mp = strstr(mon_names, mon);
        if (mp == NULL || (mp - mon_names) % 3 != 0 ||
            yr < 2000 || yr > 2099 || day < 1 || day > 31 ||
            hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 59)
        {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        int mo = (int)(mp - mon_names) / 3 + 1;
        int link_ok = (wifi_at_passthrough_enter() == 0);
        Clock_SetDateTime((uint16_t)yr, (uint8_t)mo, (uint8_t)day,
                          (uint8_t)hh, (uint8_t)mm, (uint8_t)ss);
        dbg_print("[NET] sntp %04d-%02d-%02d %02d:%02d:%02d\r\n", yr, mo, day, hh, mm, ss);
        if (!link_ok)
        {
            /* 本函数只在线时被调用：恢复失败说明 TCP 已断，置离线让重连重建 */
            g_mqtt_connected = 0;
            Log_Write(LOG_WARN, "sntp: link lost");
        }
        return 0;
    }

    /* 10s 都没等到像样的时间（多半 NTP 不通/固件不支持）：恢复透传干净收场 */
    if (wifi_at_passthrough_enter() != 0)
    {
        g_mqtt_connected = 0;
        Log_Write(LOG_WARN, "sntp: link lost");
    }
    return -2;
}
