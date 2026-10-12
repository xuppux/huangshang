# -*- coding: utf-8 -*-
r"""
_ser_wake.py —— 用 DTR/RTS 脉冲把睡死的手表复位叫醒

背景：手表深度睡眠后 uart1 断电，串口发什么都没回显。
技能库里记着「DTR/RTS 拉高会复位芯片」（所以平时读串口必须把它们置低）。
这里反过来用：**故意脉冲一下当作复位按钮**，看能不能把板子叫醒。

复位后板子会重新走开机流程：RTC 归零 -> 自动开广播（见 ble_app.c
ble_on_power_on 里那个「年份 <= 2024 就当没校过」的判断）。
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


def read_for(ser, seconds, stop_on=(b"SFBL", b"msh />", b"Serial PowerOn")):
    buf = b""
    t0 = time.time()
    hit = None
    while time.time() - t0 < seconds:
        n = ser.in_waiting
        if n:
            chunk = ser.read(n)
            buf += chunk
            for s in stop_on:
                if s in chunk:
                    hit = s
                    break
            if hit:
                break
        else:
            time.sleep(0.03)
    return buf, hit


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

# 先看看现在是不是真的哑的
ser.write(b"\r")
buf, _ = read_for(ser, 1.5)
p("[复位前] 收到 %d 字节: %r" % (len(buf), buf[:120]))
p("")

woke = False
for name, fn in (
    ("DTR 脉冲", lambda: (setattr(ser, "dtr", True),  time.sleep(0.25), setattr(ser, "dtr", False))),
    ("RTS 脉冲", lambda: (setattr(ser, "rts", True),  time.sleep(0.25), setattr(ser, "rts", False))),
    ("DTR+RTS 同时脉冲", lambda: (setattr(ser, "dtr", True), setattr(ser, "rts", True),
                              time.sleep(0.25),
                              setattr(ser, "dtr", False), setattr(ser, "rts", False))),
    ("DTR 低->高->低 两次", lambda: [(setattr(ser, "dtr", True), time.sleep(0.12),
                                 setattr(ser, "dtr", False), time.sleep(0.12)) for _ in range(2)]),
):
    p("[试] %s" % name)
    try:
        fn()
    except Exception as e:
        p("      失败: %r" % (e,))
        continue
    ser.reset_input_buffer()
    buf, hit = read_for(ser, 4.0)
    p("     复位后收到 %d 字节，命中=%s" % (len(buf), hit))
    if buf:
        p("     ---- 内容（前 1500 字节）----")
        p(buf[:1500].decode("utf-8", errors="replace"))
    p("")
    if hit:
        woke = True
        break

if woke:
    p("[结果] 板子醒了 ✓  再抓 12 秒看蓝牙起来没有")
    buf, _ = read_for(ser, 12.0, stop_on=(b"ble: ", b"advertising", b"msh />"))
    p(buf.decode("utf-8", errors="replace"))
else:
    p("[结果] 三种脉冲都没叫醒。只能物理复位了（按 KEY1 / 碰一下屏幕），")
    p("       或者重新烧一遍固件（sftool 自己会复位）。")

ser.close()
p("")
p("[串口已关]")

sys.stdout.write(out.getvalue())
open(r"C:\Users\18058\Desktop\huangshanpai\watch\_ser_wake.txt", "w", encoding="utf-8").write(out.getvalue())
