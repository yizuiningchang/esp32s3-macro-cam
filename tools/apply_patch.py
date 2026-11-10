#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
为 esp32-camera 组件应用 EOI 探测窗口补丁（幂等）。

背景：仓库不包含 managed_components/（由组件管理器按 dependencies.lock 下载）。
      官方 esp32-camera 2.1.7 在极暗场景会因 NO-EOI 丢帧，必须打完补丁才能存卡。

用法：
    idf.py build            # 先编译一次，触发组件下载
    python tools/apply_patch.py
    idf.py build            # 增量重编

重复执行安全：已打过补丁则直接退出。
"""
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TARGET_REL = "managed_components/espressif__esp32-camera/driver/cam_hal.c"
PATCH_REL = "tools/patches/cam_hal.c.patched"
MARKER = "esp32s3_macro_cam 补丁"

target = os.path.join(ROOT, TARGET_REL)
patch = os.path.join(ROOT, PATCH_REL)
backup = target + ".orig"


def read(p):
    with open(p, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def main():
    if not os.path.exists(target):
        print("[x] 目标文件不存在：%s" % TARGET_REL)
        print("    请先执行一次 `idf.py build`，让组件管理器下载 esp32-camera。")
        return 2

    if not os.path.exists(patch):
        print("[x] 补丁文件缺失：%s" % PATCH_REL)
        return 2

    if MARKER in read(target):
        print("[=] 补丁已存在，无需重复应用。")
        return 0

    if not os.path.exists(backup):
        shutil.copy2(target, backup)
        print("[+] 已备份原文件 -> %s" % os.path.basename(backup))

    shutil.copy2(patch, target)

    if MARKER in read(target):
        print("[+] 补丁应用成功：EOI 探测窗口已放宽，暗场丢帧问题修复。")
        print("    下一步：重新执行 `idf.py build`")
        return 0

    print("[x] 应用后校验失败，请检查补丁文件内容。")
    return 1


if __name__ == "__main__":
    sys.exit(main())
