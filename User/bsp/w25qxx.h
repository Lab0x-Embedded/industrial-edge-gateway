#ifndef __SPI_FLASH_H
#define __SPI_FLASH_H

#include "stm32f4xx_hal.h"
#include "main.h"
#include <stdio.h>

#define sFLASH_ID                       0xEF4018     // W25Q64
#define SPI_FLASH_PageSize              256
#define SPI_FLASH_PerWritePageSize      256

/* 命令定义 */
#define W25X_WriteEnable        0x06 
#define W25X_WriteDisable       0x04 
#define W25X_ReadStatusReg      0x05 
#define W25X_WriteStatusReg     0x01 
#define W25X_ReadData           0x03 
#define W25X_FastReadData       0x0B 
#define W25X_FastReadDual       0x3B 
#define W25X_PageProgram        0x02 
#define W25X_BlockErase         0xD8 
#define W25X_SectorErase        0x20 
#define W25X_ChipErase          0xC7 
#define W25X_PowerDown          0xB9 
#define W25X_ReleasePowerDown   0xAB 
#define W25X_DeviceID           0xAB 
#define W25X_ManufactDeviceID   0x90 
#define W25X_JedecDeviceID      0x9F

#define WIP_Flag                0x01
#define Dummy_Byte              0xFF

/* SPI 总线：W25Q 挂在 CubeMX 配置的 SPI1 上（PB3/PB4/PB5，为避开触摸的
 * PA5 被挪到了 B 口），句柄直接复用 CubeMX 生成的 hspi1。
#define SPIx                            SPI1 
*/
#define SpiHandle                       hspi1

/* 引脚定义 —— 统一复用 main.h 的 CubeMX 配置（PG8） */
#define FLASH_CS_PIN                     W25Q_CS_Pin
#define FLASH_CS_GPIO_PORT               W25Q_CS_GPIO_Port

/* CS 控制（使用 HAL 标准函数）*/
#define SPI_FLASH_CS_LOW()       HAL_GPIO_WritePin(FLASH_CS_GPIO_PORT, FLASH_CS_PIN, GPIO_PIN_RESET)
#define SPI_FLASH_CS_HIGH()      HAL_GPIO_WritePin(FLASH_CS_GPIO_PORT, FLASH_CS_PIN, GPIO_PIN_SET)

/* 超时 */
#define SPIT_FLAG_TIMEOUT         ((uint32_t)0x1000)
#define SPIT_LONG_TIMEOUT         ((uint32_t)(10 * SPIT_FLAG_TIMEOUT))

/* 调试输出 */
#define FLASH_DEBUG_ON         1
#define FLASH_INFO(fmt,arg...)           dbg_print("<<-FLASH-INFO->> "fmt"\n",##arg)
#define FLASH_ERROR(fmt,arg...)          dbg_print("<<-FLASH-ERROR->> "fmt"\n",##arg)
#define FLASH_DEBUG(fmt,arg...)          do{\
                                          if(FLASH_DEBUG_ON)\
                                          dbg_print("<<-FLASH-DEBUG->> [%d]"fmt"\n",__LINE__, ##arg);\
                                          }while(0)

/* 函数声明 */
void SPI_FLASH_Init(void);
void SPI_FLASH_SectorErase(uint32_t SectorAddr);
void SPI_FLASH_PageWrite(uint8_t* pBuffer, uint32_t WriteAddr, uint16_t NumByteToWrite);
void SPI_FLASH_BufferWrite(uint8_t* pBuffer, uint32_t WriteAddr, uint16_t NumByteToWrite);
void SPI_FLASH_BufferRead(uint8_t* pBuffer, uint32_t ReadAddr, uint16_t NumByteToRead);
uint32_t SPI_FLASH_ReadID(void);
uint8_t SPI_FLASH_SendByte(uint8_t byte);
void SPI_FLASH_WriteEnable(void);
void SPI_FLASH_WaitForWriteEnd(void);
uint8_t  SPI_FLASH_ReadSR(void);    /* 状态寄存器：bit2/3/4=BP 块保护，非 0 擦写静默失败 */
void     SPI_FLASH_WriteSR(uint8_t sr);

/* W25Q 全局互斥锁：日志/缓存/触摸校准共用同一颗芯片，跨任务访问都须经它 */
#include "FreeRTOS.h"
#include "semphr.h"
extern SemaphoreHandle_t flash_mutex;

#endif