#ifndef CACHE_H
#define CACHE_H
/* ===================== 断网补传缓存（W25Q128 缓存分区 4 ~ 8MB） =====================
 * 断网期间实时数据以 JSON 条目（128B/条）顺序写入；重连后逐条读出补传，
 * 发送成功（PUBACK 确认）才标记已发（只改魔数不擦除）。
 */
#include <stdint.h>

#define CACHE_START_ADDR    (4 * 1024 * 1024)
#define CACHE_END_ADDR      (8 * 1024 * 1024)
#define CACHE_SECTOR_SIZE   4096
#define CACHE_ENTRY_SIZE    128

#define MAGIC_VALID         0xDEADBEEF
#define MAGIC_EMPTY         0xFFFFFFFF

void FlashCache_Init(void);                       /* 上电：扫描定位断电前写地址，续写 */
int  FlashCache_Write(const char *json, uint16_t len);
int  FlashCache_Read(char *json, uint16_t *len);  /* 读一条（不消费） */
int  FlashCache_HasData(void);
void FlashCache_MarkSent(void);                   /* 补传成功后标记该条已发 */
void FlashCache_Clear(void);
void my_clear(void);                              /* 指针复位（不擦除） */

int  MQTT_PublishJson(const char *json);          /* 补传发送封装：失败不标记，下轮重试 */

#endif
