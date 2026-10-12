# -*- coding: utf-8 -*-
r"""
_ser_diag.py —— 看手表状态，并把「保持唤醒 + 开广播」正确设上

⚠ 关键教训：`keepawake` 是**无参数 toggle**，不是「保持 N 秒」。
   `keepawake 120` 的做法纯属碰巧 —— 第一次从关切到开，第二次又切回关。
   正确做法：先读它的回显，只在显示 "enabled" 时才再敲一次。

同理 `ble` 是状态查询，`ble on` / `ble off` 才是动作。
"""
import io
import sys
import time

import serial

PORT = "COM3"
BAUD = 1000000

out = io.StringIO()


def p(*a):
    print(*a, file=out, flush=True)


ser = serial.Serial()
ser.port = PORT
ser.baudrate = BAUD
ser.timeout = 0.2
ser.dsrdtr = False
ser.rtscts = False
ser.dtr = False
ser.rts = False
ser.open()
ser.dtr = False
ser.rts = False

ser.write(b"\r")
time.sleep(0.4)


def drain(t=0.4):
    buf = b""
    t0 = time.time()
    while time.time() - t0 < t:
        n = ser.in_waiting
        if n:
            buf += ser.read(n)
            t0 = time.time()
        else:
            time.sleep(0.03)
    return buf


def send(cmd, wait=1.0):
    p("")
    p("$ " + cmd)
    ser.write((cmd + "\r").encode("utf-8"))
    txt = drain(wait).decode("utf-8", errors="replace")
    p(txt)
    return txt


drain(1.0)

# 1) 先把板子叫醒：任何一条命令都能让串口活着，但要让板子**不睡**得开 keepawake。
#    先盲敲一次，再看状态；如果是 enabled 就再敲一次切成 DISABLED。
t = send("keepawake")
if "enabled (10s)" in t:
    p("[诊断] keepawake 现在是关的（息屏可用）-> 再切一次")
    t = send("keepawake")
    if "DISABLED" in t:
        p("[诊断] 现在已保持唤醒 ✓")
elif "DISABLED" in t:
    p("[诊断] 已经是保持唤醒 ✓")

# 2) 蓝牙状态
send("ble")

# 3) 确保在广播（息屏后射频会一起断电，所以广播要重新开）
t = send("ble on", 1.5)
send("ble")

ser.close()
p("")
p("[串口已关]")

sys.stdout.write(out.getvalue())
open(r"C:\Users\18058\Desktop\huangshanpai\watch\_ser_diag.txt", "w", encoding="utf-8").write(out.getvalue())
