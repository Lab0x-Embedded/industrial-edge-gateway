#!/usr/bin/env python3
"""Modbus RTU 主机测试 —— 直接点名从机板（绕开网关）

用途：网关收到的温湿度/光照一直是 0 时，用它判断问题在网关还是在从机。
  从机 0x01  保持寄存器 0x0001 ×2：温度 = reg0/10 ℃，湿度 = reg1/10 %
  从机 0x02  保持寄存器 0x0002 ×2：光照 = (reg0<<16|reg1)/1000 lx

用法（需 pyserial）：
  python3 tools/modbus_master_test.py /dev/cu.wchusbserial1110
  python3 tools/modbus_master_test.py COM5 -n 5

判读：
  有应答且数值非 0   -> 从机正常，问题在网关侧（少见）
  有应答但全是 0     -> 从机在跑但传感器没数据（查 DHT11 是否接 PB9）
  无应答（超时）     -> 从机没跑/没烧固件/A-B 线没接对
"""
import argparse
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


def poll(ser, addr, start, count):
    req = bytes([addr, 0x03]) + struct.pack(">HH", start, count)
    req += struct.pack("<H", crc16(req))
    ser.reset_input_buffer()
    ser.write(req)
    resp = ser.read(3 + count * 2 + 2)
    if len(resp) < 5:
        return None, "无应答/超时"
    if crc16(resp[:-2]) != struct.unpack("<H", resp[-2:])[0]:
        return None, "CRC 错（检查 A/B 是否接反）: " + resp.hex(" ")
    if resp[0] != addr or resp[1] != 0x03:
        return None, "应答异常: " + resp.hex(" ")
    vals = [struct.unpack(">H", resp[3 + i * 2:5 + i * 2])[0] for i in range(count)]
    return vals, resp.hex(" ")


def main():
    ap = argparse.ArgumentParser(description="直接点名从机板的 Modbus 主机测试")
    ap.add_argument("port", help="USB-RS485 适配器串口")
    ap.add_argument("--baud", type=int, default=9600)
    ap.add_argument("-n", type=int, default=8, help="每个从机轮询次数")
    args = ap.parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, bytesize=8, parity="N",
                            stopbits=1, timeout=1.0)
    except serial.SerialException as e:
        sys.exit(f"打开串口失败: {e}")

    print(f"测试 {args.port} @ {args.baud} 8N1，各轮询 {args.n} 次（间隔 1s）\n")
    cases = ((0x01, "温湿度", lambda v: f"{v[0]/10:.1f}C / {v[1]/10:.1f}%"),
             (0x02, "光照", lambda v: f"{((v[0]<<16)|v[1])/1000:.0f} lx"))
    for addr, name, fmt in cases:
        for i in range(args.n):
            vals, raw = poll(ser, addr, 0x0001 if addr == 0x01 else 0x0002, 2)
            if vals is None:
                print(f"[0x{addr:02X}] #{i+1} {name}: X {raw}")
            else:
                print(f"[0x{addr:02X}] #{i+1} {name}: OK {fmt(vals)}   ({raw})")
            time.sleep(1)


if __name__ == "__main__":
    main()
