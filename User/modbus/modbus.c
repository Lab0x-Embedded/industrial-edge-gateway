#include "modbus.h"
#include "stm32f4xx.h"
#include "usart.h"
#include "stdio.h"
#include "string.h"
#include "stm32f4xx_hal_def.h"
#include "FreeRTOS.h"
#include "task.h"
#include "log.h"
#include "tasks.h"
#include "cloud.h"

/* ================================================================
 *	查表法
 *  CRC 高字节表 (高 8 位部分)
 *  由多项式 0x8005 预计算生成，共 256 项
 * ================================================================ */

extern QueueHandle_t     data_queue;
extern UART_HandleTypeDef huart2;

static const uint8_t auchCRCHi[] = {
    0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x01, 0xC0,
    0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41,
    0x00, 0xC1, 0x81, 0x40, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0,
    0x80, 0x41, 0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 0x81, 0x40,
    0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1,
    0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x01, 0xC0, 0x80, 0x41,
    0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1,
    0x81, 0x40, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41,
    0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x01, 0xC0,
    0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x00, 0xC1, 0x81, 0x40,
    0x01, 0xC0, 0x80, 0x41, 0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1,
    0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 0x81, 0x40,
    0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x01, 0xC0,
    0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x00, 0xC1, 0x81, 0x40,
    0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0,
    0x80, 0x41, 0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 0x81, 0x40,
    0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x01, 0xC0,
    0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41,
    0x00, 0xC1, 0x81, 0x40, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0,
    0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41,
    0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0,
    0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x00, 0xC1, 0x81, 0x40,
    0x01, 0xC0, 0x80, 0x41, 0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1,
    0x81, 0x40, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41,
    0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x01, 0xC0,
    0x80, 0x41, 0x00, 0xC1, 0x81, 0x40
};

static const uint8_t auchCRCLo[] = {
    0x00, 0xC0, 0xC1, 0x01, 0xC3, 0x03, 0x02, 0xC2, 0xC6, 0x06,
    0x07, 0xC7, 0x05, 0xC5, 0xC4, 0x04, 0xCC, 0x0C, 0x0D, 0xCD,
    0x0F, 0xCF, 0xCE, 0x0E, 0x0A, 0xCA, 0xCB, 0x0B, 0xC9, 0x09,
    0x08, 0xC8, 0xD8, 0x18, 0x19, 0xD9, 0x1B, 0xDB, 0xDA, 0x1A,
    0x1E, 0xDE, 0xDF, 0x1F, 0xDD, 0x1D, 0x1C, 0xDC, 0x14, 0xD4,
    0xD5, 0x15, 0xD7, 0x17, 0x16, 0xD6, 0xD2, 0x12, 0x13, 0xD3,
    0x11, 0xD1, 0xD0, 0x10, 0xF0, 0x30, 0x31, 0xF1, 0x33, 0xF3,
    0xF2, 0x32, 0x36, 0xF6, 0xF7, 0x37, 0xF5, 0x35, 0x34, 0xF4,
    0x3C, 0xFC, 0xFD, 0x3D, 0xFF, 0x3F, 0x3E, 0xFE, 0xFA, 0x3A,
    0x3B, 0xFB, 0x39, 0xF9, 0xF8, 0x38, 0x28, 0xE8, 0xE9, 0x29,
    0xEB, 0x2B, 0x2A, 0xEA, 0xEE, 0x2E, 0x2F, 0xEF, 0x2D, 0xED,
    0xEC, 0x2C, 0xE4, 0x24, 0x25, 0xE5, 0x27, 0xE7, 0xE6, 0x26,
    0x22, 0xE2, 0xE3, 0x23, 0xE1, 0x21, 0x20, 0xE0, 0xA0, 0x60,
    0x61, 0xA1, 0x63, 0xA3, 0xA2, 0x62, 0x66, 0xA6, 0xA7, 0x67,
    0xA5, 0x65, 0x64, 0xA4, 0x6C, 0xAC, 0xAD, 0x6D, 0xAF, 0x6F,
    0x6E, 0xAE, 0xAA, 0x6A, 0x6B, 0xAB, 0x69, 0xA9, 0xA8, 0x68,
    0x78, 0xB8, 0xB9, 0x79, 0xBB, 0x7B, 0x7A, 0xBA, 0xBE, 0x7E,
    0x7F, 0xBF, 0x7D, 0xBD, 0xBC, 0x7C, 0xB4, 0x74, 0x75, 0xB5,
    0x77, 0xB7, 0xB6, 0x76, 0x72, 0xB2, 0xB3, 0x73, 0xB1, 0x71,
    0x70, 0xB0, 0x50, 0x90, 0x91, 0x51, 0x93, 0x53, 0x52, 0x92,
    0x96, 0x56, 0x57, 0x97, 0x55, 0x95, 0x94, 0x54, 0x9C, 0x5C,
    0x5D, 0x9D, 0x5F, 0x9F, 0x9E, 0x5E, 0x5A, 0x9A, 0x9B, 0x5B,
    0x99, 0x59, 0x58, 0x98, 0x88, 0x48, 0x49, 0x89, 0x4B, 0x8B,
    0x8A, 0x4A, 0x4E, 0x8E, 0x8F, 0x4F, 0x8D, 0x4D, 0x4C, 0x8C,
    0x44, 0x84, 0x85, 0x45, 0x87, 0x47, 0x46, 0x86, 0x82, 0x42,
    0x43, 0x83, 0x41, 0x81, 0x80, 0x40
};

/****************************************************手敲*************************************************************/
/* CRC16 校验 */
uint16_t CRC_16_MODBUS(uint8_t *data, uint8_t len, uint8_t flag)
{
    if (flag)
    {
        /* 按位循环法 */
        uint16_t crc = 0xffff;
        while (len--)
        {
            crc ^= *data++;
            for (int i = 0; i < 8; i++)
            {
                if (crc & 1)
                    crc = (crc >> 1) ^ 0xA001;
                else
                    crc >>= 1;
            }
        }
        return crc;
    }
    else
    {
        /* 查表法 */
        uint8_t CRCL = 0xff;
        uint8_t CRCH = 0xff;
        uint8_t index = 0;
        while (len--)
        {
            index = CRCL ^ *data++;
            CRCL = CRCH ^ auchCRCHi[index];
            CRCH = auchCRCLo[index];
        }
        return (CRCH << 8) | CRCL;
    }
}

/* 结构体数组 多从机配置：地址、功能码、起始寄存器、寄存器数量、错误计数、数据 */
ModDev_t dev_list[] = {
    {0x01, 0x03, 0x0001, 2, 0, 0, 0, 0}, // 1#温湿度
    {0x02, 0x03, 0x0002, 2, 0, 0, 0, 0}  // 2#光照
};
uint8_t dev_cnt = sizeof(dev_list) / sizeof(ModDev_t);
#define MAX_ERR_CNT 5   // 连续失败 5 次临时跳过该设备

/**
 * @brief 通用 Modbus03 读寄存器   采集数据
 * @param addr: 从机地址  func: 功能码  reg: 起始地址  regcnt: 寄存器数量  buf: 接收缓存
 * @retval 0 失败 1 成功
 */
uint8_t Modbus_Read_Dev(uint8_t addr, uint8_t func, uint16_t reg, uint16_t regcnt, uint16_t *buf)
{
    uint8_t tx[8];
    uint8_t rx[32];
    uint8_t count = 0;
    uint16_t crc = 0;

    /* 组问询帧 */
    tx[0] = addr;
    tx[1] = func;
    tx[2] = (reg >> 8) & 0xFF;
    tx[3] = reg & 0xFF;
    tx[4] = (regcnt >> 8) & 0xFF;
    tx[5] = regcnt & 0xFF;

    crc = CRC_16_MODBUS(tx, 6, 0);
    tx[6] = crc & 0xFF;
    tx[7] = (crc >> 8) & 0xFF;

    memset(rx, 0, sizeof(rx));

    /* 清除 UART 错误标志和残留数据 */
    __HAL_UART_CLEAR_OREFLAG(&huart2);
    volatile uint32_t tmp = huart2.Instance->DR;  // 读 DR 清 RXNE
    (void)tmp;

    RS485_TX;
    if (HAL_UART_Transmit(&huart2, tx, 8, 1000) != HAL_OK)
    {
        RS485_RX;
        return 0;
    }
    /* 删除原 vTaskDelay(1); 发送完立即切换接收 */
    RS485_RX;

    uint32_t start = HAL_GetTick();
    uint32_t upd = HAL_GetTick();
    count = 0;

    /* 轮询接收（建议后续改为中断/DMA，当前使用 taskYIELD 降低丢包） */
    while (1)
    {
        if (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_RXNE))
        {
            uint8_t ch = (uint8_t)(huart2.Instance->DR & 0xFF);
            if (count < sizeof(rx)) rx[count++] = ch;
            upd = HAL_GetTick();
        }
        if (count > 0 && (HAL_GetTick() - upd) > 50) break;  // 帧间隔 3.5 字符
        if (HAL_GetTick() - start > 800) break;               // 绝对超时
        taskYIELD();  // 替代 vTaskDelay(1)，让出 CPU 但不固定延时
    }

    /* 长度安全检查：最小帧为 地址 + 功能码 + 字节数 + 2 字节 CRC */
    if (count < 4) return 0;

    uint8_t data_len = rx[2];  // 数据字节数
    /* 校验总长度、地址、功能码、数据长度 */
    if (count != (3 + data_len + 2) || rx[0] != addr || rx[1] != func || data_len != regcnt * 2)
        return 0;

    uint16_t crc_calc = CRC_16_MODBUS(rx, count - 2, 1);
    if ((rx[count - 2] == (crc_calc & 0xFF)) && (rx[count - 1] == ((crc_calc >> 8) & 0xFF)))
    {
        /* 数据转存到 buf */
        for (uint8_t i = 0; i < regcnt; i++)
        {
            buf[i] = (rx[3 + i * 2] << 8) | rx[4 + i * 2];
        }
        return 1;
    }
    return 0;
}

void Modbus_Poll_AllDev(void)
{
    uint16_t tmp_buf[8];
    for (uint8_t i = 0; i < dev_cnt; i++)
    {
        ModDev_t *p = &dev_list[i];
        memset(tmp_buf, 0, sizeof(tmp_buf));

        /* 连续错误超限，跳过本次轮询（原注释保留，可根据需求启用） */
        // if (p->err_cnt >= MAX_ERR_CNT) continue;

        if (Modbus_Read_Dev(p->dev_addr, p->func, p->reg_start, p->reg_num, tmp_buf) == 1)
        {
            if (p->err_cnt >= MAX_ERR_CNT)
            {
                char log_buf[64];
                snprintf(log_buf, sizeof(log_buf), "modbus 0x%02X back online", p->dev_addr);
                Log_Write(LOG_INFO, log_buf);
            }
            p->err_cnt = 0;
            p->valid = 1;   // 成功读到过：数据才允许上云

            /* 根据从机地址解析数据 */
            switch (p->dev_addr)
            {
            case 0x01: // 温湿度
                p->val1 = tmp_buf[0] / 10.0f;
                p->val2 = tmp_buf[1] / 10.0f;
                break;
            case 0x02: // 光照
                p->val1 = ((uint32_t)tmp_buf[0] << 16 | tmp_buf[1]) / 1000.0f;  // 浮点除法
                p->val2 = 0;
                break;
            default:
                break;
            }
        }
        else
        {
            p->err_cnt++;
            if (p->err_cnt == MAX_ERR_CNT)
            {
                p->valid = 0;  // 离线后清除有效标志，避免上报过期数据
                char log_buf[64];
                snprintf(log_buf, sizeof(log_buf), "modbus 0x%02X offline", p->dev_addr);
                Log_Write(LOG_ERROR, log_buf);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(30)); // 单从机间隔防总线干扰（重点时间不能过大）
    }
}

/* 将采集到的数据赋值给结构体 然后写入队列 */
extern QueueHandle_t lvgl_data_queue;

void Modbus_Upload_Json(void)
{
    Data_t uidata;

    uidata.temp  = dev_list[0].val1;
    uidata.shi   = dev_list[0].val2;
    uidata.light = dev_list[1].val1;
    uidata.valid = (uint8_t)((dev_list[0].valid ? 0x1 : 0) |
                             (dev_list[1].valid ? 0x2 : 0));
    xQueueOverwrite(data_queue, &uidata);
    xQueueOverwrite(lvgl_data_queue, &uidata);   /* UI 离线也要刷新，不再依赖云端在线 */
}

void MQTT_SendAllDev_NoWait(void)
{
    Data_t UIdat;
    char payload[256];

    UIdat.temp  = dev_list[0].val1;
    UIdat.shi   = dev_list[0].val2;
    UIdat.light = dev_list[1].val1;
    UIdat.valid = (uint8_t)((dev_list[0].valid ? 0x1 : 0) |
                            (dev_list[1].valid ? 0x2 : 0));

    if (UIdat.valid == 0)
        return;   /* 两台设备一次都没读到过：不上报全 0 假数据 */

    cloud_build_payload(payload, sizeof(payload), &UIdat);
    cloud_publish(payload);                     /* 云端上报（topic/QoS/格式由 profile 决定） */
    vTaskDelay(10);
}

/*
 * 注意：以下 Modbus_Get_Temp 函数与 Modbus_Read_Dev 功能重复，
 * 建议直接使用 Modbus_Read_Dev 替代。此处保留仅为兼容旧代码，并已修复长度检查等问题。
 */
int Modbus_Get_Temp(float *temp, float *shi)
{
    uint8_t tx[8];
    uint8_t rx[20];
    uint8_t count = 0;
    uint16_t crc = 0;

    /* 问询帧 */
    tx[0] = 0x01;
    tx[1] = 0x03;
    tx[2] = 0x00;
    tx[3] = 0x01;
    tx[4] = 0x00;
    tx[5] = 0x02;

    /* 小端排序 低字节在前 */
    crc = CRC_16_MODBUS(tx, 6, 0);
    tx[6] = crc & 0xff;
    tx[7] = (crc >> 8) & 0xff;

    memset(rx, 0, sizeof(rx));

    /* 清除 UART 错误标志和残留数据 */
    __HAL_UART_CLEAR_OREFLAG(&huart2);
    volatile uint32_t tmp = huart2.Instance->DR;
    (void)tmp;

    RS485_TX;
    if (HAL_UART_Transmit(&huart2, tx, sizeof(tx), 1000) != HAL_OK)
    {
        RS485_RX;
        return 0;
    }
    RS485_RX;  // 发送完立即切换接收

    uint32_t start_time = HAL_GetTick();
    uint32_t up_time = HAL_GetTick();

    while (1)
    {
        if (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_RXNE))
        {
            uint8_t ch = (uint8_t)(huart2.Instance->DR & 0xff);
            if (count < sizeof(rx))
            {
                rx[count++] = ch;
                up_time = HAL_GetTick();
            }
        }
        if (count > 0 && (HAL_GetTick() - up_time) > 50) break;  // 帧间隔
        if (HAL_GetTick() - start_time > 800) break;              // 绝对超时
        taskYIELD();
    }

    if (count < 4) return 0;  // 长度安全检查

    uint16_t crc_re = CRC_16_MODBUS(rx, count - 2, 1);
    if (count == 9 && rx[0] == 0x01 && rx[1] == 0x03 && rx[2] == 0x04 &&
        rx[7] == (crc_re & 0xff) && rx[8] == ((crc_re >> 8) & 0xff))
    {
        uint16_t t = (rx[3] << 8) | rx[4];
        uint16_t s = (rx[5] << 8) | rx[6];
        *temp = t / 10.0f;
        *shi = s / 10.0f;
        return 1;
    }
    return 0;
}