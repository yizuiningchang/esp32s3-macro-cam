# -*- coding: utf-8 -*-
"""守候 ESP32-S3 上电启动日志（三阶段）。

板子被卡在 ROM 下载模式时，只有"拔插 USB"(上电复位) 才能回到正常启动。
本脚本按三阶段工作，确保从"上电第一刻"开始录：

    阶段1: 等 COM 口消失（你拔掉 USB）
    阶段2: 等 COM 口出现（你插回 USB）
    阶段3: 立刻打开并录制 N 秒

用法:
    python wait_boot.py <COMx> <阶段1超时> <阶段2超时> <录制秒数> <输出文件>
"""
import sys
import time

import serial

port = sys.argv[1]
t_absent = int(sys.argv[2])
t_present = int(sys.argv[3])
read_secs = int(sys.argv[4])
out = sys.argv[5]


def port_exists():
    try:
        s = serial.Serial(port, 115200, timeout=0.1)
        s.close()
        return True
    except Exception:
        return False


def log(msg):
    print(msg, flush=True)


# ---------- 阶段 1: 等端口消失 ----------
log("阶段1: 等待你拔掉 USB（%s 消失，最多 %d 秒）..." % (port, t_absent))
t0 = time.time()
gone = False
while time.time() - t0 < t_absent:
    if not port_exists():
        gone = True
        log("  已检测到 %s 断开 (%.1fs)" % (port, time.time() - t0))
        break
    time.sleep(0.2)
if not gone:
    log("阶段1 超时：端口一直存在，可能你没拔线。仍继续尝试阶段2。")

# ---------- 阶段 2: 等端口出现 ----------
log("阶段2: 等待你插回 USB（%s 出现，最多 %d 秒）..." % (port, t_present))
t1 = time.time()
ser = None
while time.time() - t1 < t_present:
    try:
        ser = serial.Serial(port, 115200, timeout=0.1)
        # 关键：绝不置 DTR（置位会被判为下载模式请求）
        ser.setDTR(False)
        ser.setRTS(False)
        log("  检测到 %s 上线 (%.2fs)，开始录制 ..." % (port, time.time() - t1))
        break
    except Exception:
        ser = None
        time.sleep(0.02)          # 密集轮询，尽量抓住启动最初几百毫秒

if ser is None:
    log("阶段2 超时：%s 未出现。" % port)
    open(out, "wb").write(b"")
    sys.exit(0)

# ---------- 阶段 3: 录制 ----------
buf = bytearray()
end = time.time() + read_secs
while time.time() < end:
    try:
        d = ser.read(8192)
        if d:
            buf += d
            with open(out, "wb") as f:      # 增量落盘，防中途被杀
                f.write(bytes(buf))
    except Exception as e:
        log("读取中断: %r" % (e,))
        break

try:
    ser.close()
except Exception:
    pass
with open(out, "wb") as f:
    f.write(bytes(buf))
log("共捕获 %d 字节 -> %s" % (len(buf), out))
