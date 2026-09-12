# -*- coding: utf-8 -*-
"""
从整片备份里按 ESP 镜像格式逐段解析 app 镜像，验证结构完整性与 XOR 校验和。

ESP 镜像布局（esptool: ESP32FirmwareImage）:
  0x00  base header   : E9 | seg_cnt | spi_mode | spi_size_freq | entry_addr(4)
  0x08  extended hdr  : wp_pin | drv | mode | size_freq | chip_id(2) | min_rev | ... | hash_appended
  0x18  段表起点      : [load_addr(4) | data_len(4) | data] × seg_cnt
  之后                : checksum(1)（seg_cnt != 0 时）= 全部 data 字节异或
  再之后（hash_appended=1）: SHA256(32)

用法: python check_app_img.py <整片.bin> <offset_hex> <size_hex> [标签]
"""
import struct
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HEADER_LEN = 8
EXT_HEADER_LEN = 16
SEG_START = HEADER_LEN + EXT_HEADER_LEN          # 0x18

SPI_MODE = {0: "QIO", 1: "QOUT", 2: "DIO", 3: "DOUT"}
SPI_FREQ = {0x0: "40MHz", 0x1: "26MHz", 0x2: "20MHz", 0xF: "80MHz"}
SPI_SIZE = {0x0: "1MB", 0x1: "2MB", 0x2: "4MB", 0x3: "8MB", 0x4: "16MB",
            0x5: "32MB", 0x6: "64MB", 0x7: "128MB"}


def main():
    path, off_s, size_s = sys.argv[1], sys.argv[2], sys.argv[3]
    label = sys.argv[4] if len(sys.argv) > 4 else "?"
    off, size = int(off_s, 16), int(size_s, 16)
    blob = open(path, "rb").read()[off:off + size]

    print("=" * 74)
    print("[%s]  偏移 0x%06X  分区长度 0x%06X (%d KB)" % (label, off, size, size // 1024))
    print("=" * 74)
    if len(blob) < SEG_START or blob[0] != 0xE9:
        print("  !! 不是 ESP 镜像（首字节 0x%02X）" % blob[0])
        return 1

    seg_cnt = blob[1]
    mode = SPI_MODE.get(blob[2], blob[2])
    fsize = SPI_SIZE.get((blob[3] >> 4) & 0x0F, "?")
    ffreq = SPI_FREQ.get(blob[3] & 0x0F, hex(blob[3] & 0x0F))
    entry = struct.unpack("<I", blob[4:8])[0]
    chip_id = struct.unpack("<H", blob[12:14])[0]
    hash_app = blob[23]

    print("  base 头  : magic=0xE9  seg_cnt=%d  mode=%s  声明 flash=%s/%s  entry=0x%08X"
          % (seg_cnt, mode, fsize, ffreq, entry))
    print("  扩展头   : %s" % " ".join("%02X" % b for b in blob[8:24]))
    print("  chip_id  : %d %s   hash_appended=%d"
          % (chip_id, "(9 = ESP32-S3 ✔)" if chip_id == 9 else "(非 S3)", hash_app))

    pos = SEG_START
    total = 0
    xorr = 0
    segs = []
    for i in range(seg_cnt):
        if pos + 8 > len(blob):
            print("  !! 段 %d 头越界" % i)
            return 1
        addr, dlen = struct.unpack("<II", blob[pos:pos + 8])
        pos += 8
        if pos + dlen > len(blob):
            print("  !! 段 %d 数据越界（需 %d 字节，仅剩 %d）" % (i, dlen, len(blob) - pos))
            return 1
        seg = blob[pos:pos + dlen]
        for byte in seg:
            xorr ^= byte
        pos += dlen
        total += dlen
        segs.append((i, addr, dlen))
    print("  段表     :")
    for i, addr, dlen in segs:
        print("    段%d  load=0x%08X  len=%8d  (%7.1f KB)" % (i, addr, dlen, dlen / 1024))
    print("  数据合计 : %.1f KB" % (total / 1024))

    ok = True
    if seg_cnt:
        if pos >= len(blob):
            print("  !! 缺少校验和字节")
            return 1
        ck = blob[pos]
        ok = (ck == xorr)
        print("  XOR 校验 : 文件=0x%02X  计算=0x%02X  ->  %s"
              % (ck, xorr, "一致 ✔" if ok else "不一致 ✘"))
        pos += 1
    if hash_app == 1:
        if pos + 32 <= len(blob):
            print("  尾部     : SHA256 已附加 ✔")
            pos += 32
        else:
            print("  !! 声明有 SHA256 但长度不足 ✘")
            ok = False
    fits = pos <= size
    print("  镜像实际占用: %d 字节 (%.1f KB) / 分区 %d KB  ->  %s"
          % (pos, pos / 1024, size // 1024, "放得下 ✔" if fits else "超出分区 ✘"))
    print("  小结     : %s" % ("镜像结构完整、校验和通过 ✔✔" if (ok and fits) else "存在问题 ✘"))
    return 0 if (ok and fits) else 1


if __name__ == "__main__":
    sys.exit(main())
