# -*- coding: utf-8 -*-
"""
画一张「计算器」应用图标，风格对齐原厂那套（实心圆底 + 白色字形）。

背景：主菜单的图标不是随便的 PNG —— 它们放在
      watch/lvgl/watch/src/resource/images/common/ezip/ 下面，
      构建脚本里的 Env.ImgResource() 会把每个 *.png 自动编译成同名符号
      （img_clock.png -> lv_img_dsc_t img_clock，ezip 压缩、LV_IMG_CF_RAW_ALPHA）。
      所以新增图标只要丢一张 PNG 进去，不用改任何 SConscript。

实测原厂图标的规律（见 _icon_montage.png）：
  - 画布 88x88 / 90x91，有效图形是直径约 88 的实心圆，居中
  - 圆底主色：橙 (224,128,0) / 灰 (128,128,128) / 白 (224,224,224) / 绿 (160,224,64)
  - 字形是纯白 (224,224,224) 或深色，压在圆底上
  - 全部带 alpha 透明边

所以我们做：橙色圆底 + 白色计算器字形，和最像的 img_alarm / img_stopwatch 同一族。

用法：python make_icon.py            # 生成并顺手拼一张对照图
"""
import os

from PIL import Image, ImageDraw

# ---- 画布：和原厂 img_clock.png 一样 90x91，超采样 4 倍做抗锯齿 ----
W, H = 90, 91
SS = 4

# ---- 配色（取自原厂图标实测值）----
ORANGE = (224, 128, 0, 255)      # 圆底：和 img_alarm / img_stopwatch 同一个橙
WHITE = (235, 235, 235, 255)     # 字形：原厂白是 (224,224,224)，略提亮一点点更干净
TRANSPARENT = (0, 0, 0, 0)

OUT_DIR = r"C:\Users\18058\Desktop\huangshanpai\watch\lvgl\watch\src\resource\images\common\ezip"
OUT_NAME = "img_calculator.png"


def px(v):
    """逻辑像素 -> 超采样画布像素"""
    return v * SS


def draw_icon():
    im = Image.new("RGBA", (W * SS, H * SS), TRANSPARENT)
    d = ImageDraw.Draw(im)

    # 圆底：直径 88，圆心 (45, 45.5) -> 外接框 1..89，和原厂完全对齐
    d.ellipse([px(1), px(1.5), px(89), px(89.5)], fill=ORANGE)

    # 计算器机身：居中，宽 44 高 56
    bw, bh = 44.0, 56.0
    bx = (W - bw) / 2.0            # 23
    by = (H - bh) / 2.0            # 17.5
    d.rounded_rectangle([px(bx), px(by), px(bx + bw), px(by + bh)],
                        radius=px(7), fill=WHITE)

    # 显示屏：机身顶部，用底色挖出来，看起来就是"屏幕"
    dx, dy, dw, dh = bx + 6, by + 6, bw - 12, 11
    d.rounded_rectangle([px(dx), px(dy), px(dx + dw), px(dy + dh)],
                        radius=px(2.5), fill=ORANGE)

    # 按键：3x3，同样用底色挖
    kw, gap = 8.0, 4.0
    kx0 = bx + 6                                    # 29
    ky0 = dy + dh + (bh - 6 - 6 - dh - (kw * 3 + gap * 2)) / 2.0
    for r in range(3):
        for c in range(3):
            kx = kx0 + c * (kw + gap)
            ky = ky0 + r * (kw + gap)
            d.rounded_rectangle([px(kx), px(ky), px(kx + kw), px(ky + kw)],
                                radius=px(2), fill=ORANGE)

    return im.resize((W, H), Image.LANCZOS)


def montage(icon, path):
    """把新图标插在原厂图标中间，方便人眼比对风格/大小/对齐。"""
    src = os.path.join(OUT_DIR, "img_clock.png")
    names = ["img_clock", "img_alarm", "img_stopwatch", "img_settings", "img_workout"]
    cell = 110
    tiles = []
    for n in names:
        p = os.path.join(OUT_DIR, n + ".png")
        if os.path.exists(p):
            tiles.append((n, Image.open(p).convert("RGBA")))
    tiles.insert(2, ("img_calculator (new)", icon))

    cols = 3
    rows = (len(tiles) + cols - 1) // cols
    canvas = Image.new("RGBA", (cell * cols, cell * rows), (28, 32, 40, 255))
    for i, (n, im) in enumerate(tiles):
        t = im.resize((cell - 10, cell - 10), Image.LANCZOS)
        canvas.alpha_composite(t, ((i % cols) * cell + 5, (i // cols) * cell + 5))
    canvas.convert("RGB").save(path)
    return [n for n, _ in tiles]


def main():
    icon = draw_icon()
    dst = os.path.join(OUT_DIR, OUT_NAME)
    icon.save(dst)

    # 自检：面积、不透明像素、透明边是否真的透明
    a = icon.getchannel("A")
    opaque = sum(1 for v in a.get_flattened_data() if v > 200)
    print("saved   :", dst)
    print("size    : %dx%d" % icon.size)
    print("alpha   : corner=%d (0 才对)  opacity>200 的像素=%d"
          % (a.getpixel((0, 0)), opaque))

    mp = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_icon_montage_new.png")
    order = montage(icon, mp)
    print("montage :", mp)
    print("order   :", " | ".join(order))


if __name__ == "__main__":
    main()
