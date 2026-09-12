"""在（UTF-16 或 UTF-8 的）日志里搜索关键字，打印命中行及上下文

用法: python grep_log.py <日志路径> <关键字> [上下文行数]
"""
import re
import sys

path = sys.argv[1]
kw = sys.argv[2]
ctx = int(sys.argv[3]) if len(sys.argv) > 3 else 0

raw = open(path, "rb").read()
null_ratio = raw.count(0) / max(len(raw), 1)
text = raw.decode("utf-16-le" if null_ratio > 0.15 else "utf-8", "replace")
text = re.sub(r"\x1b\[[0-9;?]*[a-zA-Z]", "", text)
text = text.replace("\r\n", "\n").replace("\r", "\n")
lines = [ln.rstrip() for ln in text.split("\n")]
lines = [ln for ln in lines if not re.fullmatch(r"[\s\d.,:]*\d{1,3}%[\s.]*", ln)]

pat = re.compile(kw, re.I)
hits = [i for i, ln in enumerate(lines) if pat.search(ln)]
print("总行数 %d，命中 %d 行" % (len(lines), len(hits)))
print("-" * 60)
for i in hits[:40]:
    lo, hi = max(0, i - ctx), min(len(lines), i + ctx + 1)
    for j in range(lo, hi):
        mark = ">>" if j == i else "  "
        print("%s %s" % (mark, lines[j][:300]))
    if ctx:
        print("   " + "-" * 40)
