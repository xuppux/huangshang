#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""诊断：字体在不同字号下 bake() 出来的真实尺寸（含描边/投影）"""
import os, sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
SRC = os.path.join(PROJ, 'lvgl', 'watch', 'src')

CJK = os.path.join(SRC, 'resource', 'fonts', 'freetype', 'SourceHanSansCN-Normal.ttf')
LAT = r'C:\Windows\Fonts\seguisb.ttf'

L = []
for path, label, texts in ((LAT, 'Segoe UI Semibold', ['0', '4']),
                           (CJK, 'SourceHanSansCN-Normal', ['月', '日', '周一'])):
    L.append('===== %s =====' % label)
    L.append('  exists=%s  size=%s' % (os.path.isfile(path), os.path.getsize(path) if os.path.isfile(path) else '-'))
    for s in (12, 16, 20, 24, 28, 32, 40, 60):
        try:
            f = ImageFont.truetype(path, s)
        except Exception as e:
            L.append('  size=%d  打开失败: %s' % (s, e))
            continue
        row = '  size=%-3d ' % s
        for t in texts:
            bb = f.getbbox(t)
            row += '| %s bbox=%s len=%.1f ' % (t, bb, f.getlength(t))
        L.append(row)
    # 真实渲染出的 alpha 包围盒
    L.append('  -- 实际渲染 (含 2px 描边) --')
    for t in texts:
        for s in (16, 24, 32):
            f = ImageFont.truetype(path, s)
            bb = f.getbbox(t)
            m = 12
            im = Image.new('RGBA', ((bb[2] - bb[0]) + 2 * m, (bb[3] - bb[1]) + 2 * m), (0, 0, 0, 0))
            ImageDraw.Draw(im).text((m - bb[0], m - bb[1]), t, font=f, fill=(255, 255, 255, 255),
                                    stroke_width=2, stroke_fill=(0, 0, 0, 255))
            ab = im.split()[3].getbbox()
            L.append('    %-4s size=%-3d canvas=%s  alpha_bbox=%s  -> %dx%d'
                     % (t, s, im.size, ab, ab[2] - ab[0], ab[3] - ab[1]))
    L.append('')

out = '\n'.join(L)
with open(os.path.join(HERE, 'font_diag.txt'), 'w', encoding='utf-8') as f:
    f.write(out)
print(out)
