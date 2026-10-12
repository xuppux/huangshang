# -*- coding: utf-8 -*-
"""编译后自检：读 build3 日志 + 确认产物。"""
import io
import os
import re

ROOT = r"C:\Users\18058\Desktop\huangshanpai\watch"
RAW = os.path.join(ROOT, "_build3.raw")
BUILD = os.path.join(ROOT, "lvgl", "watch", "project",
                     "build_sf32lb52-lchspi-ulp_hcpu")


def decode(raw):
    if raw[:2] in (b"\xff\xfe", b"\xfe\xff"):
        return raw.decode("utf-16", "replace")
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return raw.decode("latin-1", "replace")


def main():
    raw = open(RAW, "rb").read()
    txt = decode(raw)
    lines = txt.splitlines()

    print("raw bytes:", len(raw), " lines:", len(lines))

    errs = [l.strip() for l in lines if "error:" in l or "Error" in l and "0 Error" not in l]
    print("error-ish lines:", len(errs))
    for l in errs[:12]:
        print("   ERR", l[:190])

    for key in ("BUILD3_EXIT", "scons: ***", "scons: done", "Done building", "Finished"):
        hit = [l.strip() for l in lines if key in l]
        for h in hit[-3:]:
            print("  [%s] %s" % (key, h[:190]))

    cc = [l for l in lines if l.startswith("CC ")]
    print("CC lines:", len(cc))

    print("--- tail 10 ---")
    for l in lines[-10:]:
        print("   ", l[:190])

    print("--- artifacts ---")
    if os.path.isdir(BUILD):
        for name in ("main.bin", "main.elf", "main.map", "rtconfig.h"):
            p = os.path.join(BUILD, name)
            if os.path.exists(p):
                print("   %-12s %10d B   mtime=%d" % (name, os.path.getsize(p),
                                                      os.path.getmtime(p)))
            else:
                print("   %-12s MISSING" % name)
    else:
        print("   build dir not found:", BUILD)

    # 手电筒的目标文件与资源
    probes = [
        os.path.join(BUILD, "sifli_sdk", "lvgl", "watch", "src", "gui_apps", "torch", "main.o"),
        os.path.join(BUILD, "sifli_sdk", "lvgl", "watch", "src", "resource", "images",
                     "common", "ezip", "img_torch.o"),
    ]
    print("--- torch objects ---")
    for p in probes:
        if os.path.exists(p):
            print("   OK   %s  (%d B)" % (os.path.relpath(p, BUILD), os.path.getsize(p)))
        else:
            found = None
            for dirpath, _dirs, files in os.walk(BUILD):
                for f in files:
                    if f == os.path.basename(p):
                        found = os.path.join(dirpath, f)
                        break
                if found:
                    break
            print("   %s  %s" % ("OK  " if found else "MISS", found or os.path.basename(p)))


main()
