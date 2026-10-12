# -*- coding: utf-8 -*-
"""
画一张「2048」应用图标，风格对齐原厂那套（实心圆底 + 白色字形）。

和 make_torch_icon.py / make_icon.py（计算器）是同一套路子，差别只在字形：

    橙色圆底 + 白色「2048」

为什么不用现成图片：主菜单的图标不是随便的 PNG —— 放在
    watch/lvgl/watch/src/resource/images/common/ezip/
下面的每个 *.png 会被构建脚本的 Env.ImgResource() 自动编译成同名符号
（img_game2048.png -> lv_img_dsc_t img_game2048，ezip 压缩、LV_IMG_CF_RAW_ALPHA）。
所以新增图标只要丢一张 PNG 进去，不用改任何 SConscript、也不用改工程文件。

原厂图标的实测规律（见 _icon_montage*.png）：
  - 画布 90x91，有效图形是直径约 88 的实心圆，居中
  - 圆底主色：橙 (224,128,0) / 灰 (128,128,128) / 白 (224,224,224) / 绿 (160,224,64)
  - 字形是纯白或深色，压在圆底上；全部带 alpha 透明边

⚠ 「2048」是四个字符，按原厂那种「字形占满直径」的尺度排一行，
   单行字高只有 ~21px，在小尺寸下偏瘦。所以这里一次画三个候选、
   拼成对照图让人眼挑（见 _icon_montage_2048.png）：

     A 单行 2048（Arial Black，最直白，但字瘦）
     B 双行 20 / 48（字大、辨识强，但可能被读成两个数）
     C 圆底内嵌白色方牌 + 橙色 2048（最像 2048 的牌面，但小尺寸糊成一团）
     D 单行 2048（Impact：窄体 3.62 -> 2.56 字宽高比，同宽度下字高 +40%）

   实测选择见文件末尾 CHOICE。

用法：python make_game2048_icon.py
"""

import os

from PIL import Image, ImageDraw, ImageFont

# ---- 画布：和原厂 img_clock.png 一样 90x91，超采样 4 倍做抗锯齿 ----
W, H = 90, 91
SS = 4

# ---- 配色（圆底取自原厂图标实测值）----
ORANGE = (224, 128, 0, 255)
WHITE = (235, 235, 235, 255)
TRANSPARENT = (0, 0, 0, 0)

# 2048 官方的牌面橙（候选 C 用）
TILE_BEIGE = (237, 224, 200, 255)

FONT_PATH = r"C:\Windows\Fonts\ariblk.ttf"      # Arial Black：笔画最粗，小字号下最清楚
FONT_PATH_ALT = r"C:\Windows\Fonts\arialbd.ttf"  # 兜底
FONT_PATH_NARROW = r"C:\Windows\Fonts\impact.ttf"  # 窄体重字：单行四位数能排得更高

OUT_DIR = r"C:\Users\18058\Desktop\huangshanpai\watch\lvgl\watch\src\resource\images\common\ezip"
OUT_NAME = "img_game2048.png"

# 最终选定：D（Impact 窄体重字单行 2048）
#
# 比对过程（见 _icon_montage_2048.png / _icon_2048_sizes.png，三档 90 / 56 / 40px）：
#   A 字高只有 21px，整圈看着空，和原厂图标的字重不匹配；
#   B 双行虽然最粗壮，但"20 / 48"会被读成两个数；
#   C 的米色方牌在 40px 下糊成一块，且不属原厂"实心圆 + 白字形"的语汇；
#   D 换成窄体 Impact 后，同宽 78px 下字高约 30px（比 A 高 40%），
#     笔画厚度与原厂计算器/闹钟图标一致，缩到 40px 仍能一眼读出。
CHOICE = "D"


def px(v):
    """逻辑像素 -> 超采样画布像素"""
    return v * SS


def load_font(size_logical, path=None):
    path = path or (FONT_PATH if os.path.exists(FONT_PATH) else FONT_PATH_ALT)
    return ImageFont.truetype(path, int(size_logical * SS))


def fit_font(text, target_w_logical, path=None):
    """返回一个「画出来宽度接近 target_w」的字体。二分找字号，避免手算 em 比例。"""
    lo, hi = 8, 400
    for _ in range(40):
        mid = (lo + hi) / 2.0
        f = load_font(mid, path)
        bb = f.getbbox(text)
        w = (bb[2] - bb[0]) / SS
        if w < target_w_logical:
            lo = mid
        else:
            hi = mid
    return load_font(lo, path)


def text_center(d, text, font, cx_logical, cy_logical, fill):
    """把文字按「视觉外框」居中放在 (cx, cy)，而不是按基线。"""
    bb = d.textbbox((0, 0), text, font=font)
    tw = (bb[2] - bb[0]) / SS
    th = (bb[3] - bb[1]) / SS
    x = (cx_logical - tw / 2.0) * SS - bb[0]
    y = (cy_logical - th / 2.0) * SS - bb[1]
    d.text((x, y), text, font=font, fill=fill)


def base_circle():
    im = Image.new("RGBA", (W * SS, H * SS), TRANSPARENT)
    d = ImageDraw.Draw(im)
    # 圆底：直径 88，圆心 (45, 45.5) -> 外接框 1..89，和原厂完全对齐
    d.ellipse([px(1), px(1.5), px(89), px(89.5)], fill=ORANGE)
    return im, d


def variant_a():
    """单行 2048。宽度顶到 76（在圆内 y 中带还能留边），字高自然约 21。"""
    im, d = base_circle()
    f = fit_font("2048", 76)
    text_center(d, "2048", f, 45, 45.5, WHITE)
    return im.resize((W, H), Image.LANCZOS)


def variant_b():
    """双行 20 / 48。每行两个字，字号可以大一倍。"""
    im, d = base_circle()
    f = fit_font("20", 54)
    text_center(d, "20", f, 45, 31.0, WHITE)
    text_center(d, "48", f, 45, 60.0, WHITE)
    return im.resize((W, H), Image.LANCZOS)


def variant_c():
    """圆底内嵌白色方牌（2048 的牌面），牌上写橙色 2048。"""
    im, d = base_circle()
    d.rounded_rectangle([px(14), px(18), px(76), px(74)],
                        radius=px(10), fill=TILE_BEIGE)
    f = fit_font("2048", 52)
    text_center(d, "2048", f, 45, 45.5, ORANGE)
    return im.resize((W, H), Image.LANCZOS)


def variant_d():
    """单行 2048，用窄体 Impact：同宽度下字高约 30px（A 只有 21px）。"""
    im, d = base_circle()
    f = fit_font("2048", 78, FONT_PATH_NARROW if os.path.exists(FONT_PATH_NARROW) else None)
    text_center(d, "2048", f, 45, 45.5, WHITE)
    return im.resize((W, H), Image.LANCZOS)


def montage(icons, path):
    """把候选插在原厂图标中间，方便人眼比对风格/大小/对齐。"""
    names = ["img_clock", "img_alarm", "img_stopwatch", "img_calculator", "img_settings"]
    cell = 110
    tiles = []
    for n in names:
        p = os.path.join(OUT_DIR, n + ".png")
        if os.path.exists(p):
            tiles.append((n, Image.open(p).convert("RGBA")))
    # 把候选插在正中间，两侧是原厂图标
    for label, im in reversed(icons):
        tiles.insert(3, (label, im))

    cols = 4
    rows = (len(tiles) + cols - 1) // cols
    canvas = Image.new("RGBA", (cell * cols, cell * rows), (28, 32, 40, 255))
    for i, (n, im) in enumerate(tiles):
        t = im.resize((cell - 10, cell - 10), Image.LANCZOS)
        canvas.alpha_composite(t, ((i % cols) * cell + 5, (i // cols) * cell + 5))
    canvas.convert("RGB").save(path)
    return [n for n, _ in tiles]


def stat(icon, label):
    a = icon.getchannel("A")
    try:
        data = list(a.get_flattened_data())          # Pillow >= 11
    except AttributeError:
        data = list(a.getdata())
    opaque = sum(1 for v in data if v > 200)
    print("  %-6s corner_alpha=%d (0 才对)  opacity>200 像素=%d"
          % (label, a.getpixel((0, 0)), opaque))


def main():
    variants = [("A 单行", variant_a()),
                ("B 双行", variant_b()),
                ("C 方牌", variant_c()),
                ("D 窄体", variant_d())]

    here = os.path.dirname(os.path.abspath(__file__))

    # 1) 三个候选全部落盘，便于逐个放大看
    for label, im in variants:
        tag = label.split()[0]
        im.save(os.path.join(here, "_icon_2048_%s.png" % tag))

    # 2) 对照图
    mp = os.path.join(here, "_icon_montage_2048.png")
    order = montage(variants, mp)

    # 3) 按 CHOICE 把选中那版写进工程资源目录
    picked = dict((l.split()[0], im) for l, im in variants)[CHOICE]
    dst = os.path.join(OUT_DIR, OUT_NAME)
    picked.save(dst)

    print("saved   :", dst, "(choice = %s)" % CHOICE)
    print("size    : %dx%d" % picked.size)
    for label, im in variants:
        stat(im, label.split()[0])
    print("montage :", mp)
    print("order   : " + " | ".join(order))


if __name__ == "__main__":
    main()
