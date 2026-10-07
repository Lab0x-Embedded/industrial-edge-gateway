#!/usr/bin/env python3
"""Modbus RTU 从机模拟器 —— 模拟网关现场层的两台 RS485 变送器

模拟对象（与 User/model/modbus.c 的 dev_list[] 对应）：
  从机 0x01  温湿度  保持寄存器 0x0001 ×2：温度 = reg0/10 ℃，湿度 = reg1/10 %
  从机 0x02  光照    保持寄存器 0x0002 ×2：光照 = (reg0<<16|reg1)/1000 lx

数值会随机缓变，方便观察 UI 曲线和云端上报。

用法（仅依赖 pyserial：pip install pyserial）：
  python3 tools/modbus_sim.py /dev/ttyUSB0
  python3 tools/modbus_sim.py COM5 --baud 9600

接线：
  网关 RS485 A/B  ←→  USB-RS485 适配器 A/B（有共地更好）；
  若一直超时/CRC 错，先把 A/B 对调试试。

预期效果：脚本每秒打印网关的问询帧；网关屏幕数值每 800ms 刷新，
MQTT 在线时云端收到 {"temp":..,"humi":..,"light":..}。
"""
import argparse
import random
import struct
import sys
import time

import serial


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


class Sensors:
    """带随机缓变的传感器状态"""

    def __init__(self):
        self.temp = 25.6    # ℃
        self.humi = 58.0    # %RH
        self.light = 12345  # lx

    def drift(self):
        self.temp = min(max(self.temp + random.uniform(-0.08, 0.08), -10), 60)
        self.humi = min(max(self.humi + random.uniform(-0.4, 0.4), 0), 100)
        self.light = min(max(self.light + random.uniform(-150, 150), 0), 20000)

    def regs_for(self, addr, start, count):
        """返回从机 addr 在 [start, start+count) 的寄存器值，不匹配返回 None"""
        if addr == 0x01 and start == 0x0001 and count == 2:
            return [int(round(self.temp * 10)) & 0xFFFF,
                    int(round(self.humi * 10)) & 0xFFFF]
        if addr == 0x02 and start == 0x0002 and count == 2:
            raw = int(self.light) * 1000
            return [(raw >> 16) & 0xFFFF, raw & 0xFFFF]
        return None


def main():
    ap = argparse.ArgumentParser(description="Modbus RTU 温湿度/光照从机模拟器")
    ap.add_argument("port", help="串口，如 COM5 / /dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=9600)
    args = ap.parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, bytesize=8, parity="N",
                            stopbits=1, timeout=0.2)
    except serial.SerialException as e:
        sys.exit(f"打开串口失败: {e}")

    sensors = Sensors()
    print(f"监听 {args.port} @ {args.baud} 8N1，模拟从机 0x01(温湿度) / 0x02(光照)，Ctrl+C 退出")

    try:
        while True:
            head = ser.read(1)
            if not head:
                sensors.drift()
                continue
            if head[0] not in (0x01, 0x02):
                continue                      # 不是本总线的地址，丢弃

            rest = ser.read(7)
            if len(rest) < 7 or rest[0] != 0x03:
                continue                      # 只支持 03 读保持寄存器
            frame = head + rest
            if crc16(frame[:6]) != struct.unpack("<H", frame[6:8])[0]:
                print("← CRC 错误，丢弃（检查 A/B 是否接反）")
                continue

            addr = frame[0]
            start, count = struct.unpack(">HH", frame[2:6])
            vals = sensors.regs_for(addr, start, count)
            if vals is None:
                continue                      # 非本机寄存器，静默不答（网关按超时处理）

            body = bytes([addr, 0x03, count * 2])
            body += b"".join(struct.pack(">H", v) for v in vals)
            ser.write(body + struct.pack("<H", crc16(body)))

            if addr == 0x01:
                print(f"← 0x01 问询 → 温度 {vals[0]/10:.1f}℃  湿度 {vals[1]/10:.1f}%")
            else:
                lux = (vals[0] << 16 | vals[1]) / 1000
                print(f"← 0x02 问询 → 光照 {lux:.0f} lx")
            sensors.drift()
            time.sleep(0.005)                 # 模拟从机处理耗时
    except KeyboardInterrupt:
        print("\n退出")


if __name__ == "__main__":
    main()
