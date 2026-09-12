# -*- coding: utf-8 -*-
"""抓取 ESP32-S3 串口启动日志（USB-Serial-JTAG 专用）。

用法:
    python serial_capture.py <COMx> <秒数> <输出文件> [--no-reset] [--baud N]

复位时序依据 esptool/reset.py 的 HardReset(uses_usb=True)：
    RTS=True  -> EN=LOW  芯片复位
    RTS=False -> EN=HIGH 芯片释放，正常启动应用
    DTR 必须保持 False -> IO0=HIGH，否则会进 ROM 下载模式
注意：Windows 打开串口时驱动可能默认拉高 DTR(=IO0 低)，因此打开后必须显式 setDTR(False)。
"""
import sys
import time

import serial

port = sys.argv[1]
secs = int(sys.argv[2])
out = sys.argv[3]
do_reset = "--no-reset" not in sys.argv
baud = 115200
if "--baud" in sys.argv:
    baud = int(sys.argv[sys.argv.index("--baud") + 1])

buf = bytearray()


def open_port():
    s = serial.Serial(port, baud, timeout=0.1)
    # 关键：先确保 IO0=HIGH，避免复位时误入下载模式
    s.setDTR(False)
    s.setRTS(False)
    time.sleep(0.2)
    return s


print("打开 %s @ %d ..." % (port, baud))
ser = open_port()
ser.reset_input_buffer()

if do_reset:
    print("触发复位（RTS 脉冲，DTR 保持 False）...")
    try:
        ser.setRTS(True)     # EN=LOW
        time.sleep(0.2)
        ser.setRTS(False)    # EN=HIGH -> 启动
        time.sleep(0.05)
    except Exception as e:
        print("复位时序异常（继续读取）: %r" % (e,))
else:
    print("不复位，直接读取 ...")

print("读取 %d 秒 ..." % secs)
end = time.time() + secs
reopen_tries = 0
while time.time() < end:
    try:
        d = ser.read(8192)
        if d:
            buf += d
            reopen_tries = 0
    except Exception as e:
        # 复位/深睡可能导致端口掉线，尝试重连
        print("读取异常(%r)，尝试重连 ..." % (e,))
        try:
            ser.close()
        except Exception:
            pass
        reopen_tries += 1
        if reopen_tries > 6:
            print("重连失败次数过多，停止。")
            break
        time.sleep(0.5)
        try:
            ser = open_port()
        except Exception as e2:
            print("重连失败: %r" % (e2,))
            time.sleep(0.5)

try:
    ser.close()
except Exception:
    pass

with open(out, "wb") as f:
    f.write(bytes(buf))
print("共捕获 %d 字节 -> %s" % (len(buf), out))
