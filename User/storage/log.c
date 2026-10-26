/* 系统日志服务：W25Q128 日志分区的循环写入与倒序读取 */
#include "log.h"
#include "main.h"      /* dbg_print：日志镜像到调试串口 */
#include <string.h>
#include "stm32f4xx.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

uint32_t log_write_offset = 0U;    /* 下一条要写入的空白地址，最后一条有效日志在它前面 */

/* 分区寻址辅助（日志内部使用）：分区基地址 + 分区内偏移 = 芯片物理地址 */
static uint8_t part_read(uint32_t base, uint32_t off, uint8_t *buf, uint16_t len)
{
    if (buf == NULL) return 1;
    SPI_FLASH_BufferRead(buf, base + off, len);
    return 0;
}

static uint8_t part_write(uint32_t base, uint32_t off, uint8_t *buf, uint16_t len)
{
    if (buf == NULL) return 1;
    SPI_FLASH_BufferWrite(buf, base + off, len);
    return 0;
}

void Log_Init(void)
{
    log_write_offset = 0U;    /* 开机从日志分区 0 地址开始写 */
}

/* 业务点位：Log_Write() → 日志分区 → 日志页 page_log_refresh() 倒序读 → 列表新日志置顶 */
void Log_Write(Log_Level level, const char *str)
{
    Log_Obj log_buf = {0};
    uint16_t str_len;

    log_buf.time_stamp = HAL_GetTick();
    log_buf.level = level;
    log_buf.magic = LOG_MAGIC;

    str_len = strlen(str);
    if (str_len > LOG_CONTENT_LEN - 1U)
        str_len = LOG_CONTENT_LEN - 1U;
    memcpy(log_buf.content, str, str_len);

    /* 镜像到调试串口（UART5）：带级别标签，和屏幕日志页的 INFO/WARN/ERROR 三色对应。
     * 串口是纯文本不上色（也不该上 ANSI 色：现场用 CH340 + 任意终端），
     * 所以"三色"只存在于屏幕，串口靠这个标签表达同一信息。 */
    switch (level)
    {
        case LOG_WARN:  dbg_print("[LOG][WARN] ");  break;
        case LOG_ERROR: dbg_print("[LOG][ERROR] "); break;
        default:        dbg_print("[LOG][INFO] ");  break;
    }
    dbg_print(str);
    dbg_print("\r\n");

    if (flash_mutex) xSemaphoreTake(flash_mutex, portMAX_DELAY);

    /* 循环日志：写满回卷。NOR FLASH只能从 1→0，进新扇区先擦除；条目不跨扇区 */
    if ((log_write_offset + LOG_OBJ_SIZE) > LOG_PART_SIZE)          //情况1：写入位置超出日志分区大小
        log_write_offset = 0U;
    if (((log_write_offset & 0xFFFU) + LOG_OBJ_SIZE) > 0x1000U)     //情况2：写入位置跨扇区边界
    {
        log_write_offset += 0x1000U - (log_write_offset & 0xFFFU);
        if ((log_write_offset + LOG_OBJ_SIZE) > LOG_PART_SIZE)
            log_write_offset = 0U;
    }
    if ((log_write_offset & 0xFFFU) == 0U)                          //情况3：写入位置在扇区起始地址 → 擦除该扇区
        SPI_FLASH_SectorErase(LOG_PART_ADDR + log_write_offset);

    part_write(LOG_PART_ADDR, log_write_offset, (uint8_t *)&log_buf, LOG_OBJ_SIZE);
    log_write_offset += LOG_OBJ_SIZE;

    if (flash_mutex) xSemaphoreGive(flash_mutex);
}

uint8_t Log_ReadOne(uint32_t log_offset, Log_Obj *log_obj)
{
    if (log_obj == NULL) return 1;

    if (flash_mutex) xSemaphoreTake(flash_mutex, portMAX_DELAY);
    part_read(LOG_PART_ADDR, log_offset, (uint8_t *)log_obj, LOG_OBJ_SIZE);
    if (flash_mutex) xSemaphoreGive(flash_mutex);

    return (log_obj->magic != LOG_MAGIC);   /* 魔数不匹配 = 无效日志 */
}
