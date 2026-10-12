# -*- coding: utf-8 -*-
r"""
_test_real_model_path.py —— 把「接真模型」这条路上**除 key 以外**的变量全部验穿

为什么要有这个文件：
    用户手上还没有 key。而 _openai_answer() 这段 HTTP 代码从来没发过真请求。
    如果等他填了 key 才发现 URL 拼错 / 响应字段取错 / 超时没生效，
    那就会把「代码问题」误判成「key 问题」，白折腾一轮。

做法：
    在 127.0.0.1 起一个假的 OpenAI 兼容服务，把 base_url 指过去，
    然后逐项验证请求构造、响应解析、四种错误路径、超长回答的分包。

跑法：
    python _test_real_model_path.py

输出：
    控制台 + 同目录 _test_real_model_result.txt
"""

import io
import json
import os
import socket
import struct
import sys
import threading
import traceback
from http.server import BaseHTTPRequestHandler, HTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

import ai_brain                                    # noqa: E402
from ble_ai_pc import build_packets, Assembler     # noqa: E402

OUT = io.StringIO()
_fails = []


def say(*a):
    s = " ".join(str(x) for x in a)
    print(s, flush=True)
    OUT.write(s + "\n")


def check(name, cond, extra=""):
    if cond:
        say("  [OK]   %s" % name)
    else:
        say("  [FAIL] %s  %s" % (name, extra))
        _fails.append(name)
    return bool(cond)


# ══════════════════════════════════════════════════════════════════════
#  假的 OpenAI 兼容服务
# ══════════════════════════════════════════════════════════════════════

class Mock(BaseHTTPRequestHandler):
    captured = {}
    mode = "ok"
    answer = "好的，这是模型的回答。"
    hits = 0

    def log_message(self, *a):
        pass                                   # 别往 stderr 喷日志

    def _send(self, code, obj):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        Mock.hits += 1
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b""
        Mock.captured = {
            "method": self.command,
            "path": self.path,
            "auth": self.headers.get("Authorization"),
            "ctype": self.headers.get("Content-Type"),
            "ua": self.headers.get("User-Agent"),
            "raw": raw.decode("utf-8", "replace"),
        }

        if Mock.mode == "401":
            return self._send(401, {"error": {"message": "Invalid API key provided"}})
        if Mock.mode == "429":
            return self._send(429, {"error": {"message": "Rate limit reached"}})
        if Mock.mode == "404":
            return self._send(404, {"error": {"message": "model not found"}})
        if Mock.mode == "502":
            return self._send(502, {"error": {"message": "Bad Gateway"}})
        if Mock.mode == "weird":
            return self._send(200, {"unexpected": "no choices here"})
        if Mock.mode == "notjson":
            body = b"<html>502 Bad Gateway</html>"
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if Mock.mode == "slow":
            import time
            time.sleep(6)

        return self._send(200, {
            "id": "chatcmpl-mock",
            "object": "chat.completion",
            "choices": [{
                "index": 0,
                "message": {"role": "assistant", "content": Mock.answer},
                "finish_reason": "stop",
            }],
            "usage": {"prompt_tokens": 30, "completion_tokens": 12, "total_tokens": 42},
        })


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def make_conf(port, **over):
    c = {
        "provider": "openai",
        "api_key": "sk-mock-1234567890abcdef",
        "base_url": "http://127.0.0.1:%d/v1" % port,
        "model": "mock-model-v1",
        "system_prompt": "你是一个测试用助手。",
        "timeout": 5,
        "max_tokens": 200,
    }
    c.update(over)
    return c


# ══════════════════════════════════════════════════════════════════════

def main():
    say("=" * 68)
    say("  真模型链路验证（本地假 OpenAI 服务，不联网、不花钱）")
    say("=" * 68)

    port = free_port()
    srv = HTTPServer(("127.0.0.1", port), Mock)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    say("\n假服务已起在 http://127.0.0.1:%d/v1\n" % port)

    # ── 1. 请求构造 ────────────────────────────────────────────────
    say("【1】请求是不是按 OpenAI 规范发出去的")
    Mock.mode, Mock.answer = "ok", "你好！我是模型。"
    ans = ai_brain.Brain(make_conf(port)).ask("你好")
    c = Mock.captured

    check("方法是 POST", c.get("method") == "POST", c.get("method"))
    check("路径是 /v1/chat/completions", c.get("path") == "/v1/chat/completions", c.get("path"))
    check("Authorization 是 Bearer + key",
          c.get("auth") == "Bearer sk-mock-1234567890abcdef", repr(c.get("auth")))
    check("Content-Type 是 application/json",
          (c.get("ctype") or "").startswith("application/json"), repr(c.get("ctype")))

    try:
        body = json.loads(c.get("raw") or "{}")
    except Exception as e:
        body = {}
        check("请求体是合法 JSON", False, repr(e))
    else:
        check("请求体是合法 JSON", True)

    check("body.model 传对了", body.get("model") == "mock-model-v1", body.get("model"))
    check("body.stream 是 False", body.get("stream") is False, body.get("stream"))
    check("body.max_tokens 是数字", isinstance(body.get("max_tokens"), int), body.get("max_tokens"))
    msgs = body.get("messages") or []
    check("messages 有 system + user 两条", len(msgs) == 2,
          json.dumps(msgs, ensure_ascii=False)[:150])
    if len(msgs) == 2:
        check("第 1 条是 system 且内容就是配置里那一段",
              msgs[0].get("role") == "system"
              and msgs[0].get("content") == "你是一个测试用助手。",
              repr(msgs[0].get("content"))[:80])
        check("第 2 条是 user 且就是那句问题",
              msgs[1].get("role") == "user" and msgs[1].get("content") == "你好")

    # system_prompt 留空时要退到内置默认值，且默认值必须带「短」的约束
    Mock.captured = {}
    ai_brain.Brain(make_conf(port, system_prompt="")).ask("你好")
    dflt = ""
    try:
        dflt = (json.loads(Mock.captured.get("raw") or "{}")
                .get("messages", [{}])[0].get("content", ""))
    except Exception:
        pass
    check("system_prompt 留空 → 自动用内置默认提示词", dflt == ai_brain.DEFAULT_SYSTEM,
          repr(dflt)[:80])
    check("内置提示词限了字数", "200" in dflt, dflt)
    check("内置提示词禁了 Markdown", "Markdown" in dflt and "星号" in dflt, dflt)

    # 真实配置文件里那段提示词也得带上这两条约束
    _real = ai_brain.load_config(os.path.join(HERE, "ai_brain_config.json"))
    _sp = _real.get("system_prompt") or ""
    check("ai_brain_config.json 里的提示词限了字数", "200" in _sp, _sp[:60])
    check("ai_brain_config.json 里的提示词禁了 Markdown", "Markdown" in _sp, _sp[:60])

    # ── 2. 响应解析 ────────────────────────────────────────────────
    say("\n【2】响应能不能正确取出来")
    check("取到了 choices[0].message.content", ans == "你好！我是模型。", repr(ans))
    check("没有混进 [大脑出错]", "[大脑出错]" not in ans, ans)

    Mock.answer = "  前后有空格  "
    ans2 = ai_brain.Brain(make_conf(port)).ask("x")
    check("前后空白被 strip 掉了", ans2 == "前后有空格", repr(ans2))

    # ── 3. 错误路径是不是说人话 ────────────────────────────────────
    say("\n【3】四种出错场景，是否给出能看懂的原因")

    cases = [
        ("401", "key 不对",     "key"),
        ("429", "请求太频繁",   "频繁"),
        ("404", "模型名不对",   "模型名"),
    ]
    for mode, label, kw in cases:
        Mock.mode = mode
        a = ai_brain.Brain(make_conf(port)).ask("测试")
        ok = ("[大脑出错]" in a) and ("HTTP" in a) and (kw in a)
        check("%s → 提示「%s」" % (mode, label), ok, a)

    Mock.mode = "weird"
    a = ai_brain.Brain(make_conf(port)).ask("测试")
    check("结构不对 → 提示看不懂结构", "结构看不懂" in a, a)

    Mock.mode = "notjson"
    a = ai_brain.Brain(make_conf(port)).ask("测试")
    check("返回不是 JSON → 不崩、且有提示", "[大脑出错]" in a and "不是 JSON" in a
          and len(a) < 300, a)

    Mock.mode = "502"
    a = ai_brain.Brain(make_conf(port)).ask("测试")
    check("502 → 提示可能是代理/网关，并给出关代理的办法",
          "[大脑出错]" in a and "502" in a and "代理" in a and "proxy" in a, a)

    Mock.mode = "ok"
    a = ai_brain.Brain(make_conf(1, proxy="none")).ask("测试")   # 端口 1 必然连不上
    check("连不上 → 提示检查网络/base_url", "连不上接口" in a, a)

    # ★ 反证：不显式关代理时，本机代理会把「连不上」包装成 HTTP 502。
    #   这条同时证明了 proxy="none" 是真的生效了（否则第一条也会拿到 502）。
    a2 = ai_brain.Brain(make_conf(1)).ask("测试")
    say("      不关代理时同一句的结果：%s" % a2[:78])
    check("跟随系统代理时，若被代理拦下也要提到代理",
          ("502" not in a2) or ("代理" in a2), a2)

    a = ai_brain.Brain(make_conf(port, api_key="")).ask("测试")
    check("没填 key → 明确说没填", "没有填 API key" in a, a)

    # ── 4. 超时 ───────────────────────────────────────────────────
    say("\n【4】超时是否生效（假服务故意睡 6 秒）")
    import time
    Mock.mode = "slow"
    t0 = time.time()
    a = ai_brain.Brain(make_conf(port, timeout=2)).ask("测试")
    dt = time.time() - t0
    check("2 秒超时在 2~5 秒内返回", 1.5 < dt < 5.0, "实际 %.1f 秒" % dt)
    check("超时也返回字符串、不抛异常", isinstance(a, str) and len(a) > 0, a[:80])
    check("超时说的是「没响应/超时」而不是「连不上」",
          ("超时" in a) or ("没响应" in a), a)
    Mock.mode = "ok"

    # ── 5. 真模型可能话多：超长回答能不能送到手表 ──────────────────
    say("\n【5】模型话多的时候（真实会发生的），回答能不能原样送到手表")
    # 故意让假模型吐一段长中文，模拟"提示词没压住"
    long_ans = "。" .join(["这是模型没管住嘴的长回答第%d句，中文和标点都要能原样到达手表" % i
                          for i in range(1, 13)])
    Mock.answer = long_ans
    got = ai_brain.Brain(make_conf(port)).ask("随便问点什么")
    say("      假模型吐出 %d 字符 / UTF-8 %d 字节"
        % (len(got), len(got.encode("utf-8"))))
    check("长回答完整取回、没被截断", got == long_ans,
          "拿到 %d 字符" % len(got))

    # 走一遍真实 MTU 下的组包 → 解包
    for mtu in (527, 247, 23):
        payload = got.encode("utf-8")
        pkts = build_packets(0x10, payload, mtu)
        asm = Assembler()
        out = None
        for p in pkts:
            r = asm.feed(p)
            if r:
                out = r
        ok = (out is not None and out[0] == 0x10 and out[1] == payload)
        check("MTU=%d 分包 %d 个 → 解包与原文逐字节一致" % (mtu, len(pkts)), ok,
              "" if out else "解包没拼完整")

    # ── 5b. 长回答的截断：不能切在汉字中间 ────────────────────────
    say("\n【5b】回答超过手表缓冲时的截断（按 UTF-8 字节，不许切在汉字中间）")
    L = ai_brain.MAX_REPLY_BYTES
    long_cn = "中山大学" * 400                       # 1600 汉字 = 4800 字节
    c = ai_brain.clip_reply(long_cn)
    check("截断后不超过上限（省略号也算在额度里）",
          len(c.encode("utf-8")) <= L, "%d 字节" % len(c.encode("utf-8")))
    check("截断后没有替换符（没切在字符中间）", "\ufffd" not in c, repr(c[-8:]))
    body = c[:-1] if c.endswith("…") else c
    check("截断结果仍是原文的完整前缀", long_cn.startswith(body),
          repr(body[-6:]))

    check("短内容原样不动", ai_brain.clip_reply("你好") == "你好")
    check("刚好等于上限时不加省略号（300 汉字 = 900 字节）",
          ai_brain.clip_reply("好" * 300) == "好" * 300)

    # ★ 这两条是回归点，写第一版时两条都错：
    #   ①「遇到续字节就退一格」会把完整汉字的尾字节当分界，多砍一个字；
    #   ②省略号没算进额度，上限 900 却回 903 字节。
    got9 = ai_brain.clip_reply("你" * 3 + "a", 9)
    check("限 9 字节：你*3+a → 你你…（6+3=9，不超）",
          got9 == "你你…" and len(got9.encode("utf-8")) == 9, repr(got9))
    got8 = ai_brain.clip_reply("你" * 3 + "a", 8)
    check("限 8 字节：退到完整字符 → 你…（3+3=6）",
          got8 == "你…" and len(got8.encode("utf-8")) <= 8, repr(got8))
    check("任何档位都不超上限、且无替换符",
          all(len(ai_brain.clip_reply("测试字符串" * 20, n).encode("utf-8")) <= n
              and "\ufffd" not in ai_brain.clip_reply("测试字符串" * 20, n)
              for n in range(1, 60)))
    check("上限极小（1~2 字节，放不下省略号）也不崩",
          all(isinstance(ai_brain.clip_reply("中文", n), str) for n in (1, 2, 3)))

    # ── 6. 配置合并：你自己填的 base_url 不能被预置表覆盖 ──────────
    say("\n【6】配置文件里 provider 与手填 base_url 的优先级")
    tmp = os.path.join(HERE, "_tmp_cfg_test.json")
    try:
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump({"provider": "deepseek",
                       "api_key": "sk-x",
                       "base_url": "http://127.0.0.1:%d/v1" % port,
                       "model": ""}, f)
        c2 = ai_brain.load_config(tmp)
        check("手填的 base_url 生效（没被 deepseek 预置覆盖）",
              c2["base_url"] == "http://127.0.0.1:%d/v1" % port, c2["base_url"])
        check("model 留空则自动补预置值", c2["model"] == "deepseek-chat", c2["model"])

        with open(tmp, "w", encoding="utf-8") as f:
            json.dump({"provider": "  DeepSeek  ", "api_key": "  sk-y  "}, f)
        c3 = ai_brain.load_config(tmp)
        check("provider 大小写/空格容错", c3["provider"] == "deepseek", repr(c3["provider"]))
        check("base_url 自动补预置",
              c3["base_url"] == "https://api.deepseek.com/v1", c3["base_url"])

        with open(tmp, "w", encoding="utf-8") as f:
            f.write('{"provider": "openai",,}')          # 故意的坏 JSON
        c4 = ai_brain.load_config(tmp)
        check("坏配置文件不崩、退回离线", c4["provider"] == "offline" and c4["_warnings"],
              str(c4["_warnings"]))
    finally:
        try:
            os.remove(tmp)
        except Exception:
            pass

    # ── 7. 真实配置文件现状 ───────────────────────────────────────
    say("\n【7】你现在的 ai_brain_config.json")
    real = ai_brain.load_config(os.path.join(HERE, "ai_brain_config.json"))
    say("      " + ai_brain.describe(real).replace("\n", "\n      "))
    check("没配 key 时不会崩，正常退回可用状态",
          isinstance(real.get("provider"), str), repr(real.get("provider")))

    # ── 8. 你实际会敲的那条命令，端到端跑一遍 ─────────────────────
    say("\n【8】端到端：真跑一遍 `ble_ai_pc.py ask`（后端指向假服务）")
    import argparse
    import asyncio

    import ble_ai_pc

    tmp2 = os.path.join(HERE, "_tmp_cfg_e2e.json")
    try:
        with open(tmp2, "w", encoding="utf-8") as f:
            json.dump({"provider": "openai",
                       "api_key": "sk-mock-1234567890abcdef",
                       "base_url": "http://127.0.0.1:%d/v1" % port,
                       "model": "mock-model-v1",
                       "timeout": 5}, f)

        Mock.mode = "ok"
        Mock.answer = "北京今天晴，18 到 26 度。"
        Mock.captured = {}

        _saved = ai_brain.CONF_PATH
        ai_brain.CONF_PATH = tmp2
        try:
            rc = asyncio.run(ble_ai_pc.mode_ask(argparse.Namespace(text="北京天气")))
        finally:
            ai_brain.CONF_PATH = _saved

        check("mode_ask 返回码 0（=整条路没出错）", rc == 0, "rc=%s" % rc)

        # ★ 注意：json.dumps 默认 ensure_ascii=True，会把中文转义成 \u5317\u4eac。
        #   这是合法 JSON，各家接口都能正确解码，不用改；
        #   但断言时得按**解码后**的内容比，按原始字节找中文是找不到的。
        _raw = Mock.captured.get("raw") or ""
        say("      实际发出的 body（前 120 字符）：%s" % _raw[:120])
        try:
            _q = json.loads(_raw)["messages"][1]["content"]
        except Exception:
            _q = None
        check("问题真的发出去了（解码后就是原文）", _q == "北京天气", repr(_q))

        # 模型话多时，mode_ask 会不会提示「会截断」
        Mock.answer = "这是一段很长的回答。" * 120        # 1200 汉字 = 3600 字节
        _saved = ai_brain.CONF_PATH
        ai_brain.CONF_PATH = tmp2
        try:
            rc2 = asyncio.run(ble_ai_pc.mode_ask(argparse.Namespace(text="讲长一点")))
        finally:
            ai_brain.CONF_PATH = _saved
        check("长回答时 mode_ask 仍然成功返回", rc2 == 0, "rc=%s" % rc2)
    finally:
        try:
            os.remove(tmp2)
        except Exception:
            pass
    Mock.mode = "ok"

    srv.shutdown()

    # ── 汇总 ──────────────────────────────────────────────────────
    say("\n" + "=" * 68)
    if _fails:
        say("  失败 %d 项：" % len(_fails))
        for f in _fails:
            say("    - " + f)
    else:
        say("  全部通过。真模型这条路上除「key 和网络」以外的变量都已排除。")
    say("=" * 68)

    txt = OUT.getvalue()
    with open(os.path.join(HERE, "_test_real_model_result.txt"), "w", encoding="utf-8") as f:
        f.write(txt)
    return 1 if _fails else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception:
        traceback.print_exc()
        with open(os.path.join(HERE, "_test_real_model_result.txt"), "w", encoding="utf-8") as f:
            f.write(OUT.getvalue() + "\n\n!!! 脚本自身崩溃 !!!\n" + traceback.format_exc())
        sys.exit(2)
