# -*- coding: utf-8 -*-
"""解析 otadata 分区，判断板子实际会从哪个分区启动。

ota_select 结构（32 字节）:
  uint32 ota_seq   (0=factory, 1=ota_0, 2=ota_1)
  uint8  seq_label[20]
  uint32 ota_state (0=NEW, 1=PENDING_VERIFY, 2=VALID, 3=INVALID, 4=ABORTED, 5=UNDEFINED)
  uint32 crc       (crc32 of the first 28 bytes)

用法: python check_otadata.py <整片.bin> [otadata偏移=0xD000] [长度=0x2000]
"""
import binascii
import struct
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

STATES = {0: "NEW", 1: "PENDING_VERIFY", 2: "VALID", 3: "INVALID", 4: "ABORTED", 5: "UNDEFINED"}
SEQ_NAME = {0: "factory", 1: "ota_0", 2: "ota_1"}


def main():
    path = sys.argv[1]
    off = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0xD000
    size = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x2000
    data = open(path, "rb").read()[off:off + size]

    print("=" * 70)
    print("otadata @0x%06X  (%d 字节, %d 个扇区)" % (off, size, size // 4096))
    print("=" * 70)
    best = None
    for sec in range(size // 4096):
        blk = data[sec * 4096:(sec + 1) * 4096]
        seq, = struct.unpack("<I", blk[0:4])
        label_raw = blk[4:24]
        label = label_raw.rstrip(b"\x00").decode("utf-8", "replace")
        printable = all(32 <= b < 127 for b in label_raw.rstrip(b"\x00")) if label_raw.rstrip(b"\x00") else True
        state, crc = struct.unpack("<II", blk[24:32])
        # IDF 用的是 esp_rom_crc32_le(UINT32_MAX, ...)：
        # ROM 实现为 return ~lfsr，等价于标准 CRC32（与 binascii.crc32 一致）；
        # 但不同 IDF 版本也可能存其反码，故两种都算。
        c1 = binascii.crc32(blk[0:28]) & 0xFFFFFFFF
        c2 = (~c1) & 0xFFFFFFFF
        seen = seq != 0xFFFFFFFF
        match = None
        if crc == c1:
            match = "标准CRC32"
        elif crc == c2:
            match = "标准CRC32反码"
        print("  扇区%d:" % sec)
        print("    ota_seq   = %d%s" % (seq, "" if seen else "  (0xFFFFFFFF = 空)"))
        print("    seq_label = %r %s" % (label, "" if printable else "(非可打印，疑未初始化)"))
        print("    ota_state = 0x%08X %s" % (state, STATES.get(state, "(未知/未初始化)")))
        print("    crc 存储  = 0x%08X   标准CRC32=0x%08X  反码=0x%08X  -> %s"
              % (crc, c1, c2, ("匹配(%s) ✔" % match) if match else "不匹配 ✘"))
        if seen and match:
            if best is None or seq > best[0]:
                best = (seq, state, sec, match)

    print()
    if best is None:
        print("  结论: 无有效条目 -> 从 factory 分区启动（默认回退）")
    else:
        seq, state, sec = best
        print("  结论: 将由 **%s** 分区启动  (ota_seq=%d, state=%s, 取自扇区%d)"
              % (SEQ_NAME.get(seq, "未知(seq=%d)" % seq), seq, STATES.get(state, state), sec))


if __name__ == "__main__":
    main()
