# -*- coding: utf-8 -*-
r"""
precheck_src.py -- 单独预编译一个源文件，用工程自己的编译命令。

为什么要这个：
  scons 只有在整轮成功结束时才写 .sconsign.dblite。如果你新写的文件有编译错，
  整轮就会中断 —— 于是签名库不更新 —— 下一轮又要从头编 20 多分钟，
  再断在同一个地方。你会以为"编译就是很慢"，其实是在白烧。

所以新文件（新 app 的 main.c 之类）**在整轮跑到它之前**，先拿
compile_commands.json 里真实的那条命令单编一次，把语法/头文件问题提前暴露出来。

用法：
    1. 改下面 TEMPLATE_HINT（拿哪个已验证文件的命令当模板）和 SRCFILE（要检查谁）
    2. <python> precheck_src.py
    输出 "exit: 0" + "OK object N bytes" 就是通过；有错会原样打出 gcc 的报错。

为什么不能简单地 subprocess.run(命令字符串)：
  · 这条命令有 1.2 万字符，超过 Windows 命令行 8191 上限 -> 用响应文件 @args.rsp
  · 响应文件里 gcc 会把 `\` 当转义 -> 全转成正斜杠
  · 命令行里的 `-o / -MF / -MT / -MD` 要改掉，否则会覆盖 build 树里的真 .o
（这三条同样写进了技能库 huangshanpai-watch-lvgl-face 的「工程位置与编译」一节。）
"""
import json
import os
import re
import subprocess
import sys
import tempfile

PROJ = r"C:\Users\18058\Desktop\huangshanpai\watch\lvgl\watch\project"
BUILD = os.path.join(PROJ, "build_sf32lb52-lchspi-ulp_hcpu")
CC_JSON = os.path.join(BUILD, "compile_commands.json")
TOOLCHAIN = r"C:\Users\18058\.sifli\tools\arm-none-eabi-gcc\14.2.1\bin"

# 拿哪个已验证文件的命令当模板 / 要检查哪个新文件
TEMPLATE_HINT = "gui_apps\\calculator\\main.c"
# 也可以在命令行给：python precheck_src.py <要检查的 .c 的绝对路径>
DEFAULT_SRCFILE = r"C:\Users\18058\Desktop\huangshanpai\watch\lvgl\watch\src\gui_apps\torch\main.c"
SRCFILE = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_SRCFILE


def main():
    with open(CC_JSON, encoding="utf-8", errors="replace") as f:
        db = json.load(f)

    tmpl = None
    for e in db:
        if TEMPLATE_HINT in e.get("file", ""):
            tmpl = e
            break
    if not tmpl:
        print("[ERROR] template entry not found for " + TEMPLATE_HINT)
        return 2

    cmd = tmpl.get("command") or " ".join(tmpl.get("arguments", []))
    directory = tmpl.get("directory")

    # 出到临时目录，绝不碰 build 树
    outdir = tempfile.mkdtemp(prefix="precheck_")
    out = os.path.join(outdir, os.path.basename(SRCFILE)[:-2] + ".o")

    # 换掉 -o 目标、去掉依赖文件（-MF/-MT/-MD），换掉源文件
    #
    # ⚠ 替换串里是 Windows 路径（含反斜杠），不能直接把字符串当 repl ——
    #   re.sub 会把 \U \t 之类当转义处理，报 "bad escape \U"。
    #   一律用 lambda 返回，绕过转义解析。
    cmd = re.sub(r'-o\s+\S+', lambda m: '-o "' + out + '"', cmd, count=1)
    cmd = re.sub(r'\s-M[FDT]\s+\S+', ' ', cmd)
    cmd = re.sub(r'\s-MD\b', ' ', cmd)
    cmd = re.sub(r'(\s)\S+main\.c(\s*$)', lambda m: m.group(1) + '"' + SRCFILE + '"' + m.group(2), cmd)

    env = dict(os.environ)
    env["PATH"] = TOOLCHAIN + os.pathsep + env.get("PATH", "")

    print("cwd :", directory)
    print("src :", SRCFILE)
    print("out :", out)
    print("cmd len:", len(cmd))
    print("-" * 60)

    # ⚠ 这条命令有 1.2 万字符，直接 CreateProcess 会报「命令行太长」
    #   （Windows 上限约 8191）。SDK 自己也是靠「响应文件」绕过的 ——
    #   这里照做：把参数写进 @file，命令行只剩 gcc + @file。
    parts = cmd.split(" ", 1)
    prog, rest = parts[0], parts[1] if len(parts) > 1 else ""
    rsp = os.path.join(outdir, "args.rsp")
    # ⚠ 响应文件里 gcc 把 `\` 当转义字符解析，Windows 反斜杠路径会被吃掉：
    #   实测 C:\Users\...\main.c 变成了 C:Users...main.c（报 No such file）。
    #   全转成正斜杠 —— Windows 上的 gcc 两样都认。
    with open(rsp, "w", encoding="ascii", errors="replace", newline="\n") as f:
        f.write(rest.replace("\\", "/"))

    r = subprocess.run([os.path.join(TOOLCHAIN, "arm-none-eabi-gcc.exe"), "@" + rsp],
                       cwd=directory, env=env,
                       capture_output=True, text=True, errors="replace")
    print("exit:", r.returncode)
    if r.stdout.strip():
        print("--- stdout ---\n" + r.stdout)
    if r.stderr.strip():
        print("--- stderr ---\n" + r.stderr)
    if r.returncode == 0 and os.path.exists(out):
        print("OK  object %d bytes" % os.path.getsize(out))
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
