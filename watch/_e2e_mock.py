# -*- coding: utf-8 -*-
r"""
_e2e_mock.py —— 真机 + 真 HTTP 的端到端验证

为什么要它：
    前面 _test_real_model_path.py 验的是「电脑这一侧」，
    _ser_ask2.py 验的是「手表↔电脑」这条蓝牙链路（但大脑是离线的）。
    这个脚本把两段接起来：让「大脑」真的走一次 HTTP 请求，
    再让答案原路回到手表。

链路：
    手表串口 ai_send  →  蓝牙  →  电脑 serve  →  HTTP POST  →  本地假模型
                                        ←───────── 原路返回 ─────────┘

用本地假模型而不是真远端的原因：不花钱、不依赖 key、不依赖网络，
一旦这一步过了，剩下唯一的变量就真的只剩「你的 key 和你的网络」。

跑法：
    python _e2e_mock.py "1+2等于几"
"""

import io
import json
import os
import re
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
PY = sys.executable

PORT_SER = "COM3"
BAUD = 1000000

QUESTION = " ".join(sys.argv[1:]) or "1+2等于几"

OUT = io.StringIO()
LOG_MOCK = os.path.join(HERE, "_e2e_mock_server.log")
LOG_SERVE = os.path.join(HERE, "_e2e_serve.log")
CFG = os.path.join(HERE, "_e2e_cfg.json")
REPORT = os.path.join(HERE, "_e2e_mock.txt")


def p(*a):
    s = " ".join(str(x) for x in a)
    print(s, flush=True)
    OUT.write(s + "\n")


# ══════════════════════════════════════════════════════════════════════
#  本地假模型（OpenAI 兼容）
# ══════════════════════════════════════════════════════════════════════

class Mock(BaseHTTPRequestHandler):
    hits = []

    def log_message(self, *a):
        pass

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b""
        q = ""
        try:
            q = json.loads(raw.decode("utf-8", "replace"))["messages"][-1]["content"]
        except Exception:
            pass
        auth = self.headers.get("Authorization") or ""
        Mock.hits.append({"q": q, "auth": auth, "path": self.path})
        with open(LOG_MOCK, "a", encoding="utf-8") as f:
            f.write("收到请求 path=%s auth=%s q=%r\n" % (self.path, auth[:18] + "...", q))

        # ★ 答案里带上问题原文 —— 这样手表上看到的字就能证明
        #   「问题真的出去了、答案真的回来了」，而不是某个写死的字符串。
        answer = "【HTTP假模型】你问的是「%s」。这条路走的是真 HTTP 接口。" % q[:24]
        body = json.dumps({
            "id": "mock", "object": "chat.completion",
            "choices": [{"index": 0, "finish_reason": "stop",
                         "message": {"role": "assistant", "content": answer}}],
        }, ensure_ascii=False).encode("utf-8")

        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


# ══════════════════════════════════════════════════════════════════════
#  串口
# ══════════════════════════════════════════════════════════════════════

ser = serial.Serial()
ser.port = PORT_SER
ser.baudrate = BAUD
ser.timeout = 0.1
ser.dsrdtr = False
ser.rtscts = False
ser.dtr = False
ser.rts = False

SEEN = []


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
    p("  $ " + cmd)
    ser.write((cmd + "\r").encode("utf-8"))
    txt = read_for(wait)[0].decode("utf-8", errors="replace")
    SEEN.append(txt)
    for ln in txt.splitlines():
        if ln.strip():
            p("      " + ln.strip())
    return txt


# ══════════════════════════════════════════════════════════════════════

def main():
    for f in (LOG_MOCK, LOG_SERVE):
        if os.path.exists(f):
            os.remove(f)

    p("=" * 70)
    p("  端到端：手表 → 蓝牙 → 电脑 → HTTP(假模型) → 手表")
    p("=" * 70)

    # ── 起假模型 ──────────────────────────────────────────────────
    port = free_port()
    srv = HTTPServer(("127.0.0.1", port), Mock)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    p("\n[0] 假模型已起在 http://127.0.0.1:%d/v1" % port)

    with open(CFG, "w", encoding="utf-8") as f:
        json.dump({
            "provider": "openai",
            "api_key": "sk-mock-e2e-0000",
            "base_url": "http://127.0.0.1:%d/v1" % port,
            "model": "mock-e2e",
            "timeout": 15,
            "proxy": "none",          # 别让本机代理插进来
        }, f, ensure_ascii=False, indent=2)
    p("    临时配置：%s（没动你的 ai_brain_config.json）" % os.path.basename(CFG))

    # ── 起 serve（先起，让它一直扫，手表一广播就能连上）────────────
    env = dict(os.environ)
    env["AI_BRAIN_CONFIG"] = CFG
    env["BLE_AI_LOG"] = LOG_SERVE
    env["PYTHONIOENCODING"] = "utf-8"

    # ⚠ 子进程的 stdout 要单独一个文件，不能和 BLE_AI_LOG 指向同一个：
    #   这里的 open(..., "w") 不是追加打开，而 BLE_AI_LOG 那边是追加，
    #   两个写入者各自维护偏移量，会互相覆盖出半截行（踩过，日志看着像跑了两遍）。
    #   BLE_AI_LOG 已经把 log() 的输出全收了，这里只留原始 traceback 有用。
    serve_stdout = open(os.path.join(HERE, "_e2e_serve_stdout.log"), "w", encoding="utf-8")
    proc = subprocess.Popen([PY, "ble_ai_pc.py", "serve"],
                            cwd=HERE, env=env,
                            stdout=serve_stdout, stderr=subprocess.STDOUT)
    p("[1] serve 已启动（pid=%s），日志：%s" % (proc.pid, os.path.basename(LOG_SERVE)))
    time.sleep(2)

    try:
        # ── 开串口 + 复位 ─────────────────────────────────────────
        p("\n[2] 串口 %s + RTS 脉冲复位…" % PORT_SER)
        ser.open()
        ser.dtr = False
        ser.rts = False
        time.sleep(0.2)

        ser.write(b"\r")
        read_for(1.0)
        ser.rts = True
        time.sleep(0.25)
        ser.rts = False
        ser.reset_input_buffer()
        boot, hit = read_for(25.0, stop_on=(b"SFBL", b"msh />"))
        p("    开机横幅 %d 字节，命中=%s" % (len(boot), hit))
        if not hit:
            p("    [!] 没抓到开机横幅 —— 板子可能没接、或串口号不是 %s" % PORT_SER)

        # ── 等蓝牙起广播 ──────────────────────────────────────────
        p("\n[3] 等蓝牙广播…")
        adv = False
        t0 = time.time()
        while time.time() - t0 < 100:
            read_for(3.0)
            st = send("ble", 1.0)
            if "state=advertising" in st or "adv_on=1" in st:
                adv = True
                break
            if "state=connected" in st:
                adv = True
                p("      （已经被 serve 连上了）")
                break
        p("    广播就绪 = %s（用了 %.0f 秒）" % (adv, time.time() - t0))

        # ── keepawake：无参数 toggle，必须看回显 ──────────────────
        p("\n[4] keepawake（注意：这是无参数 toggle，只能靠读回显判断）")
        t = send("keepawake", 1.2)
        if "enabled (10s)" in t:
            t = send("keepawake", 1.2)
            p("    -> " + ("已保持唤醒" if "DISABLED" in t else "还是没开成 ??"))
        else:
            p("    -> 本来就是保持唤醒状态")

        # ── 等 serve 连上，并且**真的能收数据** ──────────────────
        p("\n[5] 等电脑端 serve 连上来，并确认链路双向可用…")
        up = False
        t0 = time.time()
        while time.time() - t0 < 150:
            t = send("ai_info", 1.2)
            m_up = "up=1" in t
            mm = re.search(r"rx_pkts=(\d+)", t)
            rx = int(mm.group(1)) if mm else 0
            # ★ 只判 up=1 是不够的（第一版就是这么错的，而且**第一次跑还碰巧成功了**）：
            #   连上之后手表侧还要过几十秒才处理完 CCCD 订阅，
            #   在那之前手表发出去的数据会被静默丢弃 ——
            #   手表侧显示 TX 成功、tx_err=0，电脑侧却一个字节都收不到。
            #   rx_pkts≥1 说明 serve 发的就绪信标到了，这才叫真就绪。
            if m_up and rx >= 1:
                up = True
                p("    -> 已连上且已收到电脑端数据（rx_pkts=%d，%.0f 秒）"
                  % (rx, time.time() - t0))
                break
            p("      ...up=%d rx_pkts=%d（%.0f 秒）"
              % (int(m_up), rx, time.time() - t0))
        p("    链路就绪 = %s" % up)

        if not up:
            p("\n[停止] 一直没等到「已连上且收到电脑端数据」。看 _e2e_serve.log。")
            return 1

        # ── 提问 ─────────────────────────────────────────────────
        p("\n[6] 提问：%s" % QUESTION)
        send("ai_send " + QUESTION, 3.0)

        # 判据用累积器：答案很快，可能在上一步的等待窗口里就到了
        p("\n[7] 等答案回到手表（最多 60 秒）…")
        answered = any("ble_ai: RX#" in s for s in SEEN)
        if answered:
            p("      （答案在上一步的窗口里就到了）")
        t0 = time.time()
        while not answered and time.time() - t0 < 60:
            buf = read_for(3.0)[0].decode("utf-8", errors="replace")
            SEEN.append(buf)
            for ln in buf.splitlines():
                if ln.strip():
                    p("      " + ln.strip())
            if "ble_ai: RX#" in buf:
                answered = True
        p("    收到答案 = %s" % answered)

        send("ai_recv", 2.0)

        # ── 核对 ─────────────────────────────────────────────────
        p("\n" + "=" * 70)
        p("  核对")
        p("=" * 70)

        ok_mock = len(Mock.hits) >= 1
        p("  %s 假模型收到了请求（%d 次）" % ("[OK]  " if ok_mock else "[FAIL]", len(Mock.hits)))
        q_ok = any(h["q"] == QUESTION for h in Mock.hits)
        p("  %s 请求里的问题就是「%s」" % ("[OK]  " if q_ok else "[FAIL]", QUESTION))
        auth_ok = any(h["auth"].startswith("Bearer sk-mock") for h in Mock.hits)
        p("  %s Authorization 头正确" % ("[OK]  " if auth_ok else "[FAIL]"))
        p("  %s 手表收到了回传数据" % ("[OK]  " if answered else "[FAIL]"))

        # 串口原文里应该能看到答案正文
        blob = "".join(SEEN)
        ans_ok = "HTTP假模型" in blob and QUESTION[:6] in blob
        p("  %s 手表侧日志里能看到假模型那句答案原文" % ("[OK]  " if ans_ok else "[FAIL]"))

        allok = ok_mock and q_ok and auth_ok and answered and ans_ok
        p("")
        p("=" * 70)
        p("  端到端%s" % ("成功：整条链路在真机 + 真 HTTP 上跑通了。" if allok
                        else "未通过，看上面的 [FAIL]。"))
        p("=" * 70)
        return 0 if allok else 1

    finally:
        try:
            ser.close()
        except Exception:
            pass
        try:
            proc.terminate()
            proc.wait(timeout=5)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass
        serve_stdout.close()
        srv.shutdown()
        p("\n[清理] serve 已停，串口已关，临时配置留着供你查看：%s" % os.path.basename(CFG))


if __name__ == "__main__":
    rc = 1
    try:
        rc = main()
    except Exception:
        import traceback
        p("\n!!! 脚本自身出错 !!!\n" + traceback.format_exc())
        rc = 2
    finally:
        with open(REPORT, "w", encoding="utf-8") as f:
            f.write(OUT.getvalue())
    sys.exit(rc)
