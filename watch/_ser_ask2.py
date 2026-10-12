# -*- coding: utf-8 -*-
r"""
_ser_ask2.py —— 端到端：以手表身份问一句，等大脑回答送回来

流程：
  1. RTS 脉冲复位（把睡死的板子叫醒；实测 DTR 不能复位，只有 RTS 是复位线）
  2. 等开机 + 蓝牙协议栈起来（板子会自动开广播，因为 RTC 掉了）
  3. keepawake 确保保持唤醒（⚠ 它是无参数 toggle，要看回显再决定敲不敲）
  4. 轮询 ai_info，直到 up=1（说明电脑端 serve 连上来了）
  5. ai_send <问题>
  6. 等回答，用 ai_recv 把收到的内容打出来

用法：python _ser_ask2.py "你好，你是谁"
"""
import io
import sys
import time

import serial

PORT = "COM3"
BAUD = 1000000

q = " ".join(sys.argv[1:]) or "你好，你是谁"

out = io.StringIO()


def p(*a):
    print(*a, file=out, flush=True)


# 累积所有串口原文。
# ⚠ 为什么需要一个累积器：答案可能在我们**还没进到"等回答"那一步**就回来了
#   （离线大脑只要 0.1 秒）。只看当前那一次 read 的返回，就会误报"没收到"。
#   第一版就是这么错的，日志里明明有 "ble_ai: RX#1 105 bytes" 却判 False。
SEEN = []


ser = serial.Serial()
ser.port = PORT
ser.baudrate = BAUD
ser.timeout = 0.1
ser.dsrdtr = False
ser.rtscts = False
ser.dtr = False
ser.rts = False
ser.open()
ser.dtr = False
ser.rts = False
time.sleep(0.2)


def read_for(seconds, stop_on=()):
    buf = b""
    t0 = time.time()
    while time.time() - t0 < seconds:
        n = ser.in_waiting
        if n:
            chunk = ser.read(n)
            buf += chunk
            for s in stop_on:
                if s in chunk:
                    return buf, s
        else:
            time.sleep(0.03)
    return buf, None


def send(cmd, wait=1.0):
    p("")
    p("$ " + cmd)
    ser.write((cmd + "\r").encode("utf-8"))
    txt = read_for(wait)[0].decode("utf-8", errors="replace")
    SEEN.append(txt)
    p(txt)
    return txt


# ── 1. RTS 脉冲复位 ──────────────────────────────────────────────────
p("[1] RTS 脉冲复位…")
ser.write(b"\r")
read_for(1.0)
ser.rts = True
time.sleep(0.25)
ser.rts = False
ser.reset_input_buffer()
boot, hit = read_for(20.0, stop_on=(b"SFBL", b"msh />"))
p("    开机横幅 %d 字节，命中=%s" % (len(boot), hit))
if hit:
    p(boot.decode("utf-8", errors="replace")[:1200])

# ── 2. 等蓝牙协议栈起来（板子会自己开广播：RTC 掉了 -> want_on=1）────
p("")
p("[2] 等蓝牙协议栈（约 60 秒）…")
t0 = time.time()
adv_ok = False
while time.time() - t0 < 120:
    buf = read_for(4.0)[0].decode("utf-8", errors="replace")
    for ln in buf.splitlines():
        if "advertis" in ln or "ble:" in ln or "state=" in ln:
            p("    " + ln.strip())
    state = send("ble", 1.0)
    if "state=advertising" in state or "adv_on=1" in state:
        adv_ok = True
        break
    if "state=connected" in state:
        adv_ok = True
        p("    （已经被连上了）")
        break
p("    广播就绪 = %s" % adv_ok)

# ── 3. keepawake（无参数 toggle）─────────────────────────────────────
p("")
p("[3] keepawake")
t = send("keepawake")
if "enabled (10s)" in t:
    t = send("keepawake")
    p("    -> " + ("已保持唤醒 ✓" if "DISABLED" in t else "还是没开成 ??"))

# ── 4. 等电脑端连上来 ────────────────────────────────────────────────
p("")
p("[4] 等电脑端 serve 连上来…")
up = False
t0 = time.time()
while time.time() - t0 < 120:
    t = send("ai_info", 1.2)
    if "up=1" in t:
        up = True
        p("    -> 已连上 ✓")
        break
    p("    ...还没连上（%.0f 秒）" % (time.time() - t0))
p("    连上 = %s" % up)

if not up:
    p("")
    p("[停止] 电脑端一直没连上来。检查 serve 是不是在跑、是不是也报错了。")
    ser.close()
    sys.stdout.write(out.getvalue())
    open(r"C:\Users\18058\Desktop\huangshanpai\watch\_ser_ask2.txt", "w",
         encoding="utf-8").write(out.getvalue())
    raise SystemExit(1)

# ── 5. 提问 ──────────────────────────────────────────────────────────
p("")
p("[5] 以手表身份提问：%s" % q)
send("ai_send " + q, 2.5)

# ── 6. 等回答 ────────────────────────────────────────────────────────
# 判据用累积器（见 SEEN 的注释）：答案可能在第 5 步的等待窗口里就回来了。
p("")
p("[6] 等大脑的回答（最多 60 秒）…")
t0 = time.time()
answered = any("ble_ai: RX#" in s for s in SEEN)
if answered:
    p("    （答案在第 5 步的窗口里就到了）")
while not answered and time.time() - t0 < 60:
    buf = read_for(3.0)[0].decode("utf-8", errors="replace")
    SEEN.append(buf)
    for ln in buf.splitlines():
        ln = ln.strip()
        if ln:
            p("    " + ln)
    if "ble_ai: RX#" in buf:
        answered = True
        break

p("")
p("    收到回答 = %s" % answered)
send("ai_recv", 2.0)

ser.close()
p("")
p("[串口已关]")

sys.stdout.write(out.getvalue())
open(r"C:\Users\18058\Desktop\huangshanpai\watch\_ser_ask2.txt", "w",
     encoding="utf-8").write(out.getvalue())
