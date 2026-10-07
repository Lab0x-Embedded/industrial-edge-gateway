#include "flash.h"
#include "bsp_flash.h"
#include "pal.h"
#include <string.h>
#include <stdio.h>
#include "stm32f4xx.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

// ===================== 新增：Flash 全局互斥锁 =====================
SemaphoreHandle_t flash_mutex = NULL;

// ===================== 新增：日志区写入偏移（循环写标记）=====================
 uint32_t log_write_offset = 0U;						//  下一条要写入的空白地址，最后一条有效日志在它前面(日志写指针 为空白地址)  日志偏移地址



extern UART_HandleTypeDef huart4;

//缓存 结构体对齐大小后为128字节
typedef struct {
    uint32_t magic;          // 魔术字：标记条目是否有效
    uint32_t timestamp;     // 时间戳
    uint16_t data_len;      // 有效数据长度
    uint8_t  data[CACHE_ENTRY_SIZE - 12]; // 数据区
} CacheEntry_t;

static uint32_t write_addr = CACHE_START_ADDR;
static uint32_t read_addr  = CACHE_START_ADDR;
static uint8_t  initialized = 0;


/**
 * @brief  分区读数据
 * @param  part_base 分区基地址
 * @param  offset    分区内偏移
 * @param  buf       接收缓冲区
 * @param  len       读取长度
 * @retval 0成功 1失败
 */
uint8_t Flash_Part_Read(uint32_t part_base, uint32_t offset, uint8_t *buf, uint16_t len)
{
    if(buf == NULL) return 1;
    uint32_t real_addr = part_base + offset;			//分区基地址 + 分区内偏移 = Flash 真实物理地址

    // 加锁保护
    //xSemaphoreTake(flash_mutex, portMAX_DELAY);
    SPI_FLASH_BufferRead(buf, real_addr, len);				// 调用底层硬件读
    //xSemaphoreGive(flash_mutex);

    return 0;
}

/**
 * @brief  分区写数据
 * @param  part_base 分区基地址
 * @param  offset    分区内偏移
 * @param  buf       待写数据
 * @param  len       写入长度
 * @retval 0成功 1失败
 */
uint8_t Flash_Part_Write(uint32_t part_base, uint32_t offset, uint8_t *buf, uint16_t len)
{
    if(buf == NULL) return 1;
    uint32_t real_addr = part_base + offset;

  // xSemaphoreTake(flash_mutex, portMAX_DELAY);
    SPI_FLASH_BufferWrite(buf, real_addr, len);
  //  xSemaphoreGive(flash_mutex);

    return 0;
}



//业务点位Log_Write() → W25Q日志分区存储 → 打开日志页触发page_log_refresh() → 倒序读Flash → LVGL列表(新日志置顶)
/**
入参校验：空字符串 / 锁未创建，直接退出，防止崩溃。
结构体赋值：
时间戳：HAL_GetTick() 记录事件发生时刻。
等级：传入的 LOG_INFO/LOG_WARN/LOG_ERROR。
魔数：固定 0x55AA，标记有效数据。
字符串拷贝：限制长度，防止超出结构体缓冲区导致内存越界。
循环覆盖逻辑
当「当前指针 + 单条日志大小」超过日志分区总大小 → 指针归零，实现循环日志。
工业设备常用方案：日志不手动删，写满自动覆盖旧日志。
调用分区接口写入：上层完全不碰底层地址。
写指针自增，准备写下一条日志。

 */
/*重要 日志写入 全工程统一入口*/
void Log_Write(Log_Level level, const char *str)
{
//    if(str == NULL || flash_mutex == NULL) return;

    Log_Obj log_buf = {0};
    uint16_t str_len;

    // 1. 填充日志结构体
    log_buf.time_stamp = HAL_GetTick();  // 系统运行时间戳
    log_buf.level = level;
    log_buf.magic = LOG_MAGIC;           // 标记为有效日志

    // 拷贝日志内容，防止缓冲区溢出
    str_len = strlen(str);
    if(str_len > LOG_CONTENT_LEN - 1U)
        str_len = LOG_CONTENT_LEN - 1U;
    memcpy(log_buf.content, str, str_len);				//拷贝函数的最后一个参数表示 要拷贝的字节数

    // 2. 日志区循环判断：写满则回到日志区头部
    if((log_write_offset + LOG_OBJ_SIZE) > LOG_PART_SIZE)
    {
        log_write_offset = 0U;
    }
    // 2.1 NOR 只能 1→0：先擦后写。条目不允许跨扇区——放不下就跳到下一扇区头
    //     （每次开机会写指针归零重写 0 地址，不擦除会与上次数据位与污染）
    if(((log_write_offset & 0xFFFU) + LOG_OBJ_SIZE) > 0x1000U)
    {
        log_write_offset += 0x1000U - (log_write_offset & 0xFFFU);
        if((log_write_offset + LOG_OBJ_SIZE) > LOG_PART_SIZE)
            log_write_offset = 0U;
    }
    if((log_write_offset & 0xFFFU) == 0U)
    {
        SPI_FLASH_SectorErase(LOG_PART_ADDR + log_write_offset);   /* 进入新扇区先擦除 */
    }
    // 3. 写入日志分区    基地址     偏移地址
    Flash_Part_Write(LOG_PART_ADDR, log_write_offset, (uint8_t *)&log_buf, LOG_OBJ_SIZE);
    // 4. 偏移自增
    log_write_offset += LOG_OBJ_SIZE;
}


/**
 * @brief  读取单条日志
 * @param  log_offset 日志区内偏移(读指针)
 * @param  log_obj    日志结构体接收缓存
 * @retval 0=有效日志 1=无效/空日志
 */
uint8_t Log_ReadOne(uint32_t log_offset, Log_Obj *log_obj)
{
    if(log_obj == NULL) return 1;

    Flash_Part_Read(LOG_PART_ADDR, log_offset, (uint8_t *)log_obj, LOG_OBJ_SIZE);

    // 魔数不匹配 = 无效日志
    if(log_obj->magic != LOG_MAGIC)
        return 1;

    return 0;
}


void Log_EraseAll(void)
{
    uint32_t sec_addr;
  // xSemaphoreTake(flash_mutex,portMAX_DELAY);
    //按4KB扇区循环擦除整个日志分区
    for(sec_addr = LOG_PART_ADDR; sec_addr < LOG_PART_ADDR + LOG_PART_SIZE; sec_addr += 4096)
    {
        SPI_FLASH_SectorErase(sec_addr);
        SPI_FLASH_WaitForWriteEnd();
    }
   //xSemaphoreGive(flash_mutex);
    //擦完直接把写指针置0
    log_write_offset = 0;
}


/*
上电时扫描 Flash 4MB~8MB 缓存分区，
找到上次断电前最后一条有效缓存数据，
定位下一条数据的写入地址，实现断电续写；
同时初始化读写指针与状态标记，让 Flash 缓存模块正常工作。
*/
void FlashCache_Init(void)
{
	
	 log_write_offset = 0;  //开机默认从日志分区0地址开始写
    CacheEntry_t entry;
    uint32_t addr = CACHE_START_ADDR;

    write_addr = CACHE_START_ADDR;
    while (addr < CACHE_END_ADDR) {
        SPI_FLASH_BufferRead((uint8_t *)&entry, addr, sizeof(CacheEntry_t));
			//如果数据有效 继续往下读
        if (entry.magic == MAGIC_VALID) {
            write_addr = addr + sizeof(CacheEntry_t);
        } else {
            break;
        }
        addr += sizeof(CacheEntry_t);
    }

    if (write_addr >= CACHE_END_ADDR) {
        write_addr = CACHE_START_ADDR;
    }

    read_addr = CACHE_START_ADDR;
    initialized = 1;
}

// 写入
int FlashCache_Write(const char *json, uint16_t len)
{
    if (!initialized) return -1;
    if (len > CACHE_ENTRY_SIZE - 12) return -2;

	
		    // ========== 加锁 ==========
  //  xSemaphoreTake(flash_mutex, portMAX_DELAY);
	
    CacheEntry_t entry;
		memset(&entry, 0xFF, sizeof(entry));		//擦除为1111
    entry.magic     = MAGIC_VALID;
    entry.timestamp = HAL_GetTick();
    entry.data_len  = len;
    memcpy(entry.data, json, len);

    // ─── 环形回绕处理 ───
    if ((write_addr +sizeof(CacheEntry_t))>= CACHE_END_ADDR) {   // 到达 8MB 边界
        write_addr = CACHE_START_ADDR;    // 回绕到 4MB
        SPI_FLASH_SectorErase(write_addr);// 擦除起始扇区
    }

    // ─── 扇区首地址擦除 ───
    if (write_addr % CACHE_SECTOR_SIZE == 0) {  // 写指针刚好在扇区边界
        SPI_FLASH_SectorErase(write_addr);      // 擦除整个 4KB 扇区
    }

    SPI_FLASH_BufferWrite((uint8_t *)&entry, write_addr, sizeof(CacheEntry_t));
    write_addr += sizeof(CacheEntry_t);	//固定128
		
		    // ========== 解锁 ==========
  //  xSemaphoreGive(flash_mutex);
    return 0;
}

// 读取一条，读完直接销毁（最安全，永不重复）
int FlashCache_ReadAndConsume(char *json, uint16_t *len)
{
    if (!initialized) return -1;

		    // ========== 加锁 ==========
  //  xSemaphoreTake(flash_mutex, portMAX_DELAY);
	
    CacheEntry_t entry;
    if (read_addr >= write_addr) {
        return -2; // 没有新数据
    }

    SPI_FLASH_BufferRead((uint8_t *)&entry, read_addr, sizeof(CacheEntry_t));
    if (entry.magic != MAGIC_VALID) {
        read_addr = write_addr;
        return -2;
    }

    // 读出数据
    memcpy(json, entry.data, entry.data_len);
    json[entry.data_len] = '\0';
    *len = entry.data_len;

    // 直接清空这条，防止重复读（最安全）
    entry.magic = MAGIC_EMPTY;
    SPI_FLASH_WriteEnable();
    SPI_FLASH_BufferWrite((uint8_t *)&entry, read_addr, sizeof(CacheEntry_t));

    read_addr += sizeof(CacheEntry_t);
		
		    // ========== 解锁 ==========
   // xSemaphoreGive(flash_mutex);
		
    return 0;
}

// 是否有未读数据
// 【 fix 】判断是否有未读数据：只看 read < write，
// ======================================================================
int FlashCache_HasData(void)
{
    if (!initialized) return 0;

    // 最多只允许补传 5 条！！！
    // 强制限制，永远不会无限循环
    static int total = 0;
    if (total >= 5) {
        total = 0;
        return 0;
    }

    if (read_addr < write_addr) {		// 读指针落后写指针，说明有数据
        total++;
        return 1;
    }

    total = 0;
    return 0;
}

void my_clear()
{
	    write_addr = CACHE_START_ADDR;
    read_addr  = CACHE_START_ADDR;
}

// 清空
void FlashCache_Clear(void)
{
		    // ========== 加锁 ==========
 //   xSemaphoreTake(flash_mutex, portMAX_DELAY);
    for (uint32_t addr = CACHE_START_ADDR; addr < CACHE_END_ADDR; addr += CACHE_SECTOR_SIZE) {
        SPI_FLASH_SectorErase(addr);
    }
    write_addr = CACHE_START_ADDR;
    read_addr  = CACHE_START_ADDR;
    initialized = 1;
		
		    // ========== 解锁 ==========
  //  xSemaphoreGive(flash_mutex);
}

//补传到云端
int MQTT_PublishJson(const char *json)
{
    /* 组包/选 topic/QoS 由云平台 profile 层（pal.c）统一处理；
     * QoS1 平台以收到 PUBACK 为成功，失败返回 -1（调用方不标记已发，下轮重试） */
    int ret = pal_mqtt_publish(json);
    vTaskDelay(500);
    return ret;
}

// 【 fix 】标记已发送：只改魔数，不擦除、不乱动指针
// ======================================================================
void FlashCache_MarkSent(void)
{
    if (!initialized) return;

    uint32_t cur = read_addr - sizeof(CacheEntry_t);	//找到前面补传数据的位置 
    if (cur < CACHE_START_ADDR) return;

    CacheEntry_t entry;
    SPI_FLASH_BufferRead((uint8_t *)&entry, cur, sizeof(CacheEntry_t));

    if (entry.magic == MAGIC_VALID) {
			entry.magic = MAGIC_EMPTY; // 只改魔数 不擦除扇区 标记已补传 下次不会再补传
        SPI_FLASH_WriteEnable();			 // SPI Flash 写使能（硬件要求，写操作前必须开启）
        SPI_FLASH_BufferWrite((uint8_t *)&entry, cur, sizeof(CacheEntry_t));			  // 把修改后的条目写回 Flash
    }
}

/*从读指针位置读一条缓存数据*/
int FlashCache_Read(char *json, uint16_t *len)
{
    if (!initialized) return -1;

    // 强制最多读 5 条，读完直接锁死
    static int read_cnt = 0;
    if (read_cnt >= 5) {
        read_cnt = 0;
        read_addr = write_addr;  // 👈 强制让读指针追上写指针 = 永远不再补传
        return -2;
    }

    if (read_addr >= write_addr) {
        read_cnt = 0;
        return -2;
    }

		    // ========== 加锁 ==========
 //   xSemaphoreTake(flash_mutex, portMAX_DELAY);
		
    CacheEntry_t entry;
    SPI_FLASH_BufferRead((uint8_t *)&entry, read_addr, sizeof(CacheEntry_t));

    if (entry.magic != MAGIC_VALID) {			/*只补传 这个值MAGIC_VALID*/
        read_addr = write_addr;
        read_cnt = 0;
        return -2;
    }

    memcpy(json, entry.data, entry.data_len);
    json[entry.data_len] = '\0';
    *len = entry.data_len;

    read_addr += sizeof(CacheEntry_t);//128
    read_cnt++;
		
		    // ========== 解锁 ==========
   // xSemaphoreGive(flash_mutex);
    return 0;
}                