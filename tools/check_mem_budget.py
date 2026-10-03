#!/usr/bin/env python3
"""常驻内存预算检查(纯静态分析,不编译)。

背景(2026-10-04 真实回归):v1.1.8 为 Hermes analytics 加了一个 24KB 的
`static char buf[]` 常驻缓冲,外加两页约 56 个常驻 LVGL 对象。C3 无 PSRAM,
历史诊断记录 largest_block ≈ 29.7KB —— 24KB 直接把最大连续块压到不足 6KB,
DNS 解析要分配的 PCB 第一个失败,连带三家余额全报 DNS、门户 httpd 也起不来。
本机 host 测试全绿、CI 编译全绿,只有真机才炸。

所以规则是:**任何新增的常驻(static)缓冲都必须留出堆余量**,并在改动时就检查。
本脚本扫描 main/*.c 里的 static 大缓冲,超过阈值就失败。

阈值 8KB:余额链路的 RESP_MAX 缓冲是历史基线(已在 4KB 级别),
Hermes 明文 HTTP 无 TLS 峰值,给它 8KB 是本项目认定的安全上限。
"""
import re
import sys
from pathlib import Path

LIMIT = 8192
# 头文件里的位图是 const(在 flash),不算 RAM;这里只抓 .c 里的 static 数组
ARRAY_RE = re.compile(
    r"^\s*static\s+(?:const\s+)?(?:char|uint8_t|int|int32_t|uint32_t)\s+"
    r"([A-Za-z_]\w*)\s*\[\s*(\w+)\s*\]",
    re.M,
)
# 宏常量表:名字 -> 表达式,便于算 static char x[SOME_MACRO]
def collect_defines(text: str) -> dict:
    out = {}
    for m in re.finditer(r"^\s*#define\s+([A-Za-z_]\w*)\s+(\d+)\s*(?:/\*|//|$)", text, re.M):
        out[m.group(1)] = int(m.group(2))
    return out


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("main")
    bad = False
    rows = []
    for c in sorted(root.glob("*.c")):
        text = c.read_text(encoding="utf-8", errors="replace")
        defines = collect_defines(text)
        for name, size in ARRAY_RE.findall(text):
            n = defines.get(size)
            if n is None:
                if size.isdigit():
                    n = int(size)
                else:
                    continue          # 运行时才知道的尺寸,跳过
            rows.append((c.name, name, n, size))
            if n > LIMIT:
                print(f"FAIL {c.name}: static {name}[{size}] = {n} B > {LIMIT} B budget")
                bad = True
    for f, name, n, size in sorted(rows, key=lambda r: -r[2]):
        print(f"     {f:20} static {name}[{size}] = {n} B")
    if not rows:
        print("(no static arrays with a resolvable size)")
    if bad:
        print("\n常驻缓冲超预算:无 PSRAM 的 C3 上这会压垮堆的最大连续块,")
        print("表现为 DNS 解析失败 → 余额全挂、门户 httpd 起不来。")
        print("修法:缩小缓冲 + 缩短窗口,或改成流式解析(边收边提取)。")
        return 1
    print(f"\nOK: all resident static buffers within {LIMIT} B")
    return 0


if __name__ == "__main__":
    sys.exit(main())
