# -*- coding: utf-8 -*-
"""
画一张「手电筒」应用图标，风格对齐原厂那套（实心圆底 + 白色字形）。

和 make_icon.py（计算器图标）是同一套路子，差别只在字形：

    橙色圆底 + 白色手电筒（灯头 + 筒身 + 开关 + 三道光）

为什么不用现成图片：主菜单的图标不是随便的 PNG —— 放在
    watch/lvgl/watch/src/resource/images/common/ezip/
下面的每个 *.png 会被构建脚本的 Env.ImgResource() 自动编译成同名符号
（img_torch.png -> lv_img_dsc_t img_torch，ezip 压缩、LV_IMG_CF_RAW_ALPHA）。
所以新增图标只要丢一张 PNG 进去，不用改任何 SConscript、也不用改工程文件。

原厂图标的实测规律（见 _icon_montage.png）：
  - 画布 90x91，有效图形是直径约 88 的实心圆，居中
  - 圆底主色：橙 (224,128,0) / 灰 (128,128,128) / 白 (224,224,224) / 绿 (160,224,64)
  - 字形是纯白或深色，压在圆底上；全部带 alpha 透明边

用法：python make_torch_icon.py
"""

import os

from PIL import Image, ImageDraw

# ---- 画布：和原厂 img_clock.png 一样 90x91，超采样 4 倍做抗锯齿 ----
W, H = 90, 91
SS = 4

# ---- 配色（取自原厂图标实测值）----
ORANGE = (224, 128, 0, 255)
WHITE = (235, 235, 235, 255)
TRANSPARENT = (0, 0, 0, 0)

OUT_DIR = r"C:\Users\18058\Desktop\huangshanpai\watch\lvgl\watch\src\resource\images\common\ezip"
OUT_NAME = "img_torch.png"


def px(v):
    """逻辑像素 -> 超采样画布像素"""
    return v * SS


def draw_icon():
    im = Image.new("RGBA", (W * SS, H * SS), TRANSPARENT)
    d = ImageDraw.Draw(im)

    # 圆底：直径 88，圆心 (45, 45.5) -> 外接框 1..89，和原厂完全对齐
    d.ellipse([px(1), px(1.5), px(89), px(89.5)], fill=ORANGE)

    # ---- 字形：手电筒，整体 38 宽 x 62 高，居中（x 26..64, y 15..77）----
    #
    # 尺寸是和原厂图标比对出来的：第一版画了 30x56，放进 88 直径的圆里明显
    # 比 img_alarm / img_calculator 瘦一圈（见 _icon_montage_torch.png 第一版）。
    # 手电筒本来就细长，所以只在宽度上补，高度基本不动。

    # ① 三道光：从灯头上方散开。中间那道最长，两侧斜出去。
    d.line([px(45), px(26), px(45), px(15.5)], fill=WHITE, width=int(px(4.2)))
    d.line([px(43.9), px(26.6), px(35.2), px(19.4)], fill=WHITE, width=int(px(3.8)))
    d.line([px(46.1), px(26.6), px(54.8), px(19.4)], fill=WHITE, width=int(px(3.8)))

    # ② 灯头：上宽下窄的梯形（手电筒的"喇叭口"），y 26..42
    d.polygon([(px(26), px(26)), (px(64), px(26)),
               (px(60), px(42)), (px(30), px(42))], fill=WHITE)

    # ③ 灯头里的镜片：用底色挖出来，读起来才像"灯"
    d.rounded_rectangle([px(29.5), px(29), px(60.5), px(36)], fill=ORANGE)

    # ④ 筒身：圆角矩形，比灯头窄，y 42..77
    d.rounded_rectangle([px(30), px(42), px(60), px(77)],
                        radius=px(6), fill=WHITE)

    # ⑤ 开关：筒身上挖一个小横条
    d.rounded_rectangle([px(38.5), px(48.5), px(51.5), px(55.5)],
                        radius=px(2.5), fill=ORANGE)

    # ⑥ 筒身下段挖两道防滑纹，避免和"电池"之类的图标撞脸
    for yy in (63.5, 69.5):
        d.rounded_rectangle([px(34.5), px(yy), px(55.5), px(yy + 3.0)],
                            radius=px(1.5), fill=ORANGE)

    return im.resize((W, H), Image.LANCZOS)


def montage(icon, path):
    """把新图标插在原厂图标中间，方便人眼比对风格/大小/对齐。"""
    names = ["img_clock", "img_alarm", "img_stopwatch", "img_calculator", "img_settings"]
    cell = 110
    tiles = []
    for n in names:
        p = os.path.join(OUT_DIR, n + ".png")
        if os.path.exists(p):
            tiles.append((n, Image.open(p).convert("RGBA")))
    tiles.insert(2, ("img_torch (new)", icon))

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

    # 自检：透明边、不透明像素数（太少说明字形没画上，太多说明圆底溢出）
    a = icon.getchannel("A")
    try:
        data = list(a.get_flattened_data())          # Pillow >= 11
    except AttributeError:
        data = list(a.getdata())
    opaque = sum(1 for v in data if v > 200)

    print("saved   :", dst)
    print("size    : %dx%d" % icon.size)
    print("alpha   : corner=%d (0 才对)  opacity>200 的像素=%d"
          % (a.getpixel((0, 0)), opaque))

    mp = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_icon_montage_torch.png")
    order = montage(icon, mp)
    print("montage :", mp)
    print("order   : " + " | ".join(order))


if __name__ == "__main__":
    main()
