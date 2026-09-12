# -*- coding: utf-8 -*-
"""
固件整片备份完整性核查 + 生成还原脚本/分区清单。

用法:
  python verify_backup.py <备份.bin> <输出目录>

核查项:
  1. 文件大小与哈希（独立重算，供与 manifest 比对）
  2. 首 16 字节
  3. 镜像头有效性（0x0 / 0x8000 / 0x10000）
  4. 分区表解析 + 每个分区的实际内容概况（0xFF / 混合 / 全0x00）
  5. 每个 app 分区的镜像头与应用描述（项目名/版本/编译时间/IDF 版本）
  6. 可疑的"大段全 0x00"检测（读失败的典型特征）
产出:
  partitions_backup.csv / restore.bat / restore.ps1 / VERIFY_REPORT.txt
"""
import hashlib
import os
import struct
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

PT_MAGIC = 0x50AA
APP_DESC_MAGIC = 0xABCD5432

TYPE_NAMES = {0x00: "app", 0x01: "data"}
APP_SUB = {0x00: "factory", 0x10: "ota_0", 0x11: "ota_1", 0x20: "test"}
DATA_SUB = {0x00: "ota", 0x01: "phy", 0x02: "nvs", 0x03: "coredump", 0x04: "nvs_keys",
            0x05: "efuse", 0x06: "undefined", 0x80: "esphttpd", 0x81: "fat", 0x82: "spiffs",
            0x83: "littlefs"}

out_lines = []


def p(s=""):
    print(s)
    out_lines.append(s)


def region_kind(data, off, size):
    """返回 (kind, fill_ratio)：kind = EMPTY(全FF) / ZERO(全00) / MIXED"""
    chunk = data[off:off + size]
    if not chunk:
        return "OUT_OF_RANGE", 0.0
    ff = chunk.count(0xFF)
    zz = chunk.count(0x00)
    n = len(chunk)
    if ff == n:
        return "EMPTY(全FF)", 0.0
    if zz == n:
        return "ZERO(全00)", 0.0
    return "MIXED", 1.0 - (ff + zz) / n


def parse_app_desc(data, base):
    if base + 0x100 > len(data) or data[base] != 0xE9:
        return None
    d = base + 0x20
    if struct.unpack("<I", data[d:d + 4])[0] != APP_DESC_MAGIC:
        return None
    f = lambda a, b: data[d + a:d + b].rstrip(b"\x00").decode("utf-8", "replace")
    return {
        "version": f(16, 48), "project": f(48, 80),
        "time": f(80, 96), "date": f(96, 112), "idf": f(112, 144),
    }


def main():
    bin_path, out_dir = sys.argv[1], sys.argv[2]
    data = open(bin_path, "rb").read()
    total = len(data)
    name = os.path.basename(bin_path)

    p("=" * 74)
    p("固件整片备份 · 完整性核查报告")
    p("=" * 74)
    p("文件      : %s" % bin_path)
    p("文件名    : %s" % name)
    p("大小      : %d 字节 (%.2f MiB)" % (total, total / 1048576))
    p("期望大小  : 16777216 字节 (16 MiB)  ->  %s" % ("一致 ✔" if total == 16777216 else "不一致 ✘"))
    sha = hashlib.sha256(data).hexdigest().upper()
    p("SHA256    : %s" % sha)
    p("首 16 字节: %s" % " ".join("%02X" % b for b in data[:16]))

    p()
    p("-" * 74)
    p("① 镜像头检查")
    p("-" * 74)
    # 注意：ESP32-S3 的 bootloader 在 0x0（不是 ESP32 时代的 0x1000）
    checks = [
        ("bootloader @0x000000", 0x0, "img"),
        ("app 区常见位置 @0x010000", 0x10000, "img"),
        ("partition tbl @0x008000", 0x8000, "pt"),
    ]
    for label, off, kind in checks:
        if off >= total:
            p("  %-26s 越界" % label)
            continue
        if kind == "img":
            b = data[off]
            empty = all(x == 0xFF for x in data[off:off + 64])
            note = "空(该处未使用)" if empty else (
                "ESP 镜像头 0xE9 ✔" if b == 0xE9 else "非镜像头 0x%02X" % b)
        else:
            magic = struct.unpack("<H", data[off:off + 2])[0]
            note = "分区表 magic 0xAA50 ✔" if magic == PT_MAGIC else \
                   "非分区表 magic 0x%04X" % magic
        p("  %-26s %s" % (label, note))

    p()
    p("-" * 74)
    p("② 分区表（@0x8000）")
    p("-" * 74)
    rows = []
    for i in range(95):
        ent = data[0x8000 + i * 32: 0x8000 + i * 32 + 32]
        if len(ent) < 32:
            break
        magic, ptype, subtype, off, size = struct.unpack("<HBBII", ent[:12])
        if magic != PT_MAGIC:
            break
        label = ent[12:28].rstrip(b"\x00").decode("utf-8", "replace")
        rows.append((label, ptype, subtype, off, size))
    if not rows:
        p("  !! 未解析到有效分区表")
    p("  %-12s %-6s %-9s %-10s %-11s %-8s %s" % (
        "label", "type", "subtype", "offset", "size", "使用率", "内容概况"))
    for label, ptype, subtype, off, size in rows:
        t = TYPE_NAMES.get(ptype, "0x%02X" % ptype)
        s = (APP_SUB if ptype == 0 else DATA_SUB).get(subtype, "0x%02X" % subtype)
        kind, ratio = region_kind(data, off, size)
        inrange = "✔" if off + size <= total else "✘越界"
        p("  %-12s %-6s %-9s 0x%06X   0x%06X   %5.1f%%   %s %s" % (
            label, t, s, off, size, ratio * 100, kind, inrange))
    p("  分区数: %d   已分配合计: %.2f MiB / %.2f MiB" % (
        len(rows), sum(r[4] for r in rows) / 1048576, total / 1048576))

    p()
    p("-" * 74)
    p("③ 应用镜像信息（每个 app 分区）")
    p("-" * 74)
    apps = [r for r in rows if r[1] == 0x00]
    if not apps:
        p("  (无 app 分区)")
    for label, ptype, subtype, off, size in apps:
        info = parse_app_desc(data, off)
        p("  [%s] @0x%06X size=%.0f KB" % (label, off, size / 1024))
        if info:
            p("      项目名  : %s" % info["project"])
            p("      版本    : %s" % info["version"])
            p("      编译时间: %s %s" % (info["date"], info["time"]))
            p("      IDF 版本: %s" % info["idf"])
        else:
            p("      (无标准 app 描述 / 非镜像)")

    p()
    p("-" * 74)
    p("④ 可疑区段扫描（大段全 0x00 = 读失败的典型特征）")
    p("-" * 74)
    blocks = 16
    bs = total // blocks
    zero_runs, ff_bytes = [], 0
    for i in range(blocks):
        seg = data[i * bs:(i + 1) * bs]
        z = seg.count(0x00)
        ff_bytes += seg.count(0xFF)
        if z == len(seg):
            zero_runs.append((i * bs, (i + 1) * bs))
    if zero_runs:
        for a, b in zero_runs:
            p("  !! 全 0x00 区段: 0x%06X - 0x%06X" % (a, b))
    else:
        p("  未发现全 0x00 区段 ✔")
    p("  全 0xFF（擦除态）占比: %.1f%%" % (100.0 * ff_bytes / total))

    p()
    p("=" * 74)
    p("结论")
    p("=" * 74)
    ok = (total == 16777216) and bool(rows) and not zero_runs and data[0] == 0xE9
    p("  %s" % ("备份结构完整：大小为 16MB 整片、分区表可解析、bootloader 镜像头有效。" if ok
             else "存在异常项，见上。"))
    if len(sys.argv) > 3:
        exp = sys.argv[3].strip().upper()
        same = (exp == sha)
        p("  与清单声明的 SHA256 比对: %s" % ("一致 ✔" if same else "不一致 ✘"))
        p("    声明值: %s" % exp)
        p("    实测值: %s" % sha)
        ok = ok and same

    # ---- 产出 ----
    os.makedirs(out_dir, exist_ok=True)
    py = r"<IDF_ROOT>\tools\python_env\idf5.5_py3.13_env\Scripts\python.exe"

    bat = os.path.join(out_dir, "restore.bat")
    with open(bat, "w", encoding="ascii", newline="\r\n") as f:
        f.write("@echo off\n")
        f.write("REM ================================================================\n")
        f.write("REM  Restore the full 16MB flash image back to the board.\n")
        f.write("REM  Usage: restore.bat [COMx]        (default COM5)\n")
        f.write("REM  WARNING: rewrites the WHOLE 16MB flash (bootloader+table+NVS+app).\n")
        f.write("REM ================================================================\n")
        f.write("setlocal\n")
        f.write("set PORT=%1\n")
        f.write('if "%PORT%"=="" set PORT=COM5\n')
        f.write("set HERE=%~dp0\n")
        f.write('set PY="' + py + '"\n')
        f.write("echo Restoring full 16MB image to %PORT% ...\n")
        f.write('%PY% -m esptool --chip esp32s3 --port %PORT% write_flash 0x0 "%HERE%' + name + '"\n')
        f.write("if errorlevel 1 goto fail\n")
        f.write("echo.\necho Done. Board restored and reset.\ngoto end\n")
        f.write(":fail\necho FAILED. Check the COM port / cable.\n:end\nendlocal\npause\n")
    p("")
    p("已生成还原脚本: %s" % bat)

    csv = os.path.join(out_dir, "partitions_backup.csv")
    with open(csv, "w", encoding="utf-8", newline="") as f:
        f.write("label,type,subtype,offset,size,size_dec\n")
        for label, ptype, subtype, off, size in rows:
            tname = TYPE_NAMES.get(ptype, "0x%02X" % ptype)
            sname = (APP_SUB if ptype == 0 else DATA_SUB).get(subtype, "0x%02X" % subtype)
            f.write("%s,%s,%s,0x%06X,0x%06X,%d\n" % (label, tname, sname, off, size, size))
    p("已生成分区清单: %s" % csv)

    rep = os.path.join(out_dir, "VERIFY_REPORT.txt")
    open(rep, "w", encoding="utf-8").write("\n".join(out_lines) + "\n")
    p("已生成核查报告: %s" % rep)
    return 0


if __name__ == "__main__":
    sys.exit(main())
