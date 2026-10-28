# ============================================================
#  工业边缘网关 Makefile（包装 CMake + Ninja + 烧录 + 串口）
#
#  本仓库有**两个独立固件**，分开构建、分开烧录：
#    网关  industrial-edge-gateway  → 主板（LCD/触摸/RS485 主机/ESP8266）
#    从机  modbus_slave             → 第二块板（Modbus 从机模拟，含 DHT11）
#    从机是独立 CMake 工程（ModbusSlave/ 有自己的 CMakePresets.json），
#    为什么要分开：两块板改代码/编译的节奏完全不同，混在一起的话
#    改一行从机代码也要过一遍 LVGL/FreeRTOS 的编译检查。
#
#  为什么这么写（都是踩过的坑）：
#   1) 必须走 `cmake --preset Debug`：工具链写在 CMakePresets.json 的
#      "toolchainFile" 里。手工 `cmake -S . -B build/Debug -G Ninja` 少写
#      -DCMAKE_TOOLCHAIN_FILE 时，CMake 会拿宿主编译器（AppleClang）配置，
#      而且 configure 还会"成功"，到 build 才炸：
#         startup_stm32f407xx.s:27:3: error: unknown directive
#   2) preset 每次都会重新 configure，顺带解决根 CMakeLists 里
#      `file(GLOB_RECURSE USER_SOURCES ... User/*.c)` 的坑：
#      新增源文件后不重新 configure，它不会被编进工程（改了等于没改）。
#   3) 工具链是 STM32Cube 里那份（不是 homebrew 的，缺 newlib）。CMake
#      侧路径写死在 cmake/gcc-arm-none-eabi.cmake 的 ST_TOOLCHAIN_DIR；
#      下面 export PATH 是给 Makefile 自己调 objcopy/size 用的。
#      注意：STM32Cube 升级 bundle 后版本号路径会变，两处要一起改。
# ============================================================

# Cube bundle 里的 GCC（带 newlib）；与 cmake/gcc-arm-none-eabi.cmake 保持一致
CUBE_BIN  := $(HOME)/Library/Application Support/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin
export PATH := $(CUBE_BIN):$(PATH)

# ---------- 配置 ----------
# BUILD_TYPE: Debug | Release（Release 用 -O3，体积/速度都更好）
# 注意：这一行**不能写行内注释** —— make 会把 `#` 前的空格并进变量值，
# 于是 BUILD_DIR 变成 "build/Debug   "，ELF/HEX/BIN 全被空格拆成两个参数
# （症状：make bin/hex/flash/size 报 objcopy 用法，或烧进旧文件）。
BUILD_TYPE ?= Debug
BUILD_DIR  ?= build/$(BUILD_TYPE)
PROJECT    ?= industrial-edge-gateway
ELF        := $(BUILD_DIR)/$(PROJECT).elf
HEX        := $(BUILD_DIR)/$(PROJECT).hex
BIN        := $(BUILD_DIR)/$(PROJECT).bin

# DBG: 调试打印总开关（1=开 0=关）。关掉时 dbg_print 变空宏，调用点、格式串、
# UART5 发送与互斥锁全部不进固件（省 ~1.2KB FLASH），机制见 main.h。
# 用法：make DBG=0 / make flash DBG=0 / make release DBG=0
# 注意：不传 DBG 时永远按 1 重新配置 —— `make DBG=0` 之后的下一次裸 `make`
# 会自动把日志开回来，这是有意的：开关状态跟构建走，不藏在缓存里。
DBG ?= 1

# 从机（独立 CMake 工程，preset 在 ModbusSlave/CMakePresets.json）
SLAVE_DIR   := ModbusSlave
SLAVE_ELF   := $(SLAVE_DIR)/build/$(BUILD_TYPE)/modbus_slave.elf
SLAVE_BIN   := $(SLAVE_DIR)/build/$(BUILD_TYPE)/modbus_slave.bin

# 烧录工具：openocd（ST-Link）| pyocd（本套件配套的 CMSIS-DAP 就用它）| st-flash | STM32_Programmer_CLI
FLASH_TOOL     ?= openocd
# 换 J-Link 改成 interface/jlink.cfg（并加 -c "transport select swd"）
OPENOCD_IF     ?= interface/stlink.cfg
OPENOCD_TARGET ?= target/stm32f4x.cfg
# pyocd 的目标芯片：本板是 STM32F407ZGT6
PYOCD_TARGET   ?= stm32f407zg

# 串口监视：当前固件没有 printf 通道（UART4=ESP8266、USART2=RS485），
# 这个 target 留给以后接 USB-TTL 做调试输出用
SERIAL_PORT ?= /dev/cu.usbserial-0001
SERIAL_BAUD ?= 115200

.PHONY: all build slave release hex bin size size-slave flash flash-slave \
        reset openocd serial clean clean-slave distclean rebuild help

all: build

# ---------- 编译：网关 ----------
build:
	@echo ">>> configure + build 网关（cmake --preset $(BUILD_TYPE)，DBG_PRINT_ENABLE=$(DBG)）"
	@cmake --preset $(BUILD_TYPE) -DDBG_PRINT_ENABLE=$(DBG)
	@cmake --build --preset $(BUILD_TYPE)

release:
	@$(MAKE) build BUILD_TYPE=Release

# ---------- 编译：Modbus 从机（独立工程，独立 build 目录） ----------
slave:
	@echo ">>> configure + build 从机（ModbusSlave/，cmake --preset $(BUILD_TYPE)）"
	@cd $(SLAVE_DIR) && cmake --preset $(BUILD_TYPE)
	@cd $(SLAVE_DIR) && cmake --build --preset $(BUILD_TYPE)

hex: build
	@arm-none-eabi-objcopy -O ihex $(ELF) $(HEX)
	@echo ">>> $(HEX)"

bin: build
	@arm-none-eabi-objcopy -O binary $(ELF) $(BIN)
	@echo ">>> $(BIN)"

size: build
	@arm-none-eabi-size $(ELF)

size-slave: slave
	@arm-none-eabi-size $(SLAVE_ELF)

# ---------- 烧录 ----------
# flash 依赖 bin/hex：原来只依赖 build，recipe 里却用 $(BIN)/$(HEX)，
# 轻则报文件不存在，重则把上一次留下的旧 .bin 烧进去。
flash: bin hex
	@echo ">>> 烧录网关 via $(FLASH_TOOL) ..."
ifeq ($(FLASH_TOOL),openocd)
	openocd -f $(OPENOCD_IF) -f $(OPENOCD_TARGET) \
		-c "program $(ELF) verify reset exit"
else ifeq ($(FLASH_TOOL),pyocd)
	pyocd flash -t $(PYOCD_TARGET) $(ELF) --format elf
else ifeq ($(FLASH_TOOL),st-flash)
	st-flash --reset write $(BIN) 0x08000000
else ifeq ($(FLASH_TOOL),STM32_Programmer_CLI)
	STM32_Programmer_CLI -c port=SWD -w $(HEX) -v -rst
else
	@echo "Unknown FLASH_TOOL: $(FLASH_TOOL)" && exit 1
endif

# 从机板烧录：注意先接好第二块板的调试器（make flash-slave FLASH_TOOL=pyocd）
flash-slave: slave
	@echo ">>> 烧录从机 via $(FLASH_TOOL) ..."
ifeq ($(FLASH_TOOL),openocd)
	openocd -f $(OPENOCD_IF) -f $(OPENOCD_TARGET) \
		-c "program $(SLAVE_ELF) verify reset exit"
else ifeq ($(FLASH_TOOL),pyocd)
	pyocd flash -t $(PYOCD_TARGET) $(SLAVE_ELF) --format elf
else ifeq ($(FLASH_TOOL),st-flash)
	arm-none-eabi-objcopy -O binary $(SLAVE_ELF) $(SLAVE_BIN)
	st-flash --reset write $(SLAVE_BIN) 0x08000000
else ifeq ($(FLASH_TOOL),STM32_Programmer_CLI)
	arm-none-eabi-objcopy -O ihex $(SLAVE_ELF) $(SLAVE_DIR)/build/$(BUILD_TYPE)/modbus_slave.hex
	STM32_Programmer_CLI -c port=SWD -w $(SLAVE_DIR)/build/$(BUILD_TYPE)/modbus_slave.hex -v -rst
else
	@echo "Unknown FLASH_TOOL: $(FLASH_TOOL)" && exit 1
endif

reset:
	openocd -f $(OPENOCD_IF) -f $(OPENOCD_TARGET) -c "init" -c "reset run" -c "shutdown"

openocd:
	openocd -f $(OPENOCD_IF) -f $(OPENOCD_TARGET)

# ---------- 串口监视 ----------
serial:
	@echo ">>> $(SERIAL_PORT) @ $(SERIAL_BAUD)   退出：先按 Ctrl-A 再按 \\"
	@screen $(SERIAL_PORT) $(SERIAL_BAUD)

# ---------- 清理 ----------
clean:
	@rm -rf $(BUILD_DIR)

clean-slave:
	@rm -rf $(SLAVE_DIR)/build

distclean:
	@rm -rf build $(SLAVE_DIR)/build

rebuild: clean build

# ---------- 帮助 ----------
help:
	@echo "两个独立固件：网关（本目录）+ Modbus 从机（ModbusSlave/，独立 CMake 工程）"
	@echo ""
	@echo "Available targets:"
	@echo "  make            - 配置 + 编译网关（PRESET=$(BUILD_TYPE)）"
	@echo "  make build      - 同上"
	@echo "  make slave      - 配置 + 编译 Modbus 从机（独立构建目录）"
	@echo "  make release    - Release(-O3) 构建网关（= make build BUILD_TYPE=Release）"
	@echo "  make flash      - 编译网关 + bin/hex + 烧录（$(FLASH_TOOL)）"
	@echo "  make flash-slave- 编译从机 + 烧录（第二块板；CMSIS-DAP 用 FLASH_TOOL=pyocd）"
	@echo "  make reset      - 复位并运行"
	@echo "  make openocd    - 只起 openocd 服务（给 GDB / VS Code 用）"
	@echo "  make serial     - 打开串口 $(SERIAL_PORT) @ $(SERIAL_BAUD)"
	@echo "  make size       - 网关 Flash/RAM 占用"
	@echo "  make size-slave - 从机 Flash/RAM 占用"
	@echo "  make hex / bin  - 只生成网关 hex / bin"
	@echo "  make clean      - 删网关 $(BUILD_DIR)"
	@echo "  make clean-slave- 删从机 $(SLAVE_DIR)/build"
	@echo "  make distclean  - 两个 build 全删"
	@echo ""
	@echo "可覆盖变量："
	@echo "  FLASH_TOOL=openocd|pyocd|st-flash|STM32_Programmer_CLI"
	@echo "  OPENOCD_IF=interface/stlink.cfg | interface/jlink.cfg"
	@echo "  PYOCD_TARGET=stm32f407zg"
	@echo "  BUILD_TYPE=Debug|Release（make slave/flash-slave 同样生效）"
	@echo "  DBG=1|0        - 调试打印开关，0 = 整体裁掉 UART5 日志（make flash DBG=0 可直接烧无日志固件）"
	@echo "  SERIAL_PORT=/dev/cu.usbserial-0001"
