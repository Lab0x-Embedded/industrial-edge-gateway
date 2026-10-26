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

/* ===================== 条目魔数（三种状态，必须互相可区分） =====================
 *
 *  NOR Flash 的物理约束决定了这三个值必须满足两条：
 *    ① "未发 → 已发" 的写入必须是**纯 1→0**（NOR 只能 1→0，要 0→1 得擦整扇区）
 *    ② "已发" 不能等于**擦除态 0xFFFFFFFF**，否则和"从没写过"无法区分
 *
 *  历史（★ 两个真实 bug，2026-10 修复）：
 *    旧值 MAGIC_EMPTY = 0xFFFFFFFF，同时踩了上面两条：
 *      ① 0xDEADBEEF → 0xFFFFFFFF 有 8 个 bit 需要 0→1，**物理上写不进去**；
 *         而 SPI_FLASH_BufferWrite 写后不校验，所以 MarkSent() 一直静默失败
 *         → 已发条目永远标记不上 → 每次重启都把全部历史数据重传一遍。
 *      ② 即便能写进去，它又恰好等于擦除态，扫描时和"空白"无法区分
 *         → 扫描会在第一个已发条目处停下，后面的未发数据被越过覆盖。
 *
 *    改成 MAGIC_SENT = 0x00000000 后两个问题一起消失：
 *      - 0xDEADBEEF → 0x00000000 是纯 1→0，直接可写，不需要擦除
 *      - 和擦除态 0xFFFFFFFF 天然可区分，扫描能正确区分"已发/未发/空白"
 *
 *  注意：这是**存进 Flash 的数据格式**，改了值就意味着旧固件写的缓存条目
 *  不再被识别（会被当成空白，从头覆盖）。只影响断网补传的历史数据，可接受。 */
#define MAGIC_VALID         0xDEADBEEF   /* 已写入、未发送 */
#define MAGIC_SENT          0x00000000   /* 已发送（PUBACK 确认）；纯 1→0 可写 */
/* 擦除态 0xFFFFFFFF 本身即"从未写过"，不再单独定义宏——
 * 它就是 Flash 擦除后的默认值，任何"哨兵值"都不该复用它。 */

void FlashCache_Init(void);                       /* 上电：扫描定位断电前写地址，续写 */
int  FlashCache_Write(const char *json, uint16_t len);
int  FlashCache_Read(char *json, uint16_t *len);  /* 读一条（不消费） */
int  FlashCache_HasData(void);
void FlashCache_MarkSent(void);                   /* 补传成功后标记该条已发 */
void my_clear(void);                              /* 指针复位（不擦除） */

int  MQTT_PublishJson(const char *json);          /* 补传发送封装：失败不标记，下轮重试 */

#endif
