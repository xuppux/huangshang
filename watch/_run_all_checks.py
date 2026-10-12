# -*- coding: utf-8 -*-
r"""
_run_all_checks.py —— 一把梭的全量自检

    python _run_all_checks.py

三件事：
    ① 所有 Python 文件的语法（py_compile）
    ② 蓝牙协议往返回归（_test_proto.py）
    ③ 真模型链路验证（_test_real_model_path.py，本地假服务，不联网）

输出同时写 _finalcheck.txt —— 因为这台机器上 PowerShell/Bash 的输出经常被吞，
写文件再读才是稳的。
"""

import io
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PY = sys.executable

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

# 需要检查语法的文件（不改动手表固件那一侧，只查电脑端脚本）
FILES = [
    "ble_ai_pc.py",
    "ai_brain.py",
    "_test_proto.py",
    "_test_real_model_path.py",
    "_e2e_mock.py",
    "_ser_wake.py",
    "_ser_diag.py",
    "_ser_ask2.py",
    "_run_all_checks.py",
]

OUT = io.StringIO()


def say(*a):
    s = " ".join(str(x) for x in a)
    print(s, flush=True)
    OUT.write(s + "\n")


def run(cmd, cwd=HERE, timeout=300):
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, timeout=timeout)
    out = (p.stdout or b"").decode("utf-8", "replace")
    err = (p.stderr or b"").decode("utf-8", "replace")
    return p.returncode, out, err


def main():
    bad = []

    say("=" * 70)
    say("  全量自检")
    say("=" * 70)

    # ── ① 语法 ───────────────────────────────────────────────────
    # 用内置 compile() 而不是 py_compile.compile()：后者在 Windows 上
    # 不接受 os.devnull（"nul" 不是普通文件），而且会往 __pycache__ 写东西。
    say("\n### ① 语法检查 ###")
    for fn in FILES:
        p = os.path.join(HERE, fn)
        if not os.path.exists(p):
            say("  [跳过] %s（不在）" % fn)
            continue
        try:
            with open(p, encoding="utf-8") as f:
                compile(f.read(), p, "exec")
            say("  [OK]   %s" % fn)
        except SyntaxError as e:
            say("  [FAIL] %s → 第 %s 行：%s" % (fn, e.lineno, e.msg))
            bad.append("语法：" + fn)
        except Exception as e:
            say("  [FAIL] %s → %r" % (fn, e))
            bad.append("语法：" + fn)

    # ── ② 协议回归 ───────────────────────────────────────────────
    say("\n### ② 蓝牙协议往返回归（_test_proto.py）###")
    rc, out, err = run([PY, "_test_proto.py"])
    lines = [ln for ln in out.splitlines() if ln.strip()]
    picked = [ln for ln in lines if any(k in ln for k in ("组合", "通过", "失败", "PASS", "FAIL"))]
    for ln in picked:
        say("  " + ln)
    say("  （上面是结论行；最后几个分包样例：）")
    for ln in lines[-4:]:
        say("  " + ln)
    if rc != 0:
        say("  [FAIL] 返回码 %s" % rc)
        bad.append("协议回归")
        if err.strip():
            say("  stderr: " + err.strip()[:400])

    # ── ③ 真模型链路 ─────────────────────────────────────────────
    say("\n### ③ 真模型链路验证（_test_real_model_path.py）###")
    rc, out, err = run([PY, "_test_real_model_path.py"])
    if rc != 0:
        say("  [FAIL] 返回码 %s" % rc)
        bad.append("真模型链路")
        for ln in out.splitlines():
            if "FAIL" in ln or "失败" in ln:
                say("  " + ln)
        if err.strip():
            say("  stderr: " + err.strip()[:400])
    else:
        n_ok = out.count("[OK]")
        say("  [OK]   全部通过（%d 项断言）" % n_ok)

    # ── ④ 当前配置 ───────────────────────────────────────────────
    say("\n### ④ 当前大脑配置 ###")
    try:
        import ai_brain
        conf = ai_brain.load_config(os.path.join(HERE, "ai_brain_config.json"))
        for ln in ai_brain.describe(conf).splitlines():
            say(ln)
    except Exception as e:
        say("  [FAIL] 读不了配置：%r" % (e,))
        bad.append("读配置")

    say("\n" + "=" * 70)
    if bad:
        say("  有 %d 项没过：" % len(bad))
        for b in bad:
            say("    - " + b)
    else:
        say("  全部通过。")
    say("=" * 70)

    with open(os.path.join(HERE, "_finalcheck.txt"), "w", encoding="utf-8") as f:
        f.write(OUT.getvalue())
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
