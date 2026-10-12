# -*- coding: utf-8 -*-
"""看一眼原厂图标长什么样：尺寸、透明区、不透明像素的颜色分布。
顺便拼一张深色底的大图，方便人眼对照风格。"""
import os
from collections import Counter
from PIL import Image

SRC = r"C:\Users\18058\Desktop\huangshanpai\watch\lvgl\watch\src\resource\images\common\ezip"
NAMES = ["img_clock", "img_calendar", "img_camera", "img_settings", "img_stopwatch",
         "img_stocks", "img_remote", "img_workout", "img_alarm", "img_photos",
         "img_compass", "img_world_clock", "img_activity", "img_passbook", "img_LiChuang"]

out = []
tiles = []
for n in NAMES:
    p = os.path.join(SRC, n + ".png")
    if not os.path.exists(p):
        out.append("%-18s MISSING" % n)
        continue
    im = Image.open(p).convert("RGBA")
    w, h = im.size
    a = im.getchannel("A")
    bbox = a.getbbox()
    px = list(im.getdata())
    opaque = [c for c in px if c[3] > 200]
    cnt = Counter((r // 32 * 32, g // 32 * 32, b // 32 * 32) for r, g, b, al in opaque)
    top = cnt.most_common(3)
    out.append("%-18s %3dx%-3d bbox=%-22s opaque=%6d/%d  colors=%s"
               % (n, w, h, str(bbox), len(opaque), len(px),
                  " ".join("(%3d,%3d,%3d)x%d" % (c[0], c[1], c[2], k) for c, k in top)))
    tiles.append((n, im))

# 拼图：深色底，好让白色线稿看得见
if tiles:
    cell = 110
    cols = 5
    rows = (len(tiles) + cols - 1) // cols
    canvas = Image.new("RGBA", (cell * cols, cell * rows), (28, 32, 40, 255))
    for i, (n, im) in enumerate(tiles):
        im2 = im.resize((cell - 10, cell - 10), Image.LANCZOS)
        canvas.alpha_composite(im2, ((i % cols) * cell + 5, (i // cols) * cell + 5))
    dst = r"C:\Users\18058\Desktop\huangshanpai\watch\watchface-tools\_icon_montage.png"
    canvas.convert("RGB").save(dst)
    out.append("montage -> " + dst)

dst_txt = r"C:\Users\18058\Desktop\huangshanpai\watch\watchface-tools\_icon_info.txt"
open(dst_txt, "w", encoding="utf-8").write("\n".join(out))
print("\n".join(out))
