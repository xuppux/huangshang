# -*- coding: utf-8 -*-
"""把 PowerShell `*>` 重定向出来的日志解成可读文本（UTF-16 / ANSI 双兼容）。"""
import re
import sys


def dec(src, dst):
    raw = open(src, "rb").read()
    if raw[:2] in (b"\xff\xfe", b"\xfe\xff"):
        txt = raw.decode("utf-16", "replace")
    else:
        try:
            txt = raw.decode("utf-8")
        except UnicodeDecodeError:
            txt = raw.decode("latin-1", "replace")
    txt = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", txt)      # ANSI 色码
    txt = re.sub(r"\n{3,}", "\n\n", txt)
    open(dst, "w", encoding="utf-8").write(txt)
    return len(txt)


if __name__ == "__main__":
    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else src + ".txt"
    print("decoded %d chars -> %s" % (dec(src, dst), dst))
