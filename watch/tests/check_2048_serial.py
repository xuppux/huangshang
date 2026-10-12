# -*- coding: utf-8 -*-
"""
check_2048_serial.py -- 把 _ser2048.py 抓下来的串口日志变成断言。

为什么要单独一个脚本：串口输出有 2.4 万字节，人眼扫一遍很容易只看见"数字变大了"
就点头。把"哪个残局、期望哪一行、期望加多少分"写成断言，才算真的验过。
（这一套的价值已经证明了：B 组 4 4 2 2 就是它抓出来的 i += 2 bug。）

用法：
    <python> _ser2048.py            # 先抓
    <python> tests/check_2048_serial.py
退出码 0 = 全部通过。
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TXT = os.path.join(ROOT, "_ser2048.txt")


def load():
    with open(TXT, encoding="utf-8", errors="replace") as f:
        return f.read()


def section(text, tag):
    """取 "===== <tag> : ..." 到下一个 "===== " 之间的内容。"""
    m = re.search(r"=====\s*" + re.escape(tag) + r"\s*(?::[^\n]*)?=====\n", text)
    if not m:
        return None
    start = m.end()
    nxt = text.find("\n===== ", start)
    return text[start:] if nxt < 0 else text[start:nxt]


def row(text, r):
    m = re.search(r"2048: r%d\s+((?:\d+\s*){4})" % r, text)
    return m.group(1).split() if m else None


def score(text):
    m = re.search(r"2048: ui=\S+ starts=\d+ score=(\d+)", text)
    return int(m.group(1)) if m else None


def last(text, pattern, groups=0):
    """
    取**最后一次**匹配。

    ⚠ 这个 capture 脚本跑的时候，app 可能已经是"在运行列表里"的了 ——
      框架这时走的是 Resume（日志里的 `- same. Resume it`），压根不会重新
      create。所以"create 时打一次"的东西（几何 dump）那一次就不存在，
      只有 g2048_info 里那份还在。
      凡是可能被打印多次的，一律取最后一次，不要假定只有一处。
      （2026-09-23 就被这个咬过：一堆几何断言集体假失败，其实固件没问题。）
    """
    ms = re.findall(pattern, text)
    return ms[-1] if ms else None


def main():
    if not os.path.exists(TXT):
        print("[ERROR] %s not found - run _ser2048.py first" % TXT)
        return 2
    t = load()
    checks = []

    def ck(name, cond, detail=""):
        checks.append((name, bool(cond), detail))

    # ── 0. 起没起来 / 几何 ─────────────────────────────────────────
    # ⚠ 不要断言"2048: start (#1)"：app 可能本来就是 Resume 进来的。
    #   可靠的判据是"我们确实让它进前台了" + "info 说 ui=on"。
    ck("g2048_ui drove the framework to open the app",
       'gui_app_run("game2048")' in t and "2048: ui=on" in t)
    m = last(t, r"2048: geom screen=(\d+)x(\d+) root=(\d+)x(\d+) board x=(\d+)\.\.(\d+) "
                r"y=(\d+)\.\.(\d+) cell=(\d+)x(\d+) right_edge=(\d+)")
    ck("geometry dump present (from g2048_info)", m is not None)
    if m:
        g = m  # already a tuple of strings from findall
        ck("screen is 390x450", (g[0], g[1]) == ("390", "450"), g[:2])
        ck("root covers the whole screen", (g[2], g[3]) == ("390", "450"), g[2:4])
        ck("board 33..356 (x) and 89..412 (y)",
           (g[4], g[5], g[6], g[7]) == ("33", "356", "89", "412"), g[4:8])
        ck("cell is 71x71", (g[8], g[9]) == ("71", "71"), g[8:10])
        ck("right_edge 357 clears the swipe-back bar (357 < 374)",
           g[10] == "357" and int(g[10]) < 374, g[10])
    m = last(t, r"keep_timer=(0x[0-9a-f]+)")
    ck("keep-awake timer exists (non-NULL)", m is not None and m != "0x0", m)

    # ── 0b. 顶部两个分数面板 ───────────────────────────────────────
    # 这块真机上出过**两次**事，所以现在要同时验三件事：
    #
    #   ① 「汉字小标题和分数叠在一起」 —— label 占的高度是「行高」不是字号
    #      （SDK 的公式 size + size/3 + 4：16px -> 25px、20px -> 30px）。
    #      面板 46px 装 25+30 那版就是靠这一条抓出来的。
    #   ② 「分数框和最佳框底部和方格框重叠了」 —— 那其实**不是真叠**，
    #      是余量正好 0px：两个 #BBADA0 的方块贴在一起，边线糊成一条。
    #      所以这里要的是 gap > 0，不是 gap >= 0。
    #   ③ 「让那两个框顶部居中」 —— 最容易悄悄回归的一条（改个偏移就偏了）。
    #
    # 所有数字都是 dump_panel_geom() / dump_geometry() 从 LVGL 的 coords
    # 读回来的真值，不是回显宏 —— 回显期望值等于没验。
    PANEL_RE = (r"panel\[%s\] box x=(-?\d+)\.\.(-?\d+) y=(-?\d+)\.\.(-?\d+) \| "
                r"cap y=(-?\d+)\.\.(-?\d+) h=(-?\d+) \| "
                r"val y=(-?\d+)\.\.(-?\d+) h=(-?\d+) \| gap=(-?\d+) (\w+)")
    PANEL_X = {"score": ("111", "190"), "best": ("199", "278")}
    for tag in ("score", "best"):
        m = last(t, PANEL_RE % tag)
        ck("panel[%s] geometry printed" % tag, m is not None, tag)
        if not m:
            continue
        ck("panel[%s] caption box ends above value box (cap.y2 < val.y1)" % tag,
           int(m[5]) < int(m[7]), (m[5], m[7]))
        ck("panel[%s] gap >= 0 and verdict OK" % tag,
           int(m[10]) >= 0 and m[11] == "OK", (m[10], m[11]))
        ck("panel[%s] caption height = 25 (16 + 16/3 + 4)" % tag, int(m[6]) == 25, m[6])
        ck("panel[%s] value height = 30 (20 + 20/3 + 4)" % tag, int(m[9]) == 30, m[9])
        ck("panel[%s] stays inside the screen" % tag, int(m[3]) < 450, m[3])
        ck("panel[%s] x is %s..%s" % ((tag,) + PANEL_X[tag]),
           (m[0], m[1]) == PANEL_X[tag], (m[0], m[1]))
        ck("panel[%s] top is 28 (was 34)" % tag, m[2] == "28", m[2])
        ck("panel[%s] height is 55 (= 25 + 30)" % tag,
           int(m[3]) - int(m[2]) + 1 == 55, (m[2], m[3]))
    ck("score-panel overlap check PASS", "score-panel overlap check: PASS" in t)

    # ── 0c. 面板和棋盘之间那 6px 的缝（这次真机返工的主角）─────────
    m = last(t, r"top band y=(\d+)\.\.(\d+) \(h=(\d+)\) board_top=(\d+) gap=(-?\d+)")
    ck("top band is 28..82 (h=55), board_top=89, gap=6  <- this used to be 0",
       m == ("28", "82", "55", "89", "6"), m)
    if m:
        ck("★ there IS a real gap between the panels and the board (gap > 0)",
           int(m[4]) > 0, m[4])
        ck("★ the gap is at least 4px, so the two blocks do not read as glued",
           int(m[4]) >= 4, m[4])

    # ── 0d. 两个面板是不是真的水平居中 ──────────────────────────────
    # 「顶部居中」是真机反馈明确要的效果；光看源码里的宏不算验证。
    m = last(t, r"panels x=(\d+)\.\.(\d+) and (\d+)\.\.(\d+) \| pair_mid=(\d+) "
                r"center=(\d+) \| xgap=(-?\d+) \| (\w+)")
    ck("both panels dumped with their x range", m is not None, m)
    if m:
        ck("panel x ranges are 111..190 and 199..278",
           m[0:4] == ("111", "190", "199", "278"), m[0:4])
        ck("★ the two panels are horizontally centered", m[4] == m[5], (m[4], m[5]))
        ck("★ the firmware itself reports CENTERED", m[7] == "CENTERED", m[7])
        ck("8px gutter between the two panels", m[6] == "8", m[6])

    # 用户反馈「第0步那行可以不要」—— 日志里不该再有它
    ck("the bottom 「第 N 步」 status line is gone", "撤销可回上一步" not in t)

    # 用户反馈「其实可以不要左上角2048字样」—— 源码里也不该再有那个标题
    # （日志里没有"标题"这一行，所以只能查源码；比人眼可靠。）
    try:
        with open(os.path.join(ROOT, "lvgl", "watch", "src", "gui_apps",
                               "game2048", "main.c"),
                  encoding="utf-8", errors="replace") as f:
            code = f.read()
        ck("★ the top-left 「2048」 title is gone from the source (no TITLE_Y)",
           "TITLE_Y" not in code)
    except OSError as e:
        ck("source readable for the title check", False, e)


    # ── 1. 残局 A：2 2 4 0 -> 4 4 0 0 ─────────────────────────────
    b, a = section(t, "A.before"), section(t, "A.after")
    ck("A set up 2 2 4 0", row(b or "", 0) == ["2", "2", "4", "0"], row(b or "", 0))
    ck("A 2 2 4 0 -> 4 4 0 0  (NOT 8 0 0 0)", row(a or "", 0) == ["4", "4", "0", "0"],
       row(a or "", 0))
    ck("A scores exactly +4", b and a and score(b) == 0 and score(a) == 4,
       "%s -> %s" % (score(b or ""), score(a or "")))

    # ── 2. 残局 B：4 4 2 2 -> 8 4 0 0  ← 修正版才会对 ──────────────
    b, a = section(t, "B.before"), section(t, "B.after")
    ck("B set up 4 4 2 2", row(b or "", 0) == ["4", "4", "2", "2"], row(b or "", 0))
    ck("B 4 4 2 2 -> 8 4 0 0  (NOT 8 2 2 0 - this is the i+=2 bug)",
       row(a or "", 0) == ["8", "4", "0", "0"], row(a or "", 0))
    ck("B scores exactly +12", b and a and score(a) - score(b) == 12,
       "%s -> %s" % (score(b or ""), score(a or "")))

    # ── 3. 残局 C：4 4 4 4 -> 8 8 0 0 ─────────────────────────────
    a = section(t, "C.after")
    ck("C 4 4 4 4 -> 8 8 0 0  (NOT 8 4 4 0)", row(a or "", 0) == ["8", "8", "0", "0"],
       row(a or "", 0))

    # ── 4. 残局 D：2 2 2 2 -> 4 4 0 0 ─────────────────────────────
    a = section(t, "D.after")
    ck("D 2 2 2 2 -> 4 4 0 0", row(a or "", 0) == ["4", "4", "0", "0"], row(a or "", 0))

    # ── 5. 残局 E：按列抽线那条分支（上滑）────────────────────────
    a = section(t, "E.after")
    ck("E column 2 2 4 up -> 4 4  (the column path works too)",
       row(a or "", 0) == ["4", "0", "0", "0"] and row(a or "", 1) == ["4", "0", "0", "0"],
       "%s / %s" % (row(a or "", 0), row(a or "", 1)))

    # ── 6. 残局 F：划不动的一步 ──────────────────────────────────
    b, a = section(t, "F.before"), section(t, "F.after")
    ck("F packed row 2 4 8 16", row(b or "", 0) == ["2", "4", "8", "16"], row(b or "", 0))
    ck("F 'move left -> no change' reported", "move left -> no change" in (section(t, "F.move_l") or ""))
    ck("F no-op move does not score", score(a) == score(b), "%s -> %s" % (score(b or ""), score(a or "")))

    # ── 7. 撤销：三步走 ──────────────────────────────────────────
    # ⚠ 原来这里断的是"撤销后 moves 正好是 8"这种**绝对值**，它依赖
    #   _ser2048.py 前面的步数，也依赖 app 是 create 还是 resume 进来的 ——
    #   2026-09-23 改了几处场景它就变成 3 条假失败。现在只断**不变量**。
    ck("G0: after F.clear + a no-op move, there is nothing to undo",
       "nothing to undo" in (section(t, "G0.undo") or ""),
       (section(t, "G0.undo") or "")[:60])
    b, mvd, a = section(t, "G1.before"), section(t, "G1.moved"), section(t, "G1.after")
    ck("G1 set up 2 2 0 0", row(b or "", 0) == ["2", "2", "0", "0"], row(b or "", 0))
    ck("G1 move left -> 4 0 0 0", row(mvd or "", 0) == ["4", "0", "0", "0"],
       row(mvd or "", 0))
    ck("G1 scores +4", b and mvd and score(mvd) - score(b) == 4,
       (score(b or ""), score(mvd or "")))
    ck("G1 ★ undo restores the board", row(a or "", 0) == ["2", "2", "0", "0"],
       row(a or "", 0))
    ck("G1 ★ undo restores the score", b and a and score(a) == score(b),
       (score(b or ""), score(a or "")))
    ck("G2: undo is only one step deep (second undo says nothing to undo)",
       "nothing to undo" in (section(t, "G2.undo") or ""),
       (section(t, "G2.undo") or "")[:60])

    # ── 8. 真机触摸：真手指划屏真的驱动了棋盘 ───────────────────
    # ⚠ 要取**最后一次**统计。第一行 info0 里 down/up/swipe 全是 0，
    #   re.search 会命中那一条，然后断言永远失败（或者反过来永远"通过"）。
    # ⚠ 变量名不能叫 last —— 那会遮蔽上面那个 last() 函数，后面再加一句
    #   last(...) 就会静默拿到字符串。叫 tlast。
    touches = re.findall(r"touch down=(\d+) up=(\d+) swipe=(\d+)", t)
    tlast = touches[-1] if touches else None
    ck("touch counters printed at all", tlast is not None, len(touches))
    ck("swipes reached the app (last snapshot)",
       tlast and int(tlast[0]) > 0 and int(tlast[2]) > 0, tlast)
    # 这一条只能证明"点下去确实走到 do_undo() 了"，证不了撤销本身有效
    # （那一步多半是 nothing to undo）。有效的撤销由上面 G1 那两组断言负责。
    ck("the 撤销 button (105,430) actually reached do_undo()",
       "2048: undo ->" in (section(t, "H.click2") or ""))

    # ── 8b. ★ tp_ctrl slide：真手指滑动真的推动了棋盘 ───────────
    # 前面那些 g2048_move 走的是 shell 线程直接调逻辑，证明不了输入通路。
    # 这一条是 DOWN -> MOVE -> UP 从触摸驱动注入的，棋盘必须跟着变。
    lb, la = section(t, "L.before"), section(t, "L.after")
    ck("L board set up 2 2 0 0", row(lb or "", 0) == ["2", "2", "0", "0"],
       row(lb or "", 0))
    ck("L ★ a real swipe (right) moved the board: 2 2 0 0 -> 0 0 0 4",
       row(la or "", 0) == ["0", "0", "0", "4"], row(la or "", 0))
    ck("L the swipe scored +4", lb and la and score(la) - score(lb) == 4,
       "%s -> %s" % (score(lb or ""), score(la or "")))

    # ── 9. app 切出去再回来：接着下，**不是重开一局** ──────────────
    # 框架有两条合法路径，都算"没重开"，所以断言写成 or，别只认一条：
    #   · app 还在运行列表里 -> 框架直接 "Resume it"（不打 on_start）
    #   · app 被卸载了       -> 重新 on_start，那时 app 自己判断
    #                           tile_count() != 0 并打 "resume in-progress game"
    # （2026-09-23 的教训：只认后者，结果发现前一版的检查在"Resume"路径下
    #   必然假失败。要断的是"没重开"这个**语义**，不是某一句日志。）
    kb = section(t, "K.back") or ""
    ck("K.back: switching back did NOT start a fresh game",
       ("Resume it" in kb) or ("resume in-progress game" in kb), kb[:100])
    ck("K.back: the app is on screen again (ui=on)",
       "2048: ui=on" in (section(t, "K.info") or ""))

    # ── 10. 新局重播种 ──────────────────────────────────────────
    seeds = re.findall(r"new game seed=0x([0-9A-F]+)", t)
    ck("each new game reseeds the RNG", len(seeds) >= 2 and len(set(seeds)) == len(seeds),
       seeds)
    a = section(t, "I.after")
    ck("I new game -> score 0 tiles 2", a and score(a) == 0 and "tiles=2" in a,
       score(a or ""))
    ck("I 自动生成新块恢复后，新局还是 2 颗", "tiles=2" in (section(t, "I.after2") or ""),
       (section(t, "I.after2") or "")[:60])

    # ── 11. 蜂窝里真的多了一格（icon 传 NULL 就不会有这条）──────
    m = re.search(r"calc: cell\s+(\d+)\s+id=game2048\s+icon=(0x[0-9a-f]+)\s+(\S+)",
                  section(t, "J.menu") or "")
    ck("game2048 is in the honeycomb with a REAL icon",
       m is not None and m.group(2) != "0x0" and m.group(3) == "drawn",
       m.groups() if m else None)
    m2 = re.search(r"calc: (\d+) builtin apps", section(t, "J.menu") or "")
    ck("builtin app count is 7 (was 6 before 2048)", m2 and m2.group(1) == "7",
       m2.group(1) if m2 else None)

    fails = 0
    for name, ok, detail in checks:
        print(("  PASS  " if ok else "  FAIL  ") + name + ("" if ok else "   <- got %r" % (detail,)))
        if not ok:
            fails += 1

    print("")
    if fails == 0:
        print("ALL PASS  (%d checks)" % len(checks))
        return 0
    print("FAILED: %d of %d" % (fails, len(checks)))
    return 1


if __name__ == "__main__":
    sys.exit(main())
