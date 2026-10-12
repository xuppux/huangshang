#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""验证合成预览图：确认文字真的画在了预期位置、亮度正确（不是一张空背景）"""
import os
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
P = os.path.join(PROJ, 'ui-preview')
im = Image.open(os.path.join(P, 'wf_mockup.png')).convert('RGB')
W, H = im.size

L = ['预览图 %dx%d' % (W, H), '']

# 逐行统计"接近白色"的像素数（白字 >= 235）
rows = []
px = im.load()
for y in range(H):
    c = 0
    for x in range(0, W, 2):
        r, g, b = px[x, y]
        if r >= 235 and g >= 235 and b >= 235:
            c += 1
    rows.append(c)

# 找出有白字的连续区间
bands, start = [], None
for y in range(H):
    if rows[y] > 2 and start is None:
        start = y
    elif rows[y] <= 2 and start is not None:
        bands.append((start, y - 1, max(rows[start:y])))
        start = None
if start is not None:
    bands.append((start, H - 1, max(rows[start:])))

L.append('=== 白字行区间（y 起-止, 该区间单行最多白像素数）===')
for a, b, m in bands:
    if b - a >= 2:
        L.append('  y=%3d..%-3d  高%3d  峰值白像素 %3d' % (a, b, b - a + 1, m))
if not bands:
    L.append('  !! 一个白字都没有 —— 预览图是空的')

# 时间行应占 y=WF_TIME_Y .. +WF_DIGIT_H（140..245），日期行 259..296
L.append('')
L.append('=== 与 wf_layout.h 期望位置对照 ===')
import re
lay = {}
p = os.path.join(PROJ, 'lvgl', 'watch', 'src', 'gui_apps', 'clock', 'wf_layout.h')
for line in open(p, encoding='utf-8'):
    m = re.match(r'#define\s+(WF_\w+)\s+(\d+)', line)
    if m:
        lay[m.group(1)] = int(m.group(2))

for name, a, b in (('时间行', lay['WF_TIME_Y'], lay['WF_TIME_Y'] + lay['WF_DIGIT_H']),
                   ('日期行', lay['WF_DATE_Y'], lay['WF_DATE_Y'] + lay['WF_DATE_H'])):
    L.append('  %s 期望 y=%d..%d' % (name, a, b))

# 时间行里白像素的横向范围
ty0, ty1 = lay['WF_TIME_Y'], lay['WF_TIME_Y'] + lay['WF_DIGIT_H']
xs = [x for y in range(ty0, ty1) for x in range(W)
      if px[x, y][0] >= 235 and px[x, y][1] >= 235 and px[x, y][2] >= 235]
if xs:
    L.append('')
    L.append('时间行白像素横向范围 x=%d..%d  (期望约 %d..%d, 两侧留白 %d/%d)'
             % (min(xs), max(xs), lay['WF_TIME_X0'],
                lay['WF_TIME_X0'] + 4 * lay['WF_DIGIT_W'] + lay['WF_COLON_W'] - 1,
                min(xs), W - 1 - max(xs)))

# 冒号：圆点不在单元中线上，而是"点对中心 ± off"两处。
# 点对中心 = cell_h/2 - dy/2（投影占了下方），off = round(cell_h * 0.20)
cx0 = lay['WF_TIME_X0'] + 2 * lay['WF_DIGIT_W']
cx1 = cx0 + lay['WF_COLON_W']
dy, off = 4, round(lay['WF_DIGIT_H'] * 0.20)
c_center = ty0 + round(lay['WF_DIGIT_H'] / 2 - dy / 2)
L.append('冒号格 x=%d..%d  点对中心 y=%d  两点 y=%d / %d'
         % (cx0, cx1, c_center, c_center - off, c_center + off))
for yy in (c_center - off, c_center + off, c_center):
    hit = sum(1 for x in range(cx0, cx1) if px[x, yy][0] >= 235 and px[x, yy][1] >= 235)
    L.append('   y=%-3d 白像素 %2d %s' % (yy, hit, '(应为 0，点在两点之间)' if yy == c_center else '(应有白点)'))

# 日期行：横向墨迹范围 / 是否居中 / 墨迹块数
# 这一项验证的是「最终那张 PNG 里到底画了什么」，跟排版常量的对账。
d0, d1 = lay['WF_DATE_Y'], lay['WF_DATE_Y'] + lay['WF_DATE_H']
dxs = [x for y in range(d0, d1) for x in range(W)
       if px[x, y][0] >= 235 and px[x, y][1] >= 235 and px[x, y][2] >= 235]
L.append('')
if dxs:
    ink_w = max(dxs) - min(dxs) + 1
    L.append('日期行白像素横向范围 x=%d..%d  墨迹宽 %d  左留白 %d  右留白 %d'
             % (min(dxs), max(dxs), ink_w, min(dxs), W - 1 - max(dxs)))
    L.append('  水平居中偏差 %d px（应 <= 4）' % abs(min(dxs) - (W - 1 - max(dxs))))
    col = [any(px[x, y][0] >= 235 for y in range(d0, d1)) for x in range(W)]
    blobs, run, gap = [], 0, 0
    for v in col:
        if v:
            if gap >= 3 and run:
                blobs.append(run)
                run = 0
            gap = 0
            run += 1
        else:
            gap += 1
    if run:
        blobs.append(run)
    L.append('  墨迹块（按 >=3px 空白切分）: %d 个  宽度 %s'
             % (len(blobs), blobs))
    L.append('  预期: 9月20日 = 5 块 + 周日的 1~2 块（周/日之间可能不分开）')
else:
    L.append('日期行 !! 一个白像素都没有 —— 日期没画上去')

# 亮度分布：确认整体不是全黑也不是全白
small = im.resize((1, 1), Image.BOX).getpixel((0, 0))
L.append('')
L.append('整图均值 RGB = %s   (应偏暗，说明是深色底 + 少量白字)' % (small,))

open(os.path.join(HERE, 'preview_check.txt'), 'w', encoding='utf-8').write('\n'.join(L))
print('\n'.join(L))
