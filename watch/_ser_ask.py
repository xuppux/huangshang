# -*- coding: utf-8 -*-
r"""
_ser_ask.py —— 以手表身份问一句（串口跑 ai_send）

用法：python _ser_ask.py "你好"

这一步是「模拟手表发起提问」。等手表上有了问答页，就不需要它了。
DTR/RTS 必须显式置低 —— pyserial 默认拉高，会把板子摁在复位里。
"""
import io
import sys
import time

import serial

PORT = "COM3"
BAUD = 1000000

q = " ".join(sys.argv[1:]) or "你好"

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


def send(cmd, wait=0.9):
    p("")
    p("$ " + cmd)
    ser.write((cmd + "\r").encode("utf-8"))
    p(drain(wait).decode("utf-8", errors="replace"))


drain(0.8)

send("keepawake 300")
send("ai_info")
send("ai_send " + q)

# 等大脑回答回来（离线大脑很快，真模型可能要几秒到几十秒）
p("")
p("[等大脑的回答… 20 秒]")
p(drain(20.0).decode("utf-8", errors="replace"))

send("ai_recv")

ser.close()
p("")
p("[串口已关]")

sys.stdout.write(out.getvalue())
open(r"C:\Users\18058\Desktop\huangshanpai\watch\_ser_ask.txt", "w", encoding="utf-8").write(out.getvalue())
