/* 断网补传缓存：断网期间存 JSON 条目，重连后逐条补传（PUBACK 确认才标记已发） */
#include "cache.h"
#include "w25qxx.h"
#include "cloud.h"
#include <string.h>
#include "stm32f4xx.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

/* 缓存条目：结构体对齐后 128 字节 */
typedef struct {
    uint32_t magic;          /* 魔术字：标记条目是否有效 */
    uint32_t timestamp;
    uint16_t data_len;
    uint8_t  data[CACHE_ENTRY_SIZE - 12];
} CacheEntry_t;

static uint32_t write_addr = CACHE_START_ADDR;
static uint32_t read_addr  = CACHE_START_ADDR;
static uint8_t  initialized = 0;

/* 上电扫描缓存分区，定位断电前最后一条有效数据，实现断电续写
 *
 * ★ 扫描的终止条件必须是"三种状态都不匹配"，不能只判 != MAGIC_VALID：
 *   条目有三种合法状态 —— 未发(MAGIC_VALID) / 已发(MAGIC_SENT) / 空白(擦除态)。
 *   只判 != MAGIC_VALID 会把 MAGIC_SENT 也当成"末端"，于是 write_addr
 *   停在第一个已发条目上，**后面那些还没发的条目被越过**，新数据直接覆盖它们
 *   （历史上的 MAGIC_EMPTY=0xFFFFFFFF 就踩了这个坑，见 cache.h 的说明）。
 *
 *  另一个细节：**已发的条目也要推进 write_addr**。它们是"占过位置的数据"，
 *   不能当成可复用的空洞——否则新数据会覆盖它们，而 read_addr 还没越过。
 *   只有三者都不匹配（真空白）才是写入终点。 */
void FlashCache_Init(void)
{
    CacheEntry_t entry;
    uint32_t addr = CACHE_START_ADDR;

    write_addr = CACHE_START_ADDR;
    while (addr < CACHE_END_ADDR) {
        SPI_FLASH_BufferRead((uint8_t *)&entry, addr, sizeof(CacheEntry_t));
        if (entry.magic != MAGIC_VALID && entry.magic != MAGIC_SENT)
            break;                                  /* 真空白：第一条三者都不匹配处即写终点 */
        write_addr = addr + sizeof(CacheEntry_t);
        addr += sizeof(CacheEntry_t);
    }

    if (write_addr >= CACHE_END_ADDR)
        write_addr = CACHE_START_ADDR;

    read_addr = CACHE_START_ADDR;
    initialized = 1;
}

int FlashCache_Write(const char *json, uint16_t len)
{
    if (!initialized) return -1;
    if (len > CACHE_ENTRY_SIZE - 12) return -2;

    CacheEntry_t entry;
    memset(&entry, 0xFF, sizeof(entry));
    entry.magic     = MAGIC_VALID;
    entry.timestamp = HAL_GetTick();
    entry.data_len  = len;
    memcpy(entry.data, json, len);

    if (flash_mutex) xSemaphoreTake(flash_mutex, portMAX_DELAY);

    /* 环形回绕：写满 4MB 后回到分区开头，续写前要覆盖最旧的数据。
     *
     * ★ 回绕前必须先确认"要擦的扇区已经没有未读数据"，否则会把
     *   read_addr 还指向的待补传条目一起擦掉——那是**真正丢数据**
     *   （新数据没写进去只是少一条，旧数据被擦掉则永远补不回来）。
     *   判据：read_addr 必须已经越过本轮要擦的扇区末尾。
     *   不满足就**拒绝写入**（返回 -3），宁可丢新数据、不能丢未发数据。
     *
     *   触发条件：断网积压满整个 4MB（32768 条 × 0.8s ≈ 7 小时才会满），
     *   平时不会走到，但长时间断网是真实场景。 */
    if ((write_addr + sizeof(CacheEntry_t)) >= CACHE_END_ADDR) {
        /* 回绕到分区开头前，检查 read 是否已越过第一个扇区 */
        if (read_addr < CACHE_START_ADDR + CACHE_SECTOR_SIZE) {
            if (flash_mutex) xSemaphoreGive(flash_mutex);
            return -3;                  /* 有未读数据挡着：放弃本次写入 */
        }
        write_addr = CACHE_START_ADDR;
        SPI_FLASH_SectorErase(write_addr);
    }
    /* 进新扇区先擦除（NOR 只能 1→0）。
     *
     * 正常推进时 read_addr 一定远在 write_addr 之前（读追上写就没有数据可读了），
     * 所以这里**无条件擦除**是对的——之前误写成"read_addr < ... 才擦"是反的。
     * 唯一需要保护的是"回绕时擦到 read 还没读过的地方"，那个判断在上面。 */
    if (write_addr % CACHE_SECTOR_SIZE == 0) {
        SPI_FLASH_SectorErase(write_addr);
    }

    SPI_FLASH_BufferWrite((uint8_t *)&entry, write_addr, sizeof(CacheEntry_t));
    write_addr += sizeof(CacheEntry_t);

    if (flash_mutex) xSemaphoreGive(flash_mutex);
    return 0;
}

int FlashCache_Read(char *json, uint16_t *len)
{
    if (!initialized) return -1;

    /* 限制单轮补传条数，防止长时间占住 MQTT 任务 */
    static int read_cnt = 0;
    if (read_cnt >= 5) {
        read_cnt = 0;
        return -2;                /* 本轮到此为止；read_addr 保持原位，下轮续读 */
    }

    if (flash_mutex) xSemaphoreTake(flash_mutex, portMAX_DELAY);

    /* ★ 遇到"已发"或"已损坏"的条目要**跳过并继续向后找**，不能整体放弃。
     *
     *   历史 bug：原来遇到 magic != MAGIC_VALID 就 `read_addr = write_addr`
     *   直接把读指针推到末尾——只要第一个条目是"已发"，后面所有未发数据
     *   就永远补传不出去（重启后 read_addr 从 0 开始，而 0 号往往刚被标成已发）。
     *
     *   正确语义：read_addr 是"扫描游标"，跳过不可用条目继续前进；
     *   只有真正追平 write_addr 才算没数据了。 */
    CacheEntry_t entry;
    while (read_addr < write_addr) {
        SPI_FLASH_BufferRead((uint8_t *)&entry, read_addr, sizeof(CacheEntry_t));

        if (entry.magic == MAGIC_VALID) break;              /* 找到一条待发 */
        if (entry.magic == MAGIC_SENT) {                    /* 已发：跳过 */
            read_addr += sizeof(CacheEntry_t);
            continue;
        }
        /* 三者都不匹配 = 真空白/损坏：后面不会再有数据，直接追平 */
        read_addr = write_addr;
        read_cnt = 0;
        if (flash_mutex) xSemaphoreGive(flash_mutex);
        return -2;
    }

    if (read_addr >= write_addr) {      /* 跳着跳着到头了 */
        read_cnt = 0;
        if (flash_mutex) xSemaphoreGive(flash_mutex);
        return -2;
    }

    memcpy(json, entry.data, entry.data_len);
    json[entry.data_len] = '\0';
    *len = entry.data_len;

    read_addr += sizeof(CacheEntry_t);
    read_cnt++;

    if (flash_mutex) xSemaphoreGive(flash_mutex);
    return 0;
}

/* 是否有未读数据：只看 read < write */
int FlashCache_HasData(void)
{
    if (!initialized) return 0;
    return (read_addr < write_addr);
}

/* 补传成功后标记已发：只改魔数，不擦除、不动指针
 *
 * MAGIC_SENT = 0x00000000 选这个值是刻意的：从 MAGIC_VALID(0xDEADBEEF)
 * 变过去是**纯 1→0**，NOR Flash 可以直接写，不需要擦除整个 4KB 扇区
 * （擦除会把同扇区另外 31 条数据一起抹掉）。
 * 旧代码用 0xFFFFFFFF 当"已发送"，需要 0→1 才能写进去——物理上做不到，
 * 所以这个函数历史上**从未真正生效**，见 cache.h 的详细说明。 */
void FlashCache_MarkSent(void)
{
    if (!initialized) return;

    if (flash_mutex) xSemaphoreTake(flash_mutex, portMAX_DELAY);

    uint32_t cur = read_addr - sizeof(CacheEntry_t);   /* 前一条补传数据的位置 */
    if (cur >= CACHE_START_ADDR) {
        CacheEntry_t entry;
        SPI_FLASH_BufferRead((uint8_t *)&entry, cur, sizeof(CacheEntry_t));
        if (entry.magic == MAGIC_VALID) {
            entry.magic = MAGIC_SENT;
            SPI_FLASH_WriteEnable();
            SPI_FLASH_BufferWrite((uint8_t *)&entry, cur, sizeof(CacheEntry_t));
        }
    }

    if (flash_mutex) xSemaphoreGive(flash_mutex);
}

/* 指针复位（不擦除）：补传异常时的快速恢复 */
void my_clear(void)
{
    write_addr = CACHE_START_ADDR;
    read_addr  = CACHE_START_ADDR;
}

/* 补传发送封装：组包/topic/QoS 由云平台 profile 层（cloud.c）处理；
 * QoS1 平台以收到 PUBACK 为成功，失败返回非 0（调用方不标记已发，下轮重试） */
int MQTT_PublishJson(const char *json)
{
    int ret = cloud_publish(json);
    vTaskDelay(500);
    return ret;
}
