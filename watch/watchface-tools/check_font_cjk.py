# -*- coding: utf-8 -*-
"""
检查字体文件到底有没有中文字模。

为什么要单独查：LVGL 显示中文的前提是「当前生效的字体里真有这些汉字的 glyph」。
字库缺字时 LVGL 不会报错，只会画不出 —— 屏幕上一片空白，日志里一个字都没有，
正是那种"看不见就查不出"的坑。所以开写中文界面之前先把字库验一遍。

用法：
    python check_font_cjk.py
输出：每个 .ttf 的中文字模覆盖情况 + 我们要用到的那几个字到底有没有。
"""
import os
import struct
import sys

FONT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "..", "lvgl", "watch", "src", "resource", "fonts", "freetype")

# 界面上真正会显示出来的字。
# 关机确认框：关机 / 确定要关机吗？ / 确定 / 取消
# 表盘未校时提示：未校时 · 点这里校准
# 校时面板：点一项选中，再用下面 - + 调整 年月日分秒 取消确定
# 状态栏蓝牙行（上划窗口 BT 按钮下面那一行）：
#     蓝牙：启动中 / 就绪 · 点 BT 开 / 广播中 · 用手机对时 / 已连上 · 已校时
NEEDED = ("关机确定要吗取消未校时请重新对上滑设置语言蓝牙连接断开已按钮"
          "时间点一项选中再用下面调整年月日分秒这里校准"
          "启动中广 播用手 机对就绪开")
# 注意："−"（U+2212 真减号）在部分字库里没有，面板上的减号一律用 ASCII "-"。
# 注意：·（U+00B7 间隔号）和全角冒号：不是汉字，但同样要字库里有才能显示。
#      上面这串只统计汉字；间隔号/冒号属于基本标点，现有几款字体都有。


class TTF(object):
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        self.tables = {}
        n = struct.unpack(">H", self.data[4:6])[0]
        for i in range(n):
            off = 12 + 16 * i
            tag = self.data[off:off + 4].decode("latin-1")
            toff, tlen = struct.unpack(">II", self.data[off + 8:off + 16])
            self.tables[tag] = (toff, tlen)
        self.codepoints = self._parse_cmap()

    def _parse_cmap(self):
        cps = set()
        if "cmap" not in self.tables:
            return cps
        base = self.tables["cmap"][0]
        n = struct.unpack(">H", self.data[base + 2:base + 4])[0]
        subtables = []
        for i in range(n):
            off = base + 4 + 8 * i
            pid, eid, sub = struct.unpack(">HHI", self.data[off:off + 8])
            subtables.append((pid, eid, base + sub))
        # 优先 unicode 全集
        subtables.sort(key=lambda s: 0 if (s[0] == 3 and s[1] == 10) else
                                     (1 if s[0] == 3 and s[1] == 1 else
                                      (2 if s[0] == 0 else 3)))
        for pid, eid, off in subtables:
            fmt = struct.unpack(">H", self.data[off:off + 2])[0]
            try:
                if fmt == 4:
                    cps |= self._cmap4(off)
                elif fmt == 12:
                    cps |= self._cmap12(off)
                elif fmt == 6:
                    cps |= self._cmap6(off)
            except Exception:
                pass
            if cps:
                break
        return cps

    def _cmap4(self, off):
        cps = set()
        segx2 = struct.unpack(">H", self.data[off + 6:off + 8])[0]
        seg = segx2 // 2
        ends = struct.unpack(">%dH" % seg, self.data[off + 14:off + 14 + segx2])
        p = off + 16 + segx2
        starts = struct.unpack(">%dH" % seg, self.data[p:p + segx2])
        p += segx2
        deltas = struct.unpack(">%dh" % seg, self.data[p:p + segx2])
        p += segx2
        range_off_base = p
        ranges = struct.unpack(">%dH" % seg, self.data[p:p + segx2])
        for i in range(seg):
            if starts[i] > ends[i]:
                continue
            for c in range(starts[i], min(ends[i], 0xFFFF) + 1):
                if ranges[i] == 0:
                    g = (c + deltas[i]) & 0xFFFF
                else:
                    gp = range_off_base + 2 * i + ranges[i] + 2 * (c - starts[i])
                    if gp + 2 > len(self.data):
                        continue
                    g = struct.unpack(">H", self.data[gp:gp + 2])[0]
                    if g:
                        g = (g + deltas[i]) & 0xFFFF
                if g:
                    cps.add(c)
        return cps

    def _cmap12(self, off):
        cps = set()
        ngroups = struct.unpack(">I", self.data[off + 12:off + 16])[0]
        for i in range(ngroups):
            p = off + 16 + 12 * i
            sc, ec, _gi = struct.unpack(">III", self.data[p:p + 12])
            for c in range(sc, min(ec, 0x2FFFF) + 1):
                cps.add(c)
        return cps

    def _cmap6(self, off):
        cps = set()
        first, count = struct.unpack(">HH", self.data[off + 6:off + 10])
        for i in range(count):
            g = struct.unpack(">H", self.data[off + 10 + 2 * i:off + 12 + 2 * i])[0]
            if g:
                cps.add(first + i)
        return cps


def main():
    font_dir = os.path.normpath(FONT_DIR)
    print("字体目录: %s" % font_dir)
    if not os.path.isdir(font_dir):
        print("!! 目录不存在")
        return 1

    ttf_files = sorted(f for f in os.listdir(font_dir) if f.lower().endswith(".ttf"))
    if not ttf_files:
        print("!! 没有 .ttf")
        return 1

    for name in ttf_files:
        path = os.path.join(font_dir, name)
        try:
            f = TTF(path)
        except Exception as e:
            print("%-32s 解析失败: %s" % (name, e))
            continue
        cps = f.codepoints
        cjk = sum(1 for c in cps if 0x4E00 <= c <= 0x9FFF)
        missing = [ch for ch in NEEDED if ord(ch) not in cps]
        print("-" * 78)
        print("%s   (%.1f MB)" % (name, os.path.getsize(path) / 1048576.0))
        print("  码位数 %d   其中汉字(4E00-9FFF) %d  %s"
              % (len(cps), cjk, "[有中文]" if cjk > 500 else "[基本没有中文]"))
        if missing:
            print("  本次要用的字缺 %d 个: %s" % (len(missing), "".join(missing)))
        else:
            print("  本次要用的字全部齐全: %s" % NEEDED)
        if "name" in f.tables:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
