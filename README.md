# industrial-edge-gateway（STM32F407ZGT6 移植版）

工业边缘网关（FreeRTOS + LVGL 8.3）：Modbus 传感器采集 → RS485、ESP8266 上云 MQTT、W25Q128 断网缓存与日志、看门狗、LVGL 触摸界面。
原项目为 STM32F411 + SPI 屏，本工程已完整移植到 **启明欣欣 STM32F407ZGT6 高配版 V4.1 + 2.8 寸 ILI9341 并口屏（XPT2046 触摸）**。

- CubeMX 工程：`industrial-edge-gateway.ioc`（CubeMX 6.18.1 / FW_F4 V1.28.3，工具链 CMake+GCC）
- 编译产物：`build/industrial-edge-gateway.elf / .bin / .hex`
- 资源占用：Flash 367KB / 1MB（36%），RAM 90KB / 128KB（70%）


## 使用方法

1. 使用 STM32CubeMX 导入 `industrial-edge-gateway.ioc`，CubeMX 版本为 6.18.1。
2. 点击 “Generate Code”，基于 CMake 生成工程。根据你使用的编辑器选择对应的 Toolchain，默认是 CMake + GCC。
3. 配置 VS Code 运行工程：安装最新版 VS Code，并安装 [STM32CubeMX 扩展](https://marketplace.visualstudio.com/items?itemName=stmicroelectronics.stm32-vscode-extension)。
4. 根据右下角提示进行配置，通常一路点击“是”即可。烧录时，使用 Debug 模式并创建 `launch.json` 文件。

## 目录结构

```
Core/                  CubeMX 生成（时钟168M、FSMC、UART4+DMA、USART2、SPI1、TIM3、RTC、IWDG、FreeRTOS）
User/
  code/                应用任务 + LCD/触摸驱动（FSMC 并口版，含补全的 LCD_WritePixels / TP_Get_Calibrated）
  ESP8266/             AT 指令 + MQTT（(已删除)），串口已改 huart4(PA0/PA1) + DMA1 S2/S4
  model/               Modbus RTU 主站（USART2 PA2/PA3，方向脚 PG6）
  SPI_Flash/           W25Q128（SPI1 PB3/4/5，CS=PG8）；日志分区 0~4MB，补传缓存 4~8MB
  Lvgl/                LVGL 8.3 + 界面 + porting（LV_COLOR_16_SWAP=0，并口屏）
```

## 构建（macOS，GCC）

```bash
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake -DCMAKE_BUILD_TYPE=Debug
cmake --build build
# 产物 build/industrial-edge-gateway.elf；如需 bin/hex：
arm-none-eabi-objcopy -O binary build/industrial-edge-gateway.elf build/industrial-edge-gateway.bin
```

工具链用 CubeIDE/CubeMX 自带的 GNU Tools for STM32（`~/Library/Application Support/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2`），`cmake/gcc-arm-none-eabi.cmake` 已指向它。Homebrew 的 arm-none-eabi-gcc 缺 newlib 头文件，不可用。

## 烧录

- **STM32CubeProgrammer**：连 SWD（板载 JTAG/SWD 座或 CMSIS-DAP），刷 `build/industrial-edge-gateway.hex`。
- **Keil 用户**：也可用 CubeMX 把 Toolchain 改为 MDK-ARM 重新生成工程后编译（应用代码都在 User CODE 区和 User/ 目录，不会丢）。

## 引脚对照（启明欣欣 V4.1）

| 功能 | 引脚 |
|---|---|
| LCD FSMC 16 位并口 | NE4=PG12，RS=A12=PG2，NOE=PD4，NWE=PD5，D0~D15=PD14/15、PD0/1、PD8/9/10、PE7~PE15 |
| LCD 背光 | PF10（高=亮） |
| XPT2046 触摸 | CLK=PA5，MOSI=PB1，MISO=PB2，CS=PB0，PEN=PF11 |
| ESP8266（WIFI 座） | UART4：TX=PA0，RX=PA1，115200；RX=DMA1_Stream2，TX=DMA1_Stream4 |
| RS485 | USART2：PA2/PA3，9600；方向 PG6（高发低收） |
| W25Q128 | SPI1：PB3/4/5，CS=PG8 |
| 指示灯 | LED0=PE3（MQTT 连接），LED1=PE4（缓存中），LED2=PG9（补传中）——低电平亮 |
| LVGL 心跳 | TIM3 1ms 中断；HAL 时基 = TIM4（RTOS 下不用 SysTick） |

## 上电验证顺序

1. 屏点亮、显示 LVGL 主界面；10 秒无操作息屏，触碰唤醒。
2. 触摸可点按界面。
3. 串口1(RS232 座) 115200 看系统日志（storage.c 的调试打印走此口）。
4. 接 Modbus 温湿度/光照传感器（RS485 端子），界面数据刷新。
5. WIFI 座插 ESP8266，MQTT 连上后 LED0 常亮；拔网线测断网缓存、恢复后自动补传（LED2 闪烁）。

## 已知注意点

- **RTC 用 LSI**（板上 LSE 32.768K 未确认焊接）；日志时间戳有漂移。要换 LSE：CubeMX → RTC → 时钟源选 LSE，重新生成。
- **触摸映射为出厂粗校准**（线性 200~3900 → 320×240）。若边缘偏差大，改 `User/bsp/touch.c` 里 `TP_Get_Calibrated` 的映射系数。
- **W25Q128 分区表**沿用原 W25Q64 布局（用满 8MB），无冲突；JEDEC ID 0xEF4018。
- ESP8266 需为标准 AT 固件（厂家资料包有刷机教程与固件）。
- FreeRTOS heap 15KB、LVGL 缓冲 320×30 行，F407 资源富余；如加界面元素可在 `User/gui/lv_conf.h` 调 `LV_MEM_SIZE`。
- 息屏逻辑：10 秒无触摸/数据 → 背光灭（PF10 拉低）；有数据或 MQTT 状态变化自动亮屏。
