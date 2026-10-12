# -*- coding: utf-8 -*-
r"""
ai_brain.py —— 「大脑」：把一句问题变成一句答案

这是阶段二的核心。手表负责问，这里负责答，答完再送回手表。

════════════════════════════════════════════════════════════════════════
  两种模式（在 ai_brain_config.json 里切换）
════════════════════════════════════════════════════════════════════════
  "offline"  本地假大脑。不联网、不花钱、不需要 key。规则式回答。
             用途：先把「手表问 → 电脑答 → 手表显示」整条链路验证通，
             免得调试通道问题时还要怀疑是不是网络/额度/模型的问题。

  "openai"   真大模型。走 **OpenAI 兼容接口** —— 国内几家都兼容这一套，
             换 base_url + model 就行，不用改代码。
             预置在配置文件里的有：deepseek / qwen / zhipu / moonshot /
             hunyuan / openai（见 ai_brain_config.json 的 _presets）。

════════════════════════════════════════════════════════════════════════
  为什么要限制回答长度
════════════════════════════════════════════════════════════════════════
系统提示词里明确要求「最多两三句话、不要 Markdown」。
这不是偷懒 —— 手表屏幕只有 390×450，而且是 LVGL 的 label，
Markdown 的 `**粗体**`、`- 列表` 在那边就是一堆原始符号。
BLE 通道单包能带 524 字节（MTU 527），也放不下长篇大论。

所以：**prompt 工程在这一步不只是"问得好"，还要"答得短"。**

════════════════════════════════════════════════════════════════════════
  命令行自测（不用手表，先确认 key 通不通）
════════════════════════════════════════════════════════════════════════
    python ai_brain.py                    看当前配置
    python ai_brain.py 1+1 等于几          问一句
    python ai_brain.py --offline 你好       强制用离线假大脑
"""

import json
import os
import socket
import sys
import urllib.error
import urllib.request

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

HERE = os.path.dirname(os.path.abspath(__file__))
# 设了环境变量 AI_BRAIN_CONFIG=<路径> 就用它当配置文件。
# 用途：想同时保留两套配置（比如一套真 key、一套指向本地假服务做测试），
# 或者不想在联调时动自己那份真配置。
CONF_PATH = os.environ.get("AI_BRAIN_CONFIG") or os.path.join(HERE, "ai_brain_config.json")

# 预置。配置文件里 provider 写这些名字，就会自动填上 base_url / model。
PRESETS = {
    "deepseek": {"base_url": "https://api.deepseek.com/v1",
                 "model": "deepseek-chat"},
    "qwen":     {"base_url": "https://dashscope.aliyuncs.com/compatible-mode/v1",
                 "model": "qwen-plus"},
    "zhipu":    {"base_url": "https://open.bigmodel.cn/api/paas/v4",
                 "model": "glm-4-flash"},
    "moonshot": {"base_url": "https://api.moonshot.cn/v1",
                 "model": "moonshot-v1-8k"},
    "hunyuan":  {"base_url": "https://api.hunyuan.cloud.tencent.com/v1",
                 "model": "hunyuan-lite"},
    "openai":   {"base_url": "https://api.openai.com/v1",
                 "model": "gpt-4o-mini"},
}

# ★ 这段提示词是产品的一部分，不是随手写的
DEFAULT_SYSTEM = (
    "你是一个运行在智能手表上的助手。"
    "回答必须简短：最多两三句话，整段回答不要超过 200 个字。"
    "不要用 Markdown（不要星号、井号、短横线列表、代码块），"
    "因为手表屏幕又小又窄，那些符号会原样显示出来。"
    "不要客套，不要重复我的问题，直接给答案。"
)

# 手表侧接收缓冲是 1024 字节（ble_ai.c 里的 BLE_AI_RX_MAX）。
# 模型如果不听话写了长篇，超过这个数在手表端会被硬截断 —— 而且可能
# 正好切在一个汉字的中间，末尾变成乱码。所以在电脑端先按字节截好。
MAX_REPLY_BYTES = 900

DEFAULT_CONF = {
    "provider": "offline",
    "api_key": "",
    "base_url": "",
    "model": "",
    "system_prompt": DEFAULT_SYSTEM,
    "timeout": 30,
    "max_tokens": 200,
    # "" = 跟随系统代理；"none" = 强制不走代理；其它值 = 用这个代理地址。
    # 为什么要有这个：国内 API（deepseek/qwen/zhipu）走全局代理绕一圈经常更慢
    # 甚至失败，而失败时表现为「对方 502」，极易误判成对方服务器挂了。
    "proxy": "",
}


def clip_reply(text, limit=MAX_REPLY_BYTES):
    """按 UTF-8 字节数截断，且不切断任何字符。返回结果**一定**不超过 limit 字节。

    为什么不用 text[:N]：Python 的 str 按字符数算，一个汉字占 3 字节。

    为什么不用「遇到续字节就往前退一格」这种写法（我第一版就是这么写的）：
    "你" 的 UTF-8 是 E4 BD A0，最后一个字节 A0 本来就是续字节形态。
    按那个写法，切在完整字符之后也会被当成"切歪了"继续往前退，
    结果白白少一个字。正确做法是**直接试着解码，解不开才退一格** ——
    UTF-8 一个字符最多 4 字节，所以最多退 3 次就一定能解开。

    省略号（3 字节）要从额度里先扣掉。第一版忘了扣，结果
    上限 900 却回了 903 字节 —— 上限是硬约束，守的是手表那 1024 字节的缓冲，
    差 3 个字节也是差。
    """
    b = text.encode("utf-8")
    if len(b) <= limit:
        return text

    mark = "…"
    room = limit - len(mark.encode("utf-8"))

    if room <= 0:
        # 上限小到连省略号都放不下（实际用不到，900 的上限不会走到这）。
        # 那就别硬塞省略号 —— 返回值超上限比少一个省略号糟糕得多。
        cut = b[:limit]
        while cut:
            try:
                return cut.decode("utf-8")
            except UnicodeDecodeError:
                cut = cut[:-1]
        return ""

    cut = b[:room]
    while cut:
        try:
            return cut.decode("utf-8") + mark
        except UnicodeDecodeError:
            cut = cut[:-1]
    return mark


def load_config(path=None):
    """读配置。文件不存在或坏了都不抛异常 —— 退回离线模式，
    免得"配置文件写错一个逗号"就整个用不了。

    path 默认在**调用时**取 CONF_PATH，不写成 `path=CONF_PATH`：
    写成默认参数会在函数定义时就把路径绑死，测试里想换成临时配置就换不了。
    """
    if path is None:
        path = CONF_PATH
    conf = dict(DEFAULT_CONF)
    conf["_warnings"] = []

    if not os.path.exists(path):
        conf["_warnings"].append("找不到 %s，先按离线模式跑" % os.path.basename(path))
        return conf

    try:
        with open(path, encoding="utf-8") as f:
            raw = json.load(f)
    except Exception as e:
        conf["_warnings"].append("配置文件读不了（%s），先按离线模式跑" % e)
        return conf

    if not isinstance(raw, dict):
        conf["_warnings"].append("配置文件不是一个 JSON 对象，先按离线模式跑")
        return conf

    # 以 _ 开头的键是给人看的注释/预置表，不带进配置
    for k, v in raw.items():
        if k.startswith("_"):
            continue
        if v != "":
            conf[k] = v

    # 值两头的空白一律去掉。
    # ★ 这里曾经只做了 .lower() 没做 .strip()，结果 provider 写成
    #   "DeepSeek "（多个空格）就匹配不上预置表，base_url 留空，
    #   最后报「配置里没有 base_url」—— 完全看不出是空格的问题。
    for k in ("provider", "api_key", "base_url", "model", "proxy", "system_prompt"):
        if isinstance(conf.get(k), str):
            conf[k] = conf[k].strip()

    # provider 如果是预置名，补齐 base_url / model
    p = str(conf.get("provider", "offline")).lower()
    conf["provider"] = p
    if p in PRESETS:
        conf.setdefault("_preset", PRESETS[p])
        if not conf.get("base_url"):
            conf["base_url"] = PRESETS[p]["base_url"]
        if not conf.get("model"):
            conf["model"] = PRESETS[p]["model"]

    return conf


def describe(conf):
    lines = []
    prov = conf.get("provider", "offline")
    lines.append("  模式      : %s" % prov)
    if prov == "offline":
        lines.append("  （本地假大脑：不联网、不花钱。要接真模型就把 provider 改成 openai）")
    else:
        lines.append("  接口      : %s" % (conf.get("base_url") or "(空)"))
        lines.append("  模型      : %s" % (conf.get("model") or "(空)"))
        k = conf.get("api_key") or ""
        lines.append("  API key   : %s" % (("已填（%s...%s，共 %d 字符）"
                                        % (k[:6], k[-4:], len(k))) if k else "**没填**"))
        px = str(conf.get("proxy") or "").strip()
        lines.append("  代理      : %s" % (
            "不走代理" if px.lower() in ("none", "off", "disable", "0", "false")
            else (px or "跟随系统默认")))
    lines.append("  超时      : %s 秒" % conf.get("timeout"))
    lines.append("  回答上限  : %d 字节（手表侧缓冲 1024，留点余量）" % MAX_REPLY_BYTES)
    for w in conf.get("_warnings", []):
        lines.append("  [!] %s" % w)
    return "\n".join(lines)


# ══════════════════════════════════════════════════════════════════════
#  离线假大脑
#
#  它不是"随便回一句"——要能真的回答几类问题，这样链路跑通的时候
#  你能确认"答案确实是根据我的问题生成的"，而不是一个固定字符串。
#  固定字符串会掩盖"问题根本没传过来"这种 bug。
# ══════════════════════════════════════════════════════════════════════

def _offline_answer(q):
    import ast
    import datetime
    import operator
    import re

    s = q.strip()
    low = s.lower()

    # 打招呼
    if any(w in low for w in ("你好", "您好", "hi", "hello", "在吗")) and len(s) <= 12:
        return "你好，我是跑在电脑上的假大脑。链路是通的，接上真模型我就能好好回答了。"

    # 时间 / 日期
    if any(w in s for w in ("几点", "时间", "日期", "几号", "星期")):
        now = datetime.datetime.now()
        wd = "一二三四五六日"[now.weekday()]
        return "现在 %s，%d 月 %d 日，星期%s。" % (
            now.strftime("%H:%M"), now.month, now.day, wd)

    # 算术：只放行数字和四则运算（用 ast 白名单，绝不 eval 原始串）
    expr = re.sub(r"[^\d+\-*/().% ]", "", s.replace("等于几", "").replace("等于多少", ""))
    if re.fullmatch(r"[\d+\-*/().% ]+", expr or "") and re.search(r"\d", expr or ""):
        ops = {ast.Add: operator.add, ast.Sub: operator.sub, ast.Mult: operator.mul,
               ast.Div: operator.truediv, ast.Mod: operator.mod,
               ast.USub: operator.neg, ast.UAdd: operator.pos}

        def ev(n):
            if isinstance(n, ast.Constant) and isinstance(n.value, (int, float)):
                return n.value
            if isinstance(n, ast.BinOp) and type(n.op) in ops:
                return ops[type(n.op)](ev(n.left), ev(n.right))
            if isinstance(n, ast.UnaryOp) and type(n.op) in ops:
                return ops[type(n.op)](ev(n.operand))
            raise ValueError("不支持")

        try:
            v = ev(ast.parse(expr, mode="eval").body)
            if isinstance(v, float) and v == int(v):
                v = int(v)
            return "%s = %s" % (expr.strip(), v)
        except Exception:
            pass

    # 自我介绍
    if any(w in s for w in ("你是谁", "你叫什么", "介绍一下你")):
        return ("我是 ai_brain.py 里的离线假大脑，跑在你的电脑上。"
                "作用是在没接大模型之前，先把手表到电脑这条链路验证通。")

    # 兜底
    return "（离线假大脑）我收到了 %d 个字：「%s」。现在还没接真模型，接上之后我就能回答它了。" % (
        len(s), s[:30])


# ══════════════════════════════════════════════════════════════════════
#  真大模型：OpenAI 兼容接口
# ══════════════════════════════════════════════════════════════════════

def _openai_answer(conf, q):
    key = (conf.get("api_key") or "").strip()
    if not key:
        raise RuntimeError(
            "没有填 API key。打开 %s，把 \"api_key\" 填上（或先把 provider 改成 offline）。"
            % os.path.basename(CONF_PATH))

    base = (conf.get("base_url") or "").rstrip("/")
    if not base:
        raise RuntimeError("配置里没有 base_url。")

    url = base + "/chat/completions"
    body = {
        "model": conf.get("model"),
        "messages": [
            {"role": "system", "content": conf.get("system_prompt") or DEFAULT_SYSTEM},
            {"role": "user", "content": q},
        ],
        "max_tokens": int(conf.get("max_tokens") or 200),
        "stream": False,
    }

    req = urllib.request.Request(
        url,
        data=json.dumps(body).encode("utf-8"),
        headers={
            "Content-Type": "application/json",
            # 各家都用 Bearer，包括国内兼容模式
            "Authorization": "Bearer " + key,
        },
        method="POST",
    )

    timeout = float(conf.get("timeout") or 30)

    # ── 代理 ──────────────────────────────────────────────────────
    # urllib 默认会吃系统/环境变量里的代理。在开着代理的机器上，
    # 「连不上」不会报 URLError，而是被本机代理包装成一个 HTTP 502/504，
    # 看起来像对方服务器挂了 —— 这个坑实测踩到过。
    # 所以允许配置里显式关掉。
    proxy = str(conf.get("proxy") or "").strip()
    if proxy.lower() in ("none", "off", "disable", "0", "false"):
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    elif proxy:
        opener = urllib.request.build_opener(
            urllib.request.ProxyHandler({"http": proxy, "https": proxy}))
    else:
        opener = urllib.request.build_opener()

    try:
        with opener.open(req, timeout=timeout) as r:
            data = json.loads(r.read().decode("utf-8", errors="replace"))
    except urllib.error.HTTPError as e:
        detail = ""
        try:
            detail = e.read().decode("utf-8", errors="replace")[:400]
        except Exception:
            pass
        # 把最常见的几种挑出来说人话
        hint = ""
        if e.code == 401:
            hint = "（key 不对或没填对）"
        elif e.code == 402:
            hint = "（余额不足）"
        elif e.code == 403:
            hint = "（key 没权限，或这个模型你没开通）"
        elif e.code == 404:
            hint = "（模型名或 base_url 不对）"
        elif e.code == 429:
            hint = "（请求太频繁或超额）"
        elif e.code in (500, 502, 503, 504):
            hint = ("（对方服务器或你本机的代理/网关出错。"
                    "如果开着 VPN/代理，到 ai_brain_config.json 里加一行 "
                    "\"proxy\": \"none\" 再试 —— 国内模型的接口走代理绕一圈常出错）")
        raise RuntimeError("接口返回 HTTP %d %s %s %s" % (e.code, e.reason, hint, detail))
    except urllib.error.URLError as e:
        # ★ 超时经常被 urllib 包成 URLError 扔出来（"timed out"），
        #   所以要在这一支里先认出来，别把超时说成"连不上"。
        r = str(getattr(e, "reason", e))
        if isinstance(getattr(e, "reason", None), (socket.timeout, TimeoutError)) \
                or "timed out" in r.lower() or "timeout" in r.lower():
            raise RuntimeError(
                "接口 %.0f 秒没响应（超时）。模型可能太忙，或网络太慢。" % timeout)
        raise RuntimeError("连不上接口（%s）。检查网络，或 base_url 写错了。" % e.reason)
    except (socket.timeout, TimeoutError):
        raise RuntimeError("接口 %.0f 秒没响应（超时）。模型可能太忙，或网络太慢。" % timeout)
    except json.JSONDecodeError as e:
        raise RuntimeError(
            "接口返回的不是 JSON（多半是你本机的代理/网关插了一页错误页）。%s" % e)
    except Exception as e:
        raise RuntimeError("请求失败：%r" % (e,))

    try:
        return data["choices"][0]["message"]["content"].strip()
    except Exception:
        raise RuntimeError("接口返回的结构看不懂：%s" % json.dumps(data)[:300])


# ══════════════════════════════════════════════════════════════════════

class Brain:
    def __init__(self, conf=None):
        self.conf = conf or load_config()
        self.last_error = None
        self.count = 0

    @property
    def provider(self):
        return self.conf.get("provider", "offline")

    def ask(self, question):
        """永远返回一个字符串 —— 出错也返回一句人话，不要抛出去。
        理由：这个函数的调用方是「收到手表消息就回一句」，
        如果我们抛异常，手表那边就是一片沉默，比收到一句报错更难查。"""
        self.count += 1
        q = (question or "").strip()
        if not q:
            return "（空问题）"

        try:
            if self.provider == "offline":
                return _offline_answer(q)
            return _openai_answer(self.conf, q)
        except Exception as e:
            self.last_error = str(e)
            return "[大脑出错] " + str(e)[:180]

    def describe(self):
        return describe(self.conf)


def main():
    args = sys.argv[1:]
    force_offline = False
    if args and args[0] == "--offline":
        force_offline = True
        args = args[1:]

    conf = load_config()
    if force_offline:
        conf["provider"] = "offline"

    print("ai_brain.py —— 大脑自测")
    print(describe(conf))
    print("")

    if not args:
        print("用法：python ai_brain.py <问题>")
        print("      python ai_brain.py --offline <问题>   强制离线假大脑")
        return 0

    q = " ".join(args)
    print("[问] " + q)
    b = Brain(conf)
    print("[答] " + b.ask(q))
    return 0


if __name__ == "__main__":
    sys.exit(main())
