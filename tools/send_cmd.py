# -*- coding: utf-8 -*-
"""向设备串口发送命令行命令并回显（USB-Serial-JTAG 专用，配合 app_cmd）。

用法:
    python send_cmd.py <COMx> <命令> [<命令2> ...] [--wait N] [--keep N]

示例:
    python send_cmd.py COM5 "cfg set frame_size 13" "cfg list"

注意:
    - 打开串口本身可能触发一次复位（Windows 驱动默认电平），脚本先等 --wait 秒
      再发命令，确保设备已启动完成、命令行任务就绪。
    - DTR 必须保持 False（IO0=HIGH），否则复位瞬间会进 ROM 下载模式。
    - 全部命令发完后保持 --keep 秒继续收日志（默认 2 秒），便于看到响应与后续输出。
"""
import sys
import time

import serial

if len(sys.argv) < 3:
    print(__doc__)
    sys.exit(1)

port = sys.argv[1]
args = sys.argv[2:]
wait_s = 3.0
keep_s = 2.0
cmds = []
i = 0
while i < len(args):
    if args[i] == "--wait":
        wait_s = float(args[i + 1]); i += 2
    elif args[i] == "--keep":
        keep_s = float(args[i + 1]); i += 2
    else:
        cmds.append(args[i]); i += 1

buf = bytearray()

ser = serial.Serial(port, 115200, timeout=0.1)
ser.setDTR(False)   # IO0=HIGH，防下载模式
ser.setRTS(False)   # EN=HIGH
print("已打开 %s，等待设备就绪 %.1f s ..." % (port, wait_s))
time.sleep(wait_s)
ser.reset_input_buffer()

for c in cmds:
    line = (c + "\r\n").encode("utf-8")
    print(">>> %s" % c)
    ser.write(line)
    end = time.time() + 1.2
    while time.time() < end:
        d = ser.read(4096)
        if d:
            buf += d
            end = time.time() + 0.6   # 有输出则顺延，读到静默为止

end = time.time() + keep_s
while time.time() < end:
    d = ser.read(4096)
    if d:
        buf += d
        end = time.time() + 0.4

ser.close()

text = buf.decode("utf-8", "replace")
# 过滤 ANSI 与串口方向控制噪声，只留可读文本
import re
text = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", text)
print("---------- 设备回显 ----------")
print(text)
print("---------- 结束 ----------")
