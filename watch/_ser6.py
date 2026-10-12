# -*- coding: utf-8 -*-
"""
_ser6.py -- serial bring-up for the fixed (v6) torch firmware.

What we are verifying this round:
  * the board still boots and reaches the MSH shell
  * `torch_ui` can enter the app
  * the WS2812 4-step chain now actually reaches DMA:
        proj.conf macro -> rtconfig.h -> bf0_pwm_obj[PWM3].pwm_update_dma
        -> HAL_GPT_PWM_Update_Start_DMA
    We cannot see the LED from here, so we look for the *absence* of the old
    signature (NULL dma -> plain HAL_GPT_PWM_Start) and for a clean ret=0 with
    the DMA path taken.

Output: _ser6.raw (byte-exact latin-1) and _ser6.txt (cleaned, CR->LF collapsed)
"""
import sys
import time

import serial

PORT = "COM3"
BAUD = 1000000

# (label, command, wait_seconds)
STEPS = [
    ("boot",  None,                        7.0),   # drain reset + boot log
    # 空命令 = 只发一个 CR。finsh 收到 CR 会重画提示符；不然连上时提示符
    # 早在开机时就打完了，我们永远等不到它。（serial.ps1 里同样的手法）
    ("wake",  "",                          0.8),
    ("ui",    "torch_ui",                  2.5),
    ("info1", "torch_info",                1.2),
    ("act",   "torch_act",                 1.5),
    ("tree",  "torch_tree",                1.5),
    ("rawR",  "torch_raw FF0000",          1.2),
    ("led1",  "torch_led 1",               1.2),
    ("tapA",  "tp_ctrl click 195 300",     1.5),
    ("info2", "torch_info",                1.2),
    ("tapB",  "tp_ctrl click 195 300",     1.5),
    ("info3", "torch_info",                1.2),
    ("tapC",  "tp_ctrl click 195 300",     1.5),
    ("info4", "torch_info",                1.2),
    ("ledW",  "torch_led 1",               1.2),
    ("rawW",  "torch_raw FFFFFF",          1.2),
    ("info5", "torch_info",                1.2),
]


def main():
    # ⚠ pyserial 的 SerialBase 默认 _dtr_state / _rts_state 都是 True，
    #   打开端口时会真的把 DTR/RTS 拉高 —— 而这块板子 DTR/RTS 拉高就等于
    #   把 MCU 一直摁在复位里，表现是"端口打得开、一个字节都收不到"。
    #   serial.ps1 里也是显式 DtrEnable=$false / RtsEnable=$false，同一个原因。
    #   所以：打开后立刻拉低，再等它重启 + 走完 boot。
    try:
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
            marker = ("\n===== %s : %s =====\n" % (label, cmd)).encode("ascii")
            raw.extend(marker)
            ser.write(cmd.encode("ascii") + b"\r")
        drain(wait)

    ser.close()

    with open(r"C:\Users\18058\Desktop\huangshanpai\watch\_ser6.raw", "wb") as f:
        f.write(bytes(raw))

    text = bytes(raw).decode("latin-1").replace("\r", "")
    lines = []
    for ln in text.split("\n"):
        # strip the ANSI colour codes MSH emits on its prompt
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

    with open(r"C:\Users\18058\Desktop\huangshanpai\watch\_ser6.txt", "w",
              encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")

    print("OK bytes=%d" % len(raw))
    return 0


if __name__ == "__main__":
    sys.exit(main())
