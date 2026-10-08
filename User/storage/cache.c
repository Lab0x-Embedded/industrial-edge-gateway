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

/* 上电扫描缓存分区，定位断电前最后一条有效数据，实现断电续写 */
void FlashCache_Init(void)
{
    CacheEntry_t entry;
    uint32_t addr = CACHE_START_ADDR;

    write_addr = CACHE_START_ADDR;
    while (addr < CACHE_END_ADDR) {
        SPI_FLASH_BufferRead((uint8_t *)&entry, addr, sizeof(CacheEntry_t));
        if (entry.magic != MAGIC_VALID) break;      /* 第一条无效处即写终点 */
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

    /* 环形回绕 + 进新扇区先擦除（NOR 只能 1→0） */
    if ((write_addr + sizeof(CacheEntry_t)) >= CACHE_END_ADDR) {
        write_addr = CACHE_START_ADDR;
        SPI_FLASH_SectorErase(write_addr);
    }
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
        read_addr = write_addr;   /* 读指针强制追上写指针：本轮到此为止 */
        return -2;
    }

    if (read_addr >= write_addr) {
        read_cnt = 0;
        return -2;
    }

    if (flash_mutex) xSemaphoreTake(flash_mutex, portMAX_DELAY);

    CacheEntry_t entry;
    SPI_FLASH_BufferRead((uint8_t *)&entry, read_addr, sizeof(CacheEntry_t));

    if (entry.magic != MAGIC_VALID) {     /* 只补传 MAGIC_VALID 的条目 */
        read_addr = write_addr;
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

/* 补传成功后标记已发：只改魔数，不擦除、不动指针 */
void FlashCache_MarkSent(void)
{
    if (!initialized) return;

    if (flash_mutex) xSemaphoreTake(flash_mutex, portMAX_DELAY);

    uint32_t cur = read_addr - sizeof(CacheEntry_t);   /* 前一条补传数据的位置 */
    if (cur >= CACHE_START_ADDR) {
        CacheEntry_t entry;
        SPI_FLASH_BufferRead((uint8_t *)&entry, cur, sizeof(CacheEntry_t));
        if (entry.magic == MAGIC_VALID) {
            entry.magic = MAGIC_EMPTY;
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

void FlashCache_Clear(void)
{
    if (flash_mutex) xSemaphoreTake(flash_mutex, portMAX_DELAY);
    for (uint32_t addr = CACHE_START_ADDR; addr < CACHE_END_ADDR; addr += CACHE_SECTOR_SIZE) {
        SPI_FLASH_SectorErase(addr);
    }
    write_addr = CACHE_START_ADDR;
    read_addr  = CACHE_START_ADDR;
    initialized = 1;
    if (flash_mutex) xSemaphoreGive(flash_mutex);
}

/* 补传发送封装：组包/topic/QoS 由云平台 profile 层（cloud.c）处理；
 * QoS1 平台以收到 PUBACK 为成功，失败返回非 0（调用方不标记已发，下轮重试） */
int MQTT_PublishJson(const char *json)
{
    int ret = cloud_publish(json);
    vTaskDelay(500);
    return ret;
}
