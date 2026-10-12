# -*- coding: utf-8 -*-
r"""
_test_proto.py —— 离线验证 ble_ai_pc.py 的组包逻辑
                  对照 ble_ai.c / bf0_sibles_serial_trans_service.c 的真实行为

不需要蓝牙、不需要板子。就是拿各种长度 x 各种 MTU 跑一遍：
    电脑组包 -> 用手表那一侧的状态机解回来 -> 必须和原文一模一样

手表那侧的状态机是照着 C 源码逐行搬的（ble_serial_tran_set_cbk）：
    frag != 0 且还没在拼  : 只有 frag==1 才开拼，其它直接忽略
    frag != 0 且正在拼    : 检查 cate 一致 / 不超长；frag==3 时校验长度相等
    frag == 0            : 完整包，通知 len-4，数据从 value+4 起
"""
import io
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

out = io.StringIO()


def p(*a):
    print(*a, file=out)


# ── 把 ble_ai_pc.py 里的两个函数搬过来（import 会连带 import bleak，没关系） ──
import importlib.util

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("ble_ai_pc", os.path.join(HERE, "ble_ai_pc.py"))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

build_packets = m.build_packets
Assembler = m.Assembler
CATE_AI = m.CATE_AI


def watch_decode(writes):
    """复刻 C 侧 ble_serial_tran_set_cbk。返回 (结果, 错误列表)。"""
    is_asm = False
    buf = None
    total = 0
    aid = 0
    errors = []
    result = None

    for w in writes:
        if len(w) < 2:
            errors.append("SHORT_PACKET")
            continue
        cate, frag = w[0], w[1]

        if frag != 0 and not is_asm:
            if frag != 1:
                errors.append("ORPHAN_FRAG_%d" % frag)
                continue
            is_asm = True
            total = struct.unpack("<H", w[2:4])[0]
            buf = bytearray(w[4:])
            aid = cate
            if len(buf) > total:
                errors.append("FIRST_OVER_LEN")
                is_asm, buf = False, None
            continue

        if frag != 0 and is_asm:
            if cate != aid or buf is None:
                errors.append("CATE_ID")
                is_asm, buf = False, None
                continue
            if len(buf) + (len(w) - 2) > total:
                errors.append("ASSEMBLE_OVER_LEN")
                is_asm, buf = False, None
                continue
            buf += w[2:]
            if frag == 3:
                if len(buf) != total:
                    errors.append("ASSEMBLE_ERROR(%d!=%d)" % (len(buf), total))
                else:
                    result = (aid, bytes(buf))
                is_asm, buf = False, None
            continue

        # frag == 0
        if is_asm:
            errors.append("FULL_PKT_WHILE_ASSEMBLING")
            is_asm, buf = False, None
        if len(w) < 4:
            errors.append("FULL_TOO_SHORT")
            continue
        result = (cate, bytes(w[4:]))

    return result, errors


# ══════════════════════════════════════════════════════════════════════

MTUS = [23, 40, 100, 185, 247, 512]
TEXTS = [
    "",
    "a",
    "hello",
    "0123456789ABCDEF",              # 16
    "0123456789ABCDEFG",             # 17
    "0123456789ABCDEFGHIJ",          # 20
    "0123456789ABCDEFGHIJKLMNOP",    # 26
    "你好，手表！",                    # 中文 18 字节
    "这是一条比较长的测试文本，用来逼出服务的分包逻辑：" + "0123456789" * 3,
    "x" * 500,
    "x" * 1000,
]

p("═" * 72)
p("  组包 / 拆包 往返测试")
p("═" * 72)
p("")

n_ok = n_bad = 0
for mtu in MTUS:
    for t in TEXTS:
        payload = t.encode("utf-8")

        # (A) 电脑组包 -> 自己的解包器
        pkts = build_packets(CATE_AI, payload, mtu)
        a = Assembler()
        got_a = None
        for pk in pkts:
            r = a.feed(pk)
            if r is not None:
                got_a = r

        # (B) 电脑组包 -> 手表那侧的状态机
        got_b, errors = watch_decode(pkts)

        # 每一包都不能超过一次 write 的上限
        over = [len(pk) for pk in pkts if len(pk) > mtu - 3]

        ok = True
        why = []
        if got_a is None or got_a[1] != payload:
            ok = False; why.append("自解包不一致")
        if errors:
            ok = False; why.append("手表侧报错:%s" % errors)
        if got_b is None or got_b[1] != payload:
            ok = False
            why.append("手表侧拼不回(得%s)" %
                       ("None" if got_b is None else "%d字节" % len(got_b[1])))
        if over:
            ok = False; why.append("有包里超过MTU: %s" % over)

        if ok:
            n_ok += 1
        else:
            n_bad += 1
            p("  [失败] mtu=%-4d len=%-5d 包数=%-3d  %s"
              % (mtu, len(payload), len(pkts), " / ".join(why)))
            p("         原始: %r" % (payload[:40],))
            for i, pk in enumerate(pkts[:6]):
                p("         包%d (%d字节): %s" % (i, len(pk), pk[:24].hex()))

p("")
p("  mtu 档位 : %s" % MTUS)
p("  长度档位 : %d 种（含 0 / 边界 16,17,20,26 / 中文 / 500 / 1000）" % len(TEXTS))
p("  组合     : %d" % (len(MTUS) * len(TEXTS)))
p("  通过     : %d" % n_ok)
p("  失败     : %d" % n_bad)
p("")

# ── 再单独看一眼分包边界，人工核对一遍 ────────────────────────────────
p("═" * 72)
p("  分包边界抽样（人工核对）")
p("═" * 72)
p("")
for mtu, ln in ((23, 16), (23, 17), (23, 40), (247, 243), (247, 244), (247, 300)):
    payload = bytes(range(256))[:ln] if ln <= 256 else b"z" * ln
    pkts = build_packets(CATE_AI, payload, mtu)
    p("  mtu=%d  payload=%d 次写上限=%d  ->  %d 个包"
      % (mtu, ln, mtu - 3, len(pkts)))
    for i, pk in enumerate(pkts):
        p("      包%d: 长%3d  cate=0x%02x flag=%d  数据%3d字节"
          % (i, len(pk), pk[0], pk[1], len(pk) - (4 if pk[1] in (0, 1) else 2)))
    p("")

sys.stdout.write(out.getvalue())
open(os.path.join(HERE, "_test_proto.txt"), "w", encoding="utf-8").write(out.getvalue())
