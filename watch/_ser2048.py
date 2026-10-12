# -*- coding: utf-8 -*-
"""
_ser2048.py -- 2048 app 的真机串口验证。

验什么（按重要性排）：

  1. 固件起来、能进这个 app（g2048_ui），几何自检打印对不对
     —— 尤其 board 右边界 357 < 374（返回条），这条错了的后果是"最右列划不动"。
  2. **合并规则**。这是唯一一件屏幕上肉眼分辨不出对错的事：
       2 2 4 0 往左   -> 4 4 0 0     （不是 8 0 0 0）
       4 4 2 2 往左   -> 8 4 0 0     （不是 8 2 2 0）
       4 4 4 4 往左   -> 8 8 0 0     （不是 8 4 4 0）
     后两条是**修正版**才有的：第一版固件 line_step() 写的是 i += 2，
     会把挪过来的那一对跳过。这三条都是 ui-preview/game2048.html 自检里
     已经跑通的用例 —— 串口这边只是确认 C 版和 JS 版是同一个结果。
  3. 上/下滑走的是另一条 switch 分支（按列抽线），单独验一条。
  4. 划不动的一步不加分、不改盘。
  5. 撤销能把盘和分数一起还原。
  6. 防息屏定时器确实在（keep_timer 非 NULL）—— 这个 app 是"盯着想"的用法，
     没有它 10 秒后自己黑屏。
  7. **真手指滑动**（tp_ctrl slide）：前面几条都是 shell 线程直接调逻辑，
     只有这一条走的是"触摸驱动 → LVGL → root 事件"的完整通路。
  8. 切出去再切回来接着下（不是重开一局）。

⚠ pyserial 默认把 DTR/RTS 拉高，这块板子被摁在复位里 —— 端口打得开、0 字节。
   必须 open 前后各压一次 dtr/rts=False。（见技能库 huangshanpai-watch-lvgl-face）

输出：_ser2048.raw（字节精确）+ _ser2048.txt（去 CR / 去 ANSI 色码）
"""
import sys
import time

import serial

PORT = "COM3"
BAUD = 1000000

# (label, command, wait_seconds)   command=None 只打分隔符
STEPS = [
    ("boot",      None,                        8.0),
    ("wake",      "",                          0.8),   # 裸 CR，让 finsh 重画提示符

    # ⚠ 开日志。s_evt_log 是 app 里的 static，**跨运行不会自己复位** ——
    #   上一轮末尾那句 `g2048_log 0` 会留到这一轮，于是 "move ... -> ..."
    #   这些行一条都不打，断言全变假失败。（2026-09-23 真踩过：
    #   第一轮 27/27 通过，第二轮同一份固件却多出几条 FAIL。）
    ("log_on",    "g2048_log 1",               0.5),

    ("ui",        "g2048_ui",                  2.5),
    ("info0",     "g2048_info",                1.2),

    # ── 残局 A：判"合并后下标不动"这个错 ──
    ("A.spawn0",  "g2048_spawn 0",             0.5),
    ("A.clear",   "g2048_clear",               0.5),
    ("A.set1",    "g2048_set 0 0 2",           0.5),
    ("A.set2",    "g2048_set 0 1 2",           0.5),
    ("A.set3",    "g2048_set 0 2 4",           0.5),
    ("A.before",  "g2048_info",                1.0),
    ("A.move_l",  "g2048_move l",              1.2),
    ("A.after",   "g2048_info",                1.0),

    # ── 残局 B：判"合并后 i += 2"这个错（更隐蔽）──
    ("B.clear",   "g2048_clear",               0.5),
    ("B.set1",    "g2048_set 0 0 4",           0.5),
    ("B.set2",    "g2048_set 0 1 4",           0.5),
    ("B.set3",    "g2048_set 0 2 2",           0.5),
    ("B.set4",    "g2048_set 0 3 2",           0.5),
    ("B.before",  "g2048_info",                1.0),
    ("B.move_l",  "g2048_move l",              1.2),
    ("B.after",   "g2048_info",                1.0),

    # ── 残局 C：4 4 4 4 ──
    ("C.clear",   "g2048_clear",               0.5),
    ("C.set1",    "g2048_set 0 0 4",           0.5),
    ("C.set2",    "g2048_set 0 1 4",           0.5),
    ("C.set3",    "g2048_set 0 2 4",           0.5),
    ("C.set4",    "g2048_set 0 3 4",           0.5),
    ("C.move_l",  "g2048_move l",              1.2),
    ("C.after",   "g2048_info",                1.0),

    # ── 残局 D：2 2 2 2 ──
    ("D.clear",   "g2048_clear",               0.5),
    ("D.set1",    "g2048_set 0 0 2",           0.5),
    ("D.set2",    "g2048_set 0 1 2",           0.5),
    ("D.set3",    "g2048_set 0 2 2",           0.5),
    ("D.set4",    "g2048_set 0 3 2",           0.5),
    ("D.move_l",  "g2048_move l",              1.2),
    ("D.after",   "g2048_info",                1.0),

    # ── 残局 E：走上/下滑那条分支（按列抽线）──
    ("E.clear",   "g2048_clear",               0.5),
    ("E.set1",    "g2048_set 0 0 2",           0.5),
    ("E.set2",    "g2048_set 1 0 2",           0.5),
    ("E.set3",    "g2048_set 2 0 4",           0.5),
    ("E.move_u",  "g2048_move u",              1.2),
    ("E.after",   "g2048_info",                1.0),

    # ── 残局 F：划不动的一步 ──
    ("F.clear",   "g2048_clear",               0.5),
    ("F.set1",    "g2048_set 0 0 2",           0.5),
    ("F.set2",    "g2048_set 0 1 4",           0.5),
    ("F.set3",    "g2048_set 0 2 8",           0.5),
    ("F.set4",    "g2048_set 0 3 16",          0.5),
    ("F.before",  "g2048_info",                1.0),
    ("F.move_l",  "g2048_move l",              1.2),
    ("F.after",   "g2048_info",                1.0),

    # ── 撤销：三步走 ──
    # G0: F 那一步是"划不动"的，而 F.clear 已经把快照清了 —— 所以这里
    #     应该"没得撤"。这一条同时证明两件事：clear 会清掉快照、
    #     **划不动的一步不会制造快照**（否则这里会撤掉更早的 E 那一步）。
    ("G0.undo",   "g2048_undo",                1.2),
    ("G0.after",  "g2048_info",                1.0),

    # G1: 一步**有效**的移动 -> 撤销 -> 盘和分数都要回去
    ("G1.clear",  "g2048_clear",               0.5),
    ("G1.set1",   "g2048_set 0 0 2",           0.5),
    ("G1.set2",   "g2048_set 0 1 2",           0.5),
    ("G1.before", "g2048_info",                1.0),
    ("G1.move",   "g2048_move l",              1.2),
    ("G1.moved",  "g2048_info",                1.0),
    ("G1.undo",   "g2048_undo",                1.2),
    ("G1.after",  "g2048_info",                1.0),

    # G2: 撤销只有一步 —— 再撤一次必须说"没得撤"
    ("G2.undo",   "g2048_undo",                1.2),
    ("G2.after",  "g2048_info",                1.0),

    # ── 触摸日志：看事件本身通不通 ──
    ("H.log",     "g2048_log 1",               0.5),
    ("H.click1",  "tp_ctrl click 195 250",     1.2),
    ("H.click2",  "tp_ctrl click 105 430",     1.2),   # 撤销按钮
    ("H.info",    "g2048_info",                1.0),
    ("H.log0",    "g2048_log 0",               0.5),

    # ── L. ★ 真手指滑动 ──
    # ⚠ 前面那些 g2048_move 是在 shell 线程直接调【逻辑】的，
    #   它证明不了"滑动这条输入通路是通的"。必须让触摸驱动真的注入一次
    #   DOWN -> MOVE -> UP（tp_ctrl slide 就是干这个的，见
    #   drv_touch.c 的 "slide <x1> <y1> <x2> <y2>"），
    #   再看棋盘有没有跟着动、swipe 计数有没有涨。
    ("L.spawn0",  "g2048_spawn 0",             0.5),
    ("L.clear",   "g2048_clear",               0.5),
    ("L.set1",    "g2048_set 0 0 2",           0.5),
    ("L.set2",    "g2048_set 0 1 2",           0.5),
    ("L.before",  "g2048_info",                1.0),
    # 从左往右划 (100,250) -> (300,250)：行 0 的 2 2 0 0 往右 -> 0 0 0 4
    ("L.swipe",   "tp_ctrl slide 100 250 300 250", 1.5),
    ("L.after",   "g2048_info",                1.0),

    # ── 回归：开回自动生成、开新局 ──
    ("I.spawn1",  "g2048_spawn 1",             0.5),
    ("I.new",     "g2048_new",                 1.2),
    ("I.after",   "g2048_info",                1.0),
    # 再开一局：保证至少有两个种子可比（app 若是 resume 进来的，
    # 这一轮就没有"开机时的第一次新局"了）
    ("I.new2",    "g2048_new",                 1.2),
    ("I.after2",  "g2048_info",                1.0),

    # ── 顺手确认蜂窝里有几格（图标没传 NULL 才会进蜂窝）──
    ("J.menu",    "calc_menu",                 1.5),

    # ── K. 切出去再切回来：接着下，不重开局 ──
    # （J.menu 已经把 app 切走了；这里切回来应该看到
    #   "resume in-progress game" + starts 递增，而不是全新的两颗子）
    ("K.back",    "g2048_ui",                  2.5),
    ("K.info",    "g2048_info",                1.0),
]

RAW = r"C:\Users\18058\Desktop\huangshanpai\watch\_ser2048.raw"
TXT = r"C:\Users\18058\Desktop\huangshanpai\watch\_ser2048.txt"


def main():
    try:
        ser = serial.Serial()
        ser.port = PORT
        ser.baudrate = BAUD
        ser.timeout = 0.2
        ser.dsrdtr = False
        ser.rtscts = False
        ser.dtr = False          # ⚠ 见文件头：不压下去 = 板子被摁在复位里
        ser.rts = False
        ser.open()
        ser.dtr = False
        ser.rts = False
    except Exception as e:
        print("OPEN-FAIL %r" % (e,))
        return 2

    raw = bytearray()

    def drain(sec):
        end = time.time() + sec
        while time.time() < end:
            n = ser.in_waiting
            chunk = ser.read(n if n else 1)
            if chunk:
                raw.extend(chunk)

    for label, cmd, wait in STEPS:
        if cmd is None:
            raw.extend(("\n===== %s =====\n" % label).encode("ascii"))
        else:
            raw.extend(("\n===== %s : %s =====\n" % (label, cmd)).encode("ascii"))
            ser.write(cmd.encode("ascii") + b"\r")
        drain(wait)

    ser.close()

    with open(RAW, "wb") as f:
        f.write(bytes(raw))

    text = bytes(raw).decode("latin-1").replace("\r", "")
    lines = []
    for ln in text.split("\n"):
        out = []
        i = 0
        while i < len(ln):
            if ln[i] == "\x1b" and i + 1 < len(ln) and ln[i + 1] == "[":
                j = i + 2
                while j < len(ln) and not ln[j].isalpha():
                    j += 1
                i = j + 1
                continue
            out.append(ln[i])
            i += 1
        lines.append("".join(out).rstrip())

    with open(TXT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")

    print("OK bytes=%d" % len(raw))
    return 0


if __name__ == "__main__":
    sys.exit(main())
