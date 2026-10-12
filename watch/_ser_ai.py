# -*- coding: utf-8 -*-
r"""
_ser_ai.py —— 抓手表串口日志，读 ai_info / ai_recv

DTR/RTS 必须显式置低：pyserial 的 SerialBase.__init__ 把它们初始化成 True，
而这块板子 DTR/RTS 拉高 = 复位。一直拉高 = 一直摁在复位里，
症状是「端口能打开、不报错、但一个字节都收不到」。
"""
import io
import os
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

p("[串口已开 %s @ %d]" % (PORT, BAUD))

# 先打个裸 CR：finsh 收到 CR 会重画提示符。
# 不然提示符早在开机时就打完了，永远等不到。
ser.write(b"\r")
time.sleep(0.5)


def drain(t=0.4):
    buf = b""
    t0 = time.time()
    while time.time() - t0 < t:
        try:
            n = ser.in_waiting
        except Exception:
            n = 0
        if n:
            buf += ser.read(n)
            t0 = time.time()
        else:
            time.sleep(0.03)
    return buf


def send(cmd, wait=0.9):
    p("")
    p("$ " + cmd)
    ser.write((cmd + "\r").encode("ascii"))
    raw = drain(wait)
    txt = raw.decode("utf-8", errors="replace")
    p(txt)
    return txt


drain(0.8)   # 先清掉缓冲区里的历史输出

# keepawake 要在业务命令之前 —— 板子一睡，串口业务命令就没反应了
send("keepawake 120")
send("ai_info")
send("ai_recv")
send("ai_echo")

ser.close()
p("")
p("[串口已关]")

sys.stdout.write(out.getvalue())
open(r"C:\Users\18058\Desktop\huangshanpai\watch\_ser_ai.txt", "w", encoding="utf-8").write(out.getvalue())
