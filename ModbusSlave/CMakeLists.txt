# ============================================================
#  Modbus 从机模拟固件 —— 独立 CMake 工程（与网关固件分开构建）
#
#  烧录到第二块启明欣欣 F407 板，通过板载 RS485 模拟
#  0x01 温湿度（真实 DHT11，DATA=PB9）/ 0x02 光照两台从机。
#
#  为什么独立而不是主工程的第二个 target：
#    网关和从机刷不同的板、改不同的代码，编译节奏完全不同；
#    混在同一个构建里，改一行从机代码也要过一遍 LVGL/FreeRTOS 的编译检查。
#  注意：
#    - 工具链文件 cmake/gcc-arm-none-eabi.cmake 用 CMAKE_SOURCE_DIR 拼
#      链接脚本路径，所以本目录下有一份 STM32F407xx_FLASH.ld 的拷贝，
#      改内存布局时两份要同步。
#    - 本工程不编译 CubeMX 的 main.c/stm32f4xx_it.c/hal_msp.c：
#      SysTick_Handler、HAL_UART_MspInit、Error_Handler 都在
#      modbus_slave_main.c 里自己提供。
# ============================================================
cmake_minimum_required(VERSION 3.22)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

project(modbus_slave C ASM)
enable_language(C ASM)

set(HAL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/STM32F4xx_HAL_Driver)
set(CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../Core)

add_executable(modbus_slave
    modbus_slave_main.c
    ${CORE_DIR}/Src/sysmem.c
    ${CORE_DIR}/Src/syscalls.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../startup_stm32f407xx.s

    # CMSIS / 系统
    ${CORE_DIR}/Src/system_stm32f4xx.c

    # 本固件用到的 HAL 驱动（链接时 --gc-sections 会裁掉没被调用的部分）
    ${HAL_DIR}/Src/stm32f4xx_hal.c
    ${HAL_DIR}/Src/stm32f4xx_hal_cortex.c
    ${HAL_DIR}/Src/stm32f4xx_hal_flash.c
    ${HAL_DIR}/Src/stm32f4xx_hal_flash_ex.c
    ${HAL_DIR}/Src/stm32f4xx_hal_flash_ramfunc.c
    ${HAL_DIR}/Src/stm32f4xx_hal_gpio.c
    ${HAL_DIR}/Src/stm32f4xx_hal_pwr.c
    ${HAL_DIR}/Src/stm32f4xx_hal_pwr_ex.c
    ${HAL_DIR}/Src/stm32f4xx_hal_rcc.c
    ${HAL_DIR}/Src/stm32f4xx_hal_rcc_ex.c
    ${HAL_DIR}/Src/stm32f4xx_hal_uart.c
)

target_include_directories(modbus_slave PRIVATE
    ${CORE_DIR}/Inc
    ${HAL_DIR}/Inc
    ${HAL_DIR}/Inc/Legacy
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/CMSIS/Device/ST/STM32F4xx/Include
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/CMSIS/Include
)

target_compile_definitions(modbus_slave PRIVATE
    USE_HAL_DRIVER
    STM32F407xx
    $<$<CONFIG:Debug>:DEBUG>
)

set_target_properties(modbus_slave PROPERTIES ADDITIONAL_CLEAN_FILES modbus_slave.map)
