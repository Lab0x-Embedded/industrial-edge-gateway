# industrial-edge-gateway（STM32F407ZGT6 移植版）

工业边缘网关（FreeRTOS + LVGL 8.3）：Modbus RTU 从机采集 → RS485、ESP8266 上云 MQTT（E2 / OneNET，profile 切换）、W25Q128 断网缓存与系统日志、看门狗、LVGL 触摸界面。
原项目为 STM32F411 + SPI 屏，本工程已完整移植到 **启明欣欣 STM32F407ZGT6 高配版 V4.1 + 2.8 寸 ILI9341 并口屏（XPT2046 触摸）**。

- CubeMX 工程：`industrial-edge-gateway.ioc`（CubeMX 6.18.1 / FW_F4 V1.28.3，工具链 CMake+GCC）
- 配套从机板：`ModbusSlave/`（第二块 F407 板模拟 0x01 温湿度[DHT11 实测] / 0x02 光照[PF3 ADC] 两台 Modbus 从机）
- 资源占用（网关）：Flash ≈390KB / 1MB（37%），RAM ≈110KB / 128KB（83.5%）；从机：Flash 27KB，RAM 2.5KB

## 目录结构

```
Core/                  CubeMX 生成（时钟168M、FSMC、UART4+DMA、USART2、UART5调试、SPI1、TIM3、TIM4时基、RTC、IWDG、FreeRTOS）
User/
  app/                 业务任务调度（tasks.c，原 Taskhandel）
  bsp/                 板级驱动：lcd.c（FSMC 并口 ILI9341）、touch.c（XPT2046 + 两点校准）
  modbus/              Modbus RTU 主站（USART2 PA2/PA3 9600，方向脚 PG6；数据有效性门控）
  net/                 cloud.c（云平台 profile 层：E2/OneNET）、esp_at.c（AT 驱动）、esp_uart.c（UART4 DMA 接收环形缓冲）
  storage/             w25qxx.c（W25Q128 SPI1 PB3/4/5，CS=PG8 驱动）、log.c（日志 0~4MB）、cache.c（断网补传缓存 4~8MB）
  gui/                 LVGL 8.3（src/）、porting/（显示/触摸移植）、ui/（界面页面）、lv_conf.h
ModbusSlave/           从机固件（独立 CMake 工程）：DHT11=PA15、光敏=PF3(ADC3_IN9)、RS485 从机 0x01/0x02
tools/                 modbus_sim.py（PC 模拟从机）、modbus_master_test.py（PC 当主机直测从机板）
docs/                  系统架构图（mermaid）、调试排查手册（18 个实战案例）
Makefile               make build / slave / flash / flash-slave / size ...（封装 CMake preset）
```

## 构建（macOS，GCC）

```bash
make            # 网关固件（= cmake --preset Debug && cmake --build --preset Debug）
make slave      # 从机固件（独立构建目录 ModbusSlave/build/）
make size       # 网关 Flash/RAM 占用；make size-slave 同理
```

工具链用 STM32Cube 自带 GNU Tools（`cmake/gcc-arm-none-eabi.cmake` 已指向），Homebrew 的 arm-none-eabi-gcc 缺 newlib 不可用。
**注意**：必须走 `cmake --preset`（工具链在 preset 里）；手工 `cmake -S . -B build` 会在 build 阶段报 startup 文件语法错误。

## 烧录

```bash
make flash                       # 网关（openocd + ST-Link，可换 FLASH_TOOL=pyocd 等）
make flash-slave                 # 从机板（ST-Link 换接从机 SWD 后）
make reset / make openocd        # 复位 / 起 gdbserver
```

- 也可用 STM32CubeProgrammer / Keil / VS Code 调试配置，应用代码都在 USER CODE 区和 User/ 目录，不会丢。
- 调试串口：**UART5 = PC12(TX)/PD2(RX)，115200**（CH340 USB-TTL），输出联网阶段、日志镜像、OneNET 回执。

## 云平台切换（cloud.h）

```c
#define CLOUD_TARGET  CLOUD_E2      /* CLOUD_E2（局域网自建平台，默认）/ CLOUD_ONENET（Studio） */
```

- **E2**：`E2_HOST / E2_PRODUCT_KEY / E2_DEVICE_ID / E2_DEVICE_SECRET`（设备需先在 E2 注册）
- **OneNET**：`ONENET_HOST(mqtts.heclouds.com) / ONENET_PRODUCT_ID / ONENET_DEVICE_ID / ONENET_TOKEN`
  （token 控制台生成，`et=` 是过期时间戳，到期设备全部掉线需重新生成）
- 上报格式：E2 = `devices/<id>/data` datapoints JSON；OneNET = `$sys/{pid}/{dev}/thing/property/post` 物模型包装格式。
  **数据有效性门控**：某设备从未成功读到过数据时，其字段不会出现在 payload 里（全 0 假数据不上云、不进断网缓存）。

## 引脚对照（启明欣欣 V4.1）

| 功能 | 引脚 |
|---|---|
| LCD FSMC 16 位并口 | NE4=PG12，RS=A12=PG2，NOE=PD4，NWE=PD5，D0~D15=PD14/15、PD0/1、PD8/9/10、PE7~PE15 |
| LCD 背光 | PF10（高=亮） |
| XPT2046 触摸 | CLK=PA5，MOSI=PB1，MISO=PB2，CS=PB0，PEN=PF11（低=按下）|
| ESP8266（WIFI 座） | UART4：TX=PA0，RX=PA1，115200；RX=DMA1_Stream2，TX=DMA1_Stream4 |
| RS485 | USART2：PA2/PA3，9600；方向 PG6（高发低收） |
| W25Q128 | SPI1：PB3/4/5，CS=PG8（日志 0~4MB，补传缓存 4~8MB，触摸校准参数 0xF00000）|
| 指示灯 | PE3（补传中/绿）、PE4（云端在线/红）、PG9（离线存储/黄），按固件写入语义高=亮 |
| LVGL 心跳 | TIM3 1ms 中断；HAL 时基 = TIM4（FreeRTOS 占用 SysTick，TIM4 在 CubeMX 里显示灰色属正常）|
| 调试串口 | UART5：PC12/PD2，115200 |

## 上电验证顺序

1. **开机校准**：首次上电（或 W25Q 无校准数据）自动出红十字校准——按左上、右下十字中心各一次；**按住屏幕开机可强制重新校准**（换板/换屏/漂移通用）。校准值存 W25Q 0xF00000，此后免校准。
2. 屏点亮显示主界面；10 秒无操作息屏，**触摸唤醒**（RS485 插拔本身不点亮，属正常）。
3. 串口终端（UART5 115200）看联网阶段输出与 `[LOG]` 日志镜像；屏幕"日志"页同内容。
4. 接从机板 RS485（A/A、B/B、共地）：屏幕温湿度/光照 800ms 刷新（DHT11 实测 + 光敏 ADC）；日志出现 `modbus back online`。
5. WIFI 座插 ESP8266：MQTT 连上后屏幕绿 ONLINE；断网测缓存（日志 WARN）、恢复后自动补传（每轮 ≤5 条）。

## 已知注意点

- **RTC 用 LSI**（板上 LSE 32.768K 未确认焊接）；日志时间戳有漂移。要换 LSE：CubeMX → RTC → 时钟源选 LSE，重新生成。
- **触摸校准**存 W25Q 0xF00000（跟主板走，不跟屏）；按住屏幕开机强制重校。
- **W25Q128 分区**：日志 0~4MB、缓存 4~8MB、参数区 0xF00000（触摸校准）；JEDEC ID 0xEF4018。`storage/w25qxx.c` 的忙等已改为 500ms 墙钟 + 必释放 CS（厂商例程的短超时会让写入静默失败）。
- **ESP8266 需标准 AT 固件**（厂家资料包有刷机教程）；重连机制 = 3 次心跳失败(30s)判离线 → 每 10s `AT+RST` 全量重连。
- **FreeRTOS heap 32KB**；LVGL 缓冲 320×30 行、内存池 48KB（`User/gui/lv_conf.h`）。
- **息屏**：10 秒无触摸且云端状态无变化 → 背光灭；触摸唤醒（唤醒那一次按压不当作点击）。
- **CubeMX 重新生成**：注意核对 `stm32f4xx_it.c` 是否保留 `TIM4_IRQHandler`（HAL 时基，缺失=开机死机白屏）；从机的 ADC 开关在 `ModbusSlave/hal/Inc` 本地 hal_conf，不受网关 .ioc 影响。
