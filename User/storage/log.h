#ifndef LOG_H
#define LOG_H
/* ===================== 系统日志（W25Q128 日志分区 0 ~ 4MB） =====================
 * 循环日志：写满自动回卷覆盖旧日志；条目不跨 4KB 扇区，进新扇区先擦除。
 * 页面呈现：日志页从写指针倒序读取，最新置顶。
 */
#include <stdint.h>
#include "w25qxx.h"

#define LOG_PART_ADDR       0x000000U
#define LOG_PART_SIZE       (0x400000U - 1)

#define LOG_CONTENT_LEN     64U     /* 单条日志内容最大长度 */
#define LOG_MAGIC           0x55AAU /* 有效日志魔数 */

typedef enum
{
    LOG_INFO  = 0,   /* 正常信息：联网、补传、启动 */
    LOG_WARN  = 1,   /* 警告：断网、重连 */
    LOG_ERROR = 2    /* 错误：Modbus超时、SPI故障 */
} Log_Level;

typedef struct
{
    uint32_t    time_stamp;                  /* 系统运行滴答数 HAL_GetTick() */
    Log_Level   level;
    uint8_t     content[LOG_CONTENT_LEN];
    uint32_t    magic;                       /* 32 位魔数：结构体恰好 76 字节，无尾部填充歧义 */
} Log_Obj;

#define LOG_OBJ_SIZE        sizeof(Log_Obj)

extern uint32_t log_write_offset;   /* 下一条要写入的偏移（空白地址），日志页倒序读的起点 */

void    Log_Init(void);                                    /* 上电：写指针归零 */
void    Log_Write(Log_Level level, const char *str);       /* 全工程统一日志入口 */
uint8_t Log_ReadOne(uint32_t log_offset, Log_Obj *log_obj);/* 读单条，0=有效 */
void    Log_EraseAll(void);

#endif
