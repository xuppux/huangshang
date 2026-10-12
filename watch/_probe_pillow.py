# -*- coding: utf-8 -*-
"""探测 Pillow 版本 + 可用的粗体 ttf（图标上要写数字 2048，需要一个真字体）。"""
import glob
import os
import sys

out = []
try:
    import PIL
    from PIL import Image, ImageDraw, ImageFont
    out.append("Pillow=" + PIL.__version__)
except Exception as e:
    out.append("Pillow MISSING " + repr(e))
    open(r"C:\Users\18058\Desktop\huangshanpai\watch\_pillow.txt", "w",
         encoding="utf-8").write("\n".join(out))
    sys.exit(0)

# load_default(size=...) 需要 Pillow >= 10.1
try:
    f = ImageFont.load_default(size=40)
    out.append("load_default(size=40) OK -> %r" % (getattr(f, "path", None),))
except Exception as e:
    out.append("load_default(size=) FAIL " + repr(e))

cands = []
for pat in ("C:/Windows/Fonts/ariblk.ttf", "C:/Windows/Fonts/arialbd.ttf",
            "C:/Windows/Fonts/arial.ttf", "C:/Windows/Fonts/segoeuib.ttf",
            "C:/Windows/Fonts/msyhbd.ttc", "C:/Windows/Fonts/msyh.ttc",
            "C:/Windows/Fonts/verdana*.ttf", "C:/Windows/Fonts/tahoma*.ttf",
            "C:/Windows/Fonts/*.ttf"):
    cands += glob.glob(pat)
seen = []
for p in cands:
    if p not in seen:
        seen.append(p)
out.append("fonts found: %d" % len(seen))
for p in seen[:25]:
    out.append("  " + os.path.basename(p))

# 试一下能否真的加载并量出宽高（能加载才算可用）
ok = []
for p in seen:
    try:
        f = ImageFont.truetype(p, 40)
        bb = f.getbbox("2048")
        ok.append("%s  bbox(2048)=%s" % (os.path.basename(p), bb))
    except Exception:
        pass
out.append("loadable truetype: %d" % len(ok))
out += ["  " + s for s in ok[:20]]

open(r"C:\Users\18058\Desktop\huangshanpai\watch\_pillow.txt", "w",
     encoding="utf-8").write("\n".join(out))
