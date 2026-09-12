# -*- coding: utf-8 -*-
"""
校验开发板整片备份，并生成还原脚本与清单。

用法:
  python analyze_backup.py <备份.bin> <输出目录>

产出:
  - 控制台/日志：分区表、各分区内容摘要、应用信息、填充率
  - partitions_backup.csv   : 备份里的分区表
  - restore.bat            : 一键还原脚本（esptool write_flash）
  - README.md              : 清单与还原说明
"""
import hashlib
import os
import struct
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

TYPES = {
    0x00: "app",
    0x01: "data",
}
APP_SUBTYPES = {
    0x00: "factory", 0x10: "ota_0", 0x11: "ota_1", 0x20: "test",
}
DATA_SUBTYPES = {
    0x00: "ota", 0x01: "phy", 0x02: "nvs", 0x03: "coredump", 0x04: "nvs_keys",
    0x05: "efuse", 0x06: "undefined", 0x80: "esphttpd", 0x81: "fat", 0x82: "spiffs",
    0x83: "littlefs",
}

PT_MAGIC = 0x50AA
APP_DESC_MAGIC = 0xABCD5432


def main():
    bin_path = sys.argv[1]
    out_dir = sys.argv[2]
    data = open(bin_path, "rb").read()
    total = len(data)
    print("=" * 72)
    print("备份文件: %s" % bin_path)
    print("大小: %d 字节 (%.2f MiB)" % (total, total / 1048576))
    print("SHA256: %s" % hashlib.sha256(data).hexdigest().upper())

    # ---- 镜像头检查 ----
    print()
    print("=" * 72)
    print("关键偏移检查")
    print("=" * 72)
    for name, off in [("bootloader @0x1000", 0x1000), ("partition table @0x8000", 0x8000),
                      ("app @0x10000", 0x10000)]:
        if off + 1 <= total:
            magic = data[off]
            blank = all(b in (0xFF, 0x00) for b in data[off:off + 64])
            print("  %-26s 首字节=0x%02X  %s" % (
                name, magic,
                "(空)" if blank else ("像 ESP 镜像 (0xE9)" if magic == 0xE9 else "非镜像头")))
    # 全片填充率
    filled = sum(1 for b in data if b not in (0xFF,))
    print("  非 0xFF 字节占比: %.1f%%  (其余为擦除态)" % (100.0 * filled / total))

    # ---- 解析分区表 ----
    print()
    print("=" * 72)
    print("分区表（@0x8000）")
    print("=" * 72)
    rows = []
    off = 0x8000
    for i in range(95):
        ent = data[off + i * 32: off + i * 32 + 32]
        if len(ent) < 32:
            break
        magic, ptype, subtype, p_off, p_size = struct.unpack("<HBBII", ent[:12])
        label = ent[12:28].rstrip(b"\x00").decode("utf-8", "replace")
        flags = struct.unpack("<I", ent[28:32])[0]
        if magic != PT_MAGIC:
            break
        tname = TYPES.get(ptype, "0x%02X" % ptype)
        if ptype == 0x00:
            sname = APP_SUBTYPES.get(subtype, "0x%02X" % subtype)
        elif ptype == 0x01:
            sname = DATA_SUBTYPES.get(subtype, "0x%02X" % subtype)
        else:
            sname = "0x%02X" % subtype
        rows.append((label, tname, sname, p_off, p_size, flags))
        print("  %-14s %-5s %-9s off=0x%06X size=0x%06X (%s) flags=0x%X" % (
            label, tname, sname, p_off, p_size,
            ("%.0f KB" % (p_size / 1024)), flags))
    if not rows:
        print("  (未解析到分区表)")
    print("  分区数: %d   已占用合计: %.2f MiB / 16.00 MiB" % (
        len(rows), sum(r[4] for r in rows) / 1048576))

    # ---- 应用信息 ----
    print()
    print("=" * 72)
    print("应用镜像信息")
    print("=" * 72)
    app_off = None
    for r in rows:
        if r[1] == "app":
            app_off = r[3]
            break
    if app_off is None:
        app_off = 0x10000
    try:
        base = app_off
        assert data[base] == 0xE9, "app 偏移处不是镜像头"
        desc_off = base + 0x20
        magic = struct.unpack("<I", data[desc_off:desc_off + 4])[0]
        print("  app 偏移: 0x%06X" % app_off)
        if magic == APP_DESC_MAGIC:
            ver = data[desc_off + 16:desc_off + 48].rstrip(b"\x00").decode("utf-8", "replace")
            pname = data[desc_off + 48:desc_off + 80].rstrip(b"\x00").decode("utf-8", "replace")
            tstr = data[desc_off + 80:desc_off + 96].rstrip(b"\x00").decode("utf-8", "replace")
            dstr = data[desc_off + 96:desc_off + 112].rstrip(b"\x00").decode("utf-8", "replace")
            idf = data[desc_off + 112:desc_off + 144].rstrip(b"\x00").decode("utf-8", "replace")
            print("  项目名  : %s" % pname)
            print("  版本    : %s" % ver)
            print("  编译时间: %s %s" % (dstr, tstr))
            print("  IDF 版本: %s" % idf)
        else:
            print("  (app_desc magic=0x%08X，非标准 IDF 描述)" % magic)
    except Exception as e:
        print("  解析失败: %r" % (e,))

    # ---- 产出还原脚本 ----
    os.makedirs(out_dir, exist_ok=True)
    py = r"<IDF_ROOT>\tools\python_env\idf5.5_py3.13_env\Scripts\python.exe"
    bat = os.path.join(out_dir, "restore.bat")
    with open(bat, "w", encoding="ascii", newline="\r\n") as f:
        f.write("@echo off\n")
        f.write("REM ============================================================\n")
        f.write("REM Restore the full 16MB flash backup taken from the board.\n")
        f.write("REM Usage: restore.bat [COMx]     (default COM5)\n")
        f.write("REM WARNING: this erases and rewrites the whole 16MB flash.\n")
        f.write("REM ============================================================\n")
        f.write("setlocal\n")
        f.write("set PORT=%1\n")
        f.write("if \"%PORT%\"==\"\" set PORT=COM5\n")
        f.write("set HERE=%~dp0\n")
        f.write("set PY=\"" + py + "\"\n")
        f.write("\n")
        f.write("echo Restoring full flash image to %PORT% ...\n")
        f.write("%PY% -m esptool --chip esp32s3 --port %PORT% write_flash 0x0 \"%HERE%esp32s3_full_flash_16MB.bin\"\n")
        f.write("if errorlevel 1 goto fail\n")
        f.write("echo.\n")
        f.write("echo Done. Board restored and reset.\n")
        f.write("goto end\n")
        f.write(":fail\n")
        f.write("echo FAILED. Check the COM port and that the board is connected.\n")
        f.write(":end\n")
        f.write("endlocal\n")
        f.write("pause\n")
    print()
    print("已生成: %s" % bat)

    csv = os.path.join(out_dir, "partitions_backup.csv")
    with open(csv, "w", encoding="utf-8", newline="") as f:
        f.write("label,type,subtype,offset,size,size_dec\n")
        for r in rows:
            f.write("%s,%s,%s,0x%06X,0x%06X,%d\n" % (r[0], r[1], r[2], r[3], r[4], r[4]))
    print("已生成: %s" % csv)
    return 0


if __name__ == "__main__":
    sys.exit(main())
