#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
黄山派表盘「手环风」资源生成器  (v3)
====================================
一次运行生成：
  1) 32 个 PNG 贴图，写进工程的图片资源目录，scons 会自动编译成 LVGL 图像描述符
  2) 一个 wf_layout.h —— 实测尺寸算出的排版常量，C 代码直接 #include，改字号会自动同步
  3) 一张合成预览图，上板子之前先看效果

生成物（390x450 屏）：
  大号时间数字  wf_d0..wf_d9   等宽单元（每格一样宽，换秒不跳字）
  时间冒号      wf_colon
  小号日期数字  wf_s0..wf_s9
  日期汉字      wf_ym(月) wf_yd(日) wf_w1..wf_w7(周一..周日)
  背景          wf_bg         ← 想换背景就替换这一个文件
  压暗层        wf_scrim      ← 垫在文字下面，换成任何照片白字都还读得清

关于"为什么看不出描边"这类问题的根治办法：
  这里所有字形都走 bake() —— 先画到一张足够大的画布上，量出真实可见像素的
  包围盒（含描边和投影），再裁到刚好包住。所以边距是"量出来的"而不是"猜的"，
  物理上不可能被裁切。改字号/描边/投影都不需要跟着调 padding。

用法：
    python make_assets.py             # 生成并写入工程
    python make_assets.py --preview   # 只写到 ui-preview/_gen，不碰工程

改配色/字号/字体：只改下面 CONFIG 区，重跑即可。C 代码不用动。
"""
import os
import sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

# ─── CONFIG ────────────────────────────────────────────────────────────────
HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)                      # ...\watch
SRC = os.path.join(PROJ, 'lvgl', 'watch', 'src')
IMG_EZIP = os.path.join(SRC, 'resource', 'images', 'common', 'ezip')
IMG_LARGE = os.path.join(SRC, 'resource', 'images', 'common', 'large_ezip')
CLOCK_DIR = os.path.join(SRC, 'gui_apps', 'clock')
PREVIEW_DIR = os.path.join(PROJ, 'ui-preview')

THEME = 'dark'            # 'dark' = 白字深底（手环风，推荐）; 'light' = 黑字浅底

SCR_W, SCR_H = 390, 450   # = LCD_HOR_RES_MAX x LCD_VER_RES_MAX
ROW_MARGIN = 10           # 时间行两侧最小留白
GROUP_CENTER_Y = 218      # 「时间+日期」整组的视觉中心
DATE_GAP = 14             # 时间行与日期行的竖向间距

GAP_BIG = 4               # 大号数字之间的呼吸感（在投影包围盒之外再让开这么多）
GAP_SMALL = 3             # 日期行每个贴图自带的内边距（boxed() 用它把墨迹居中）
DATE_ITEM_GAP = 0         # 日期行相邻贴图之间「再」让开的距离。
                          # 0 = 只靠贴图自带的 3px；调大 = 更松的排版。
                          # 注意：和 GAP_SMALL 是两个概念，别混。
COLON_DOT_R = 10          # 冒号圆点半径
SMALL_TARGET_H = 24       # 日期行字形目标高度（含描边）

# 大号数字：纯白无描边 + 贴地投影，把宽度尽量让给字形
BIG = dict(outline=0, blur=3, dy=4, shadow=(0, 0, 0, 170))
# 日期行：小字必须有描边，否则压在照片上根本读不出来
SMALL = dict(outline=2, blur=2, dy=2, shadow=(0, 0, 0, 155))

if THEME == 'dark':
    FG, STROKE = (255, 255, 255, 255), (0, 0, 0, 210)
    BIG = dict(BIG, fg=FG, stroke=STROKE)
    SMALL = dict(SMALL, fg=FG, stroke=STROKE)
    BG_PRESET, SCRIM_PRESET = 'dark', 'dark'
else:
    FG, STROKE = (24, 26, 32, 255), (255, 255, 255, 245)
    BIG = dict(BIG, fg=FG, stroke=STROKE, shadow=(255, 255, 255, 215))
    SMALL = dict(SMALL, fg=FG, stroke=STROKE, shadow=(255, 255, 255, 200))
    BG_PRESET, SCRIM_PRESET = 'light', 'light'

LATIN_CANDS = [
    r'C:\Windows\Fonts\seguisb.ttf',
    r'C:\Windows\Fonts\segoeuib.ttf',
    r'C:\Windows\Fonts\segoeui.ttf',
    r'C:\Windows\Fonts\msyhbd.ttc',
    r'C:\Windows\Fonts\msyh.ttc',
    r'C:\Windows\Fonts\arialbd.ttf',
    r'C:\Windows\Fonts\arial.ttf',
]
CJK_CANDS = [
    os.path.join(SRC, 'resource', 'fonts', 'freetype', 'SourceHanSansCN-Normal.ttf'),
    r'C:\Windows\Fonts\msyhbd.ttc',
    r'C:\Windows\Fonts\msyh.ttc',
    os.path.join(SRC, 'resource', 'fonts', 'freetype', 'DroidSansFallback.ttf'),
]

WEEKDAYS = ['一', '二', '三', '四', '五', '六', '日']

SIZES = {}       # name -> (w, h)
REPORT = []


# ─── 工具 ──────────────────────────────────────────────────────────────────
def pick(cands):
    for c in cands:
        if os.path.isfile(c):
            return c
    raise SystemExit('找不到可用字体:\n  ' + '\n  '.join(cands))


def bake(text, font_path, size, st):
    """
    把「字形 + 描边 + 投影」画到一张足够大的画布上，然后裁到刚好包住全部
    可见像素。返回值直接就是"字形贴图"，尺寸即真实边距，不用担心裁切。
    """
    f = ImageFont.truetype(font_path, size)
    bb = f.getbbox(text)
    m = st['outline'] + st['blur'] + st['dy'] + 6      # 画布留白，给描边+投影+模糊
    W, H = (bb[2] - bb[0]) + 2 * m, (bb[3] - bb[1]) + 2 * m
    canvas = Image.new('RGBA', (max(W, 1), max(H, 1)), (0, 0, 0, 0))
    ox, oy = m - bb[0], m - bb[1]

    if st['blur'] > 0:
        sh = Image.new('RGBA', canvas.size, (0, 0, 0, 0))
        ImageDraw.Draw(sh).text((ox, oy), text, font=f, fill=st['shadow'],
                                stroke_width=st['outline'], stroke_fill=st['shadow'])
        canvas.alpha_composite(sh.filter(ImageFilter.GaussianBlur(st['blur'])), (0, st['dy']))

    ImageDraw.Draw(canvas).text(
        (ox, oy), text, font=f, fill=st['fg'],
        stroke_width=st['outline'],
        stroke_fill=st['stroke'] if st['outline'] else None)

    ab = canvas.split()[3].getbbox()
    return canvas.crop(ab)


def boxed(layers, gap, cell_w=None, cell_h=None):
    """把若干贴图放进统一大小的单元里居中；左右/上下各留 gap"""
    cw = cell_w or (max(l.width for l in layers) + gap)
    ch = cell_h or (max(l.height for l in layers) + gap)
    out = []
    for l in layers:
        c = Image.new('RGBA', (cw, ch), (0, 0, 0, 0))
        c.alpha_composite(l, ((cw - l.width) // 2, (ch - l.height) // 2))
        out.append(c)
    return out, cw, ch


def save_imgs(imgs, prefix, out_dir):
    for i, im in enumerate(imgs):
        name = '%s%d' % (prefix, i) if len(imgs) > 1 else prefix
        im.save(os.path.join(out_dir, name + '.png'))
        SIZES[name] = im.size


# ─── 大号时间数字 ──────────────────────────────────────────────────────────
def build_colon(w, h, cy):
    """在一张 w x h 的画布上画一对圆点，点对中心落在 cy"""
    im = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    cx = w / 2
    off = round(h * 0.20)
    for c in (cy - off, cy + off):
        box = [cx - COLON_DOT_R, c - COLON_DOT_R, cx + COLON_DOT_R, c + COLON_DOT_R]
        if BIG['blur'] > 0:
            sh = Image.new('RGBA', (w, h), (0, 0, 0, 0))
            ImageDraw.Draw(sh).ellipse(
                [box[0], box[1] + BIG['dy'], box[2], box[3] + BIG['dy']], fill=BIG['shadow'])
            im.alpha_composite(sh.filter(ImageFilter.GaussianBlur(BIG['blur'])))
        ImageDraw.Draw(im).ellipse(box, fill=BIG['fg'])
    return im


def colon_width():
    """
    投影的高斯尾巴比 blur 参数伸得远得多，所以宽度不靠算，靠"试着画，量出来"。
    从窄到宽试，直到左右两侧都真的留出了余量。
    """
    for w in range(2 * COLON_DOT_R + 4, 2 * COLON_DOT_R + 80):
        ab = build_colon(w, 400, 200).split()[3].getbbox()
        if ab and ab[0] > 0 and ab[2] < w:
            return w
    return 2 * COLON_DOT_R + 80


def make_big(latin, out_dir):
    """
    字号不靠猜：先在参考字号下量一次包围盒宽度，按线性关系解出能放进
    ROW_MARGIN 内的最大字号，再回代验证、必要时逐像素退让。
    """
    colon_w = colon_width()
    ref_size = 200
    ref = [bake(str(n), latin, ref_size, BIG) for n in range(10)]
    ref_w = max(l.width for l in ref)

    budget = SCR_W - 2 * ROW_MARGIN
    size = int((budget - colon_w - 4 * GAP_BIG) * ref_size / (4.0 * ref_w))

    while size > 20:
        layers = [bake(str(n), latin, size, BIG) for n in range(10)]
        cell_w = max(l.width for l in layers) + GAP_BIG
        if 4 * cell_w + colon_w <= budget:
            break
        size -= 1

    cells, cell_w, cell_h = boxed(layers, GAP_BIG)
    save_imgs(cells, 'wf_d', out_dir)

    # 墨迹中心不等于单元中心：单元下方要多留一份投影的位置
    vis_cy = cell_h / 2 - BIG['dy'] / 2
    build_colon(colon_w, cell_h, vis_cy).save(os.path.join(out_dir, 'wf_colon.png'))
    SIZES['wf_colon'] = (colon_w, cell_h)

    vis = visible_h(latin, '8', size, BIG['outline'])
    REPORT.append('大号数字   : 字号=%d  单元=%dx%d  冒号格=%d  时间行=%d  两侧留白=%d'
                  % (size, cell_w, cell_h, colon_w, 4 * cell_w + colon_w,
                     (SCR_W - 4 * cell_w - colon_w) // 2))
    REPORT.append('             可见数字高=%d px = 屏高 %.0f%%   数字占宽 %d/390 = %.0f%%'
                  % (vis, 100.0 * vis / SCR_H, 4 * cell_w + colon_w,
                     100.0 * (4 * cell_w + colon_w) / SCR_W))
    return dict(size=size, cell_w=cell_w, cell_h=cell_h, colon_w=colon_w, vis_h=vis)


# ─── 日期行 ────────────────────────────────────────────────────────────────
def visible_h(font_path, text, size, outline):
    """
    眼睛能看到的字形高度 = 字形墨迹 + 上下描边。
    注意别拿 bake() 出来的贴图高度当字号依据 —— 那里面还含着投影和模糊的余量，
    用它定字号会把字选得又小又飘（这是上一版踩过的坑）。
    """
    bb = ImageFont.truetype(font_path, size).getbbox(text)
    return (bb[3] - bb[1]) + 2 * outline


def fit_small(font_path, text, target_h):
    """选一个让「可见高度（含描边）」最接近 target_h 的字号"""
    best, bd = 8, 1 << 30
    for s in range(8, 120):
        d = abs(visible_h(font_path, text, s, SMALL['outline']) - target_h)
        if d < bd:
            best, bd = s, d
        if d == 0:
            break
    return best


def make_date_row(latin, cjk, out_dir):
    size_lat = fit_small(latin, '0', SMALL_TARGET_H)
    size_cjk = fit_small(cjk, '月', SMALL_TARGET_H)

    dig = [bake(str(n), latin, size_lat, SMALL) for n in range(10)]
    digs, dig_w, cell_h = boxed(dig, GAP_SMALL)
    save_imgs(digs, 'wf_s', out_dir)

    ym = bake('月', cjk, size_cjk, SMALL)
    yd = bake('日', cjk, size_cjk, SMALL)
    wk = [bake('周' + d, cjk, size_cjk, SMALL) for d in WEEKDAYS]
    wks, wk_w, _ = boxed(wk, GAP_SMALL, cell_h=cell_h)

    def one(img, name, w):
        c = Image.new('RGBA', (w, cell_h), (0, 0, 0, 0))
        c.alpha_composite(img, ((w - img.width) // 2, (cell_h - img.height) // 2))
        c.save(os.path.join(out_dir, name + '.png'))
        SIZES[name] = c.size

    ym_w, yd_w = ym.width + GAP_SMALL, yd.width + GAP_SMALL
    one(ym, 'wf_ym', ym_w)
    one(yd, 'wf_yd', yd_w)
    for i, c in enumerate(wks, start=1):
        c.save(os.path.join(out_dir, 'wf_w%d.png' % i))
        SIZES['wf_w%d' % i] = c.size

    REPORT.append('日期行     : 数字字号=%d(可见高%d) 汉字字号=%d(可见高%d)'
                  % (size_lat, visible_h(latin, '0', size_lat, SMALL['outline']),
                     size_cjk, visible_h(cjk, '月', size_cjk, SMALL['outline'])))
    REPORT.append('             数字格=%dx%d  月=%d 日=%d 周X=%d'
                  % (dig_w, cell_h, ym_w, yd_w, wk_w))
    return dict(digit_w=dig_w, ym_w=ym_w, yd_w=yd_w, week_w=wk_w, cell_h=cell_h)


# ─── 日期行元素顺序（全工程唯一来源）──────────────────────────────────────
def mon_day_tokens(mon, day):
    """
    「9月20日」的元素顺序 —— 数字在前、单位在后。

    这是唯一一处决定顺序的地方：预览、宽度统计、自检全部调它。
    上一版就是在别处手写了顺序，写成「[月] 在数字前面」并且漏掉月份个位，
    结果屏幕上成了「月20日」—— 月份数字整个消失。别再手写这个列表。
    """
    t = []
    if mon >= 10:
        t.append(('digit', 'wf_s%d' % (mon // 10), mon // 10))
    t.append(('digit', 'wf_s%d' % (mon % 10), mon % 10))
    t.append(('unit', 'wf_ym', '月'))
    if day >= 10:
        t.append(('digit', 'wf_s%d' % (day // 10), day // 10))
    t.append(('digit', 'wf_s%d' % (day % 10), day % 10))
    t.append(('unit', 'wf_yd', '日'))
    return t


def tokens_to_text(toks):
    return ''.join(str(t[2]) for t in toks)


def text_to_tokens(s):
    """把「9月20日」独立解析一遍 —— 用来校验顺序，避免自己证明自己"""
    t = []
    for ch in s:
        if ch.isdigit():
            t.append(('digit', 'wf_s' + ch, int(ch)))
        else:
            t.append(('unit', 'wf_ym' if ch == '月' else 'wf_yd', ch))
    return t


def check_date_order(cases):
    """
    覆盖 一位/两位 月 与 日 的全部四种组合：
    拼出来的顺序必须等于「<月>月<日>日」，且逐 token 与独立解析的结果一致。
    不一致直接终止 —— 宁可生成失败，也不要再烧一块显示错的板子上去。
    """
    for mon, day in cases:
        want = '%d月%d日' % (mon, day)
        got = tokens_to_text(mon_day_tokens(mon, day))
        if got != want:
            raise SystemExit('日期行顺序错了: 期望 %s，实际拼出 %s' % (want, got))
        if mon_day_tokens(mon, day) != text_to_tokens(want):
            raise SystemExit('日期行 token 与文字不一致: %s' % want)
    return ' / '.join('%d月%d日' % c for c in cases)


# ─── 背景 & 压暗层 ─────────────────────────────────────────────────────────
def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def make_bg(path, preset):
    if preset == 'light':
        top, bot, tint = (250, 251, 253), (214, 222, 236), (255, 255, 255)
    else:
        top, bot, tint = (32, 40, 60), (8, 10, 17), (86, 132, 214)

    im = Image.new('RGB', (SCR_W, SCR_H))
    d = ImageDraw.Draw(im)
    for y in range(SCR_H):
        d.line([(0, y), (SCR_W, y)], fill=lerp(top, bot, y / (SCR_H - 1)))

    glow = Image.new('L', (SCR_W, SCR_H), 0)
    ImageDraw.Draw(glow).ellipse([SCR_W * 0.02, 70, SCR_W * 0.98, 320], fill=80)
    glow = glow.filter(ImageFilter.GaussianBlur(64))
    im = Image.composite(Image.blend(im, Image.new('RGB', (SCR_W, SCR_H), tint), 0.36), im, glow)

    vg = Image.new('L', (SCR_W, SCR_H), 0)
    ImageDraw.Draw(vg).ellipse([-SCR_W * 0.28, -SCR_H * 0.18, SCR_W * 1.28, SCR_H * 1.18], fill=255)
    vg = vg.filter(ImageFilter.GaussianBlur(70))
    im = Image.composite(im, Image.new('RGB', (SCR_W, SCR_H), (0, 0, 0)), vg)

    im.save(path)
    SIZES['wf_bg'] = im.size


def make_scrim(path, preset):
    """
    一层柔和的压暗（亮主题下是提亮）薄膜，垫在时间/日期下面。
    这是"自定义背景"能不能用的关键：换成任何照片，白字都还读得清。
    """
    a_max = 135 if preset == 'dark' else 150
    col = (0, 0, 0) if preset == 'dark' else (255, 255, 255)
    m = Image.new('L', (SCR_W, SCR_H), 0)
    ImageDraw.Draw(m).ellipse([-30, 92, SCR_W + 30, 346], fill=255)
    m = m.filter(ImageFilter.GaussianBlur(58))
    alpha = m.point(lambda v: v * a_max // 255)
    im = Image.new('RGBA', (SCR_W, SCR_H), col + (0,))
    im.putalpha(alpha)
    im.save(path)
    SIZES['wf_scrim'] = im.size


# ─── 预览合成 ──────────────────────────────────────────────────────────────
def build_date_row_img(out_ezip, date, mon, day, wd):
    """
    把一整个日期行按 mon_day_tokens() 的顺序拼出来。
    规则和 C 侧 band_set_date() 逐字对应：数字在前、单位在后，整体居中，
    日期与星期之间多留 WEEK_SEP。
    """
    sm = lambda n: Image.open(os.path.join(out_ezip, n + '.png'))
    items = [sm(t[1]) for t in mon_day_tokens(mon, day)]
    week = sm('wf_w%d' % wd)
    total = (sum(i.width for i in items) + DATE_ITEM_GAP * (len(items) - 1)
             + WEEK_SEP + week.width)
    x = (SCR_W - total) // 2
    row = Image.new('RGBA', (SCR_W, date['cell_h']), (0, 0, 0, 0))
    for i in items:
        row.alpha_composite(i, (x, 0))
        x += i.width + DATE_ITEM_GAP
    x += WEEK_SEP - DATE_ITEM_GAP
    row.alpha_composite(week, (x, 0))
    return row, total


def compose_preview(out_ezip, out_large, big, date, hhmm=(10, 8), mon=9, day=20, wd=7):
    bg = Image.open(os.path.join(out_large, 'wf_bg.png')).convert('RGBA')
    bg.alpha_composite(Image.open(os.path.join(out_large, 'wf_scrim.png')).convert('RGBA'))

    sm = lambda n: Image.open(os.path.join(out_ezip, n + '.png'))
    dig = [sm('wf_d%d' % n) for n in range(10)]
    seq = [dig[hhmm[0] // 10], dig[hhmm[0] % 10], sm('wf_colon'),
           dig[hhmm[1] // 10], dig[hhmm[1] % 10]]
    x = (SCR_W - sum(s.width for s in seq)) // 2
    row = Image.new('RGBA', (SCR_W, big['cell_h']), (0, 0, 0, 0))
    for s in seq:
        row.alpha_composite(s, (x, 0))
        x += s.width

    drow, date_row_w = build_date_row_img(out_ezip, date, mon, day, wd)

    group_h = big['cell_h'] + DATE_GAP + date['cell_h']
    time_y = GROUP_CENTER_Y - group_h // 2
    date_y = time_y + big['cell_h'] + DATE_GAP
    bg.alpha_composite(row, (0, time_y))
    bg.alpha_composite(drow, (0, date_y))

    bg.convert('RGB').save(os.path.join(PREVIEW_DIR, 'wf_mockup.png'))
    bg.convert('RGB').resize((SCR_W * 2, SCR_H * 2), Image.LANCZOS).save(
        os.path.join(PREVIEW_DIR, 'wf_mockup@2x.png'))

    # 日期行的四种形态（一位/两位 月与日）单独做一张对照图，一眼看完有没有排错
    cases = [(1, 2, 5), (9, 20, 7), (10, 8, 4), (12, 30, 3)]
    pad, ch = 12, date['cell_h']
    variants = []
    strip = Image.new('RGB', (SCR_W, pad + len(cases) * (ch + pad)), (18, 20, 26))
    y = pad
    for m, d, w in cases:
        r, wid = build_date_row_img(out_ezip, date, m, d, w)
        strip.paste(r, (0, y), r)
        y += ch + pad
        variants.append((m, d, w, wid))
    strip.save(os.path.join(PREVIEW_DIR, 'wf_date_variants.png'))
    strip.resize((SCR_W * 2, strip.height * 2), Image.LANCZOS).save(
        os.path.join(PREVIEW_DIR, 'wf_date_variants@2x.png'))

    return dict(time_y=time_y, date_y=date_y, group_h=group_h,
                date_row_w=date_row_w, variants=variants)


WEEK_SEP = 22   # 日期与星期之间的额外间距


# ─── 生成 wf_layout.h ─────────────────────────────────────────────────────
def write_layout_h(path, big, date, pos):
    txt = '''/* 本文件由 watchface-tools/make_assets.py 自动生成，请勿手改。
 * 想改字号/配色/留白 —— 改 make_assets.py 的 CONFIG 区然后重跑，这里会跟着更新。 */
#ifndef WF_LAYOUT_H
#define WF_LAYOUT_H

#define WF_SCR_W               %(W)d
#define WF_SCR_H               %(H)d

/* ---- 大号时间 HH:MM ---- */
#define WF_TIME_Y              %(time_y)d
#define WF_DIGIT_W             %(dig_w)d
#define WF_DIGIT_H             %(dig_h)d
#define WF_COLON_W             %(col_w)d
#define WF_COLON_H             %(col_h)d
/* 时间行在屏上的左起 x（水平居中；0号格/1号格/冒号/2号格/3号格 依次排开）*/
#define WF_TIME_X0             %(x0)d

/* ---- 日期行 ---- */
#define WF_DATE_Y              %(date_y)d
#define WF_DATE_H              %(date_h)d
#define WF_SDIGIT_W            %(sdig_w)d
#define WF_YM_W                %(ym_w)d
#define WF_YD_W                %(yd_w)d
#define WF_WEEK_W              %(week_w)d
#define WF_DAY_GAP             %(day_gap)d
#define WF_WEEK_SEP            %(week_sep)d

#endif /* WF_LAYOUT_H */
''' % dict(W=SCR_W, H=SCR_H,
           time_y=pos['time_y'], dig_w=big['cell_w'], dig_h=big['cell_h'],
           col_w=big['colon_w'], col_h=big['cell_h'],
           x0=(SCR_W - (4 * big['cell_w'] + big['colon_w'])) // 2,
           date_y=pos['date_y'], date_h=date['cell_h'],
           sdig_w=date['digit_w'], ym_w=date['ym_w'], yd_w=date['yd_w'],
           week_w=date['week_w'], day_gap=DATE_ITEM_GAP, week_sep=WEEK_SEP)
    with open(path, 'w', encoding='utf-8') as f:
        f.write(txt)


# ─── 主流程 ────────────────────────────────────────────────────────────────
def main():
    preview_only = '--preview' in sys.argv
    latin, cjk = pick(LATIN_CANDS), pick(CJK_CANDS)
    REPORT.append('主题       : %s' % THEME)
    REPORT.append('数字字体   : %s' % latin)
    REPORT.append('汉字字体   : %s' % cjk)
    REPORT.append('')

    out_ezip, out_large = IMG_EZIP, IMG_LARGE
    if preview_only:
        out_ezip = out_large = os.path.join(PREVIEW_DIR, '_gen')
    os.makedirs(out_ezip, exist_ok=True)
    os.makedirs(out_large, exist_ok=True)
    os.makedirs(PREVIEW_DIR, exist_ok=True)

    big = make_big(latin, out_ezip)
    date = make_date_row(latin, cjk, out_ezip)
    make_bg(os.path.join(out_large, 'wf_bg.png'), BG_PRESET)
    make_scrim(os.path.join(out_large, 'wf_scrim.png'), SCRIM_PRESET)

    REPORT.append('日期行顺序 : 自检通过  %s' % check_date_order(
        [(1, 2), (9, 20), (10, 8), (11, 9), (12, 30), (3, 31)]))
    REPORT.append('')

    pos = compose_preview(out_ezip, out_large, big, date)
    REPORT.append('时间行位置 : y=%d..%d   左右留白 %d'
                  % (pos['time_y'], pos['time_y'] + big['cell_h'],
                     (SCR_W - 4 * big['cell_w'] - big['colon_w']) // 2))
    REPORT.append('日期行位置 : y=%d..%d   日期行宽 %d  两侧留白 %d'
                  % (pos['date_y'], pos['date_y'] + date['cell_h'], pos['date_row_w'],
                     (SCR_W - pos['date_row_w']) // 2))
    wm, wd_, ww, ww_px = max(pos['variants'], key=lambda v: v[3])
    REPORT.append('日期行宽度 : 元素间隔 %d px + 贴图自带 %d px；最宽是 %d月%d日 = %d px'
                  % (DATE_ITEM_GAP, GAP_SMALL, wm, wd_, ww_px))
    REPORT.append('整组高度   : %d  (屏高 %d, 上方空 %d, 下方空 %d)'
                  % (pos['group_h'], SCR_H, pos['time_y'],
                     SCR_H - pos['date_y'] - date['cell_h']))
    REPORT.append('')

    if not preview_only:
        write_layout_h(os.path.join(CLOCK_DIR, 'wf_layout.h'), big, date, pos)
        REPORT.append('已写出     : %s' % os.path.join(CLOCK_DIR, 'wf_layout.h'))
        REPORT.append('已写出     : %s\\wf_*.png (%d 个)' % (IMG_EZIP, len(SIZES) - 2))
        REPORT.append('已写出     : %s\\wf_bg.png, wf_scrim.png' % IMG_LARGE)
        REPORT.append('')
        REPORT.append('资源清单 (%d 个):' % len(SIZES))
        for k in sorted(SIZES):
            REPORT.append('   %-12s %3d x %-3d' % (k, SIZES[k][0], SIZES[k][1]))
    else:
        REPORT.append('(--preview 模式，未写入工程)')

    with open(os.path.join(HERE, 'make_assets.log'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(REPORT))
    print('\n'.join(REPORT))


if __name__ == '__main__':
    main()
