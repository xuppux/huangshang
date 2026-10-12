# -*- coding: utf-8 -*-
"""查 TTF 的 cmap，确认我要用的符号有没有字形。
不依赖 fonttools：手解 cmap 表（format 4 / 12）。"""
import os
import struct

FONTS = [
    r"C:\Users\18058\Desktop\huangshanpai\watch\lvgl\watch\src\resource\fonts\freetype\SourceHanSansCN-Normal.ttf",
    r"C:\Users\18058\Desktop\huangshanpai\watch\lvgl\watch\src\resource\fonts\freetype\DroidSansFallback.ttf",
    r"C:\Users\18058\Desktop\huangshanpai\watch\lvgl\watch\src\resource\fonts\freetype\tiny55_full.ttf",
    r"C:\Users\18058\Desktop\huangshanpai\watch\lvgl\watch\src\resource\fonts\freetype\tiny55_lite.ttf",
]

CHARS = {
    "0": 0x30, "9": 0x39, ".": 0x2E, "+": 0x2B, "-": 0x2D, "*": 0x2A, "/": 0x2F,
    "=": 0x3D, "%": 0x25, "C": 0x43, "(": 0x28, ")": 0x29,
    "\u00d7 ×": 0x00D7, "\u00f7 ÷": 0x00F7, "\u00b1 ±": 0x00B1,
    "\u2212 −": 0x2212, "\u232b ⌫": 0x232B, "\u2190 ←": 0x2190,
    "\u9000 退": 0x9000, "\u6e05 清": 0x6E05, "\u9664": 0x9664, "\u4e58": 0x4E58,
}


def cmap_codepoints(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"\x00\x01\x00\x00":
        return None, 0
    num_tables = struct.unpack(">H", data[4:6])[0]
    tables = {}
    for i in range(num_tables):
        off = 12 + i * 16
        tag = data[off:off + 4].decode("latin1")
        toff, tlen = struct.unpack(">II", data[off + 8:off + 16])
        tables[tag] = (toff, tlen)
    if "cmap" not in tables:
        return None, 0
    coff, _ = tables["cmap"]
    n = struct.unpack(">H", data[coff + 2:coff + 4])[0]
    cps = set()
    for i in range(n):
        rec = coff + 4 + i * 8
        pid, eid, sub = struct.unpack(">HHI", data[rec:rec + 8])
        base = coff + sub
        fmt = struct.unpack(">H", data[base:base + 2])[0]
        if fmt == 4:
            segx2 = struct.unpack(">H", data[base + 6:base + 8])[0]
            seg = segx2 // 2
            ends = struct.unpack(">%dH" % seg, data[base + 14:base + 14 + segx2])
            starts = struct.unpack(">%dH" % seg, data[base + 16 + segx2:base + 16 + 2 * segx2])
            for s, e in zip(starts, ends):
                if e == 0xFFFF and s == 0xFFFF:
                    continue
                cps.update(range(s, min(e, 0xFFFF) + 1))
        elif fmt == 12:
            ngroups = struct.unpack(">I", data[base + 12:base + 16])[0]
            for g in range(ngroups):
                go = base + 16 + g * 12
                s, e, _ = struct.unpack(">III", data[go:go + 12])
                cps.update(range(s, min(e, 0x10FFFF) + 1))
                if len(cps) > 90000:
                    break
    return cps, len(cps)


out = []
for p in FONTS:
    if not os.path.exists(p):
        out.append("MISSING %s" % p)
        continue
    cps, n = cmap_codepoints(p)
    name = os.path.basename(p)
    out.append("=== %s  (size %.1f MB, %s codepoints, cmap=%s)"
               % (name, os.path.getsize(p) / 1e6, n, "ok" if cps else "UNREADABLE"))
    if not cps:
        continue
    miss = [k for k, v in CHARS.items() if v not in cps]
    out.append("    missing: " + ("(none)" if not miss else ", ".join(miss)))

open(r"C:\Users\18058\Desktop\huangshanpai\watch\watchface-tools\_glyph_info.txt", "w",
     encoding="utf-8").write("\n".join(out))
print("\n".join(out))
