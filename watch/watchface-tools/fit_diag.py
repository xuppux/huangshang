#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""诊断 fit_small / bake 的实际返回"""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import make_assets as M

cjk = M.pick(M.CJK_CANDS)
lat = M.pick(M.LATIN_CANDS)

L = []
L.append('SMALL = %r' % (M.SMALL,))
L.append('BIG   = %r' % (M.BIG,))
L.append('SMALL_TARGET_H = %s' % M.SMALL_TARGET_H)
L.append('')

L.append('--- bake("月", cjk, s, SMALL).size  (s=8..30) ---')
for s in range(8, 31):
    l = M.bake('月', cjk, s, M.SMALL)
    L.append('  s=%-3d %s' % (s, l.size))

L.append('')
L.append('--- bake("0", lat, s, SMALL).size ---')
for s in range(8, 31):
    l = M.bake('0', lat, s, M.SMALL)
    L.append('  s=%-3d %s' % (s, l.size))

L.append('')
L.append('--- bake("8", lat, s, BIG).size  (大号) ---')
for s in (60, 120, 125, 130):
    L.append('  s=%-3d %s' % (s, M.bake('8', lat, s, M.BIG).size))
    L.append('  s=%-3d 4 -> %s' % (s, M.bake('4', lat, s, M.BIG).size))

L.append('')
L.append('fit_small(lat, "0", %d) = %s' % (M.SMALL_TARGET_H, M.fit_small(lat, '0', M.SMALL_TARGET_H)))
L.append('fit_small(cjk, "月", %d) = %s' % (M.SMALL_TARGET_H, M.fit_small(cjk, '月', M.SMALL_TARGET_H)))

# bake 内部到底裁没裁
from PIL import Image, ImageDraw, ImageFilter, ImageFont
f = ImageFont.truetype(cjk, 24)
bb = f.getbbox('月')
st = M.SMALL
m = st['outline'] + st['blur'] + st['dy'] + 6
W, H = (bb[2]-bb[0]) + 2*m, (bb[3]-bb[1]) + 2*m
cv = Image.new('RGBA', (W, H), (0,0,0,0))
ox, oy = m - bb[0], m - bb[1]
sh = Image.new('RGBA', cv.size, (0,0,0,0))
ImageDraw.Draw(sh).text((ox, oy), '月', font=f, fill=st['shadow'], stroke_width=st['outline'], stroke_fill=st['shadow'])
cv.alpha_composite(sh.filter(ImageFilter.GaussianBlur(st['blur'])), (0, st['dy']))
ImageDraw.Draw(cv).text((ox, oy), '月', font=f, fill=st['fg'], stroke_width=st['outline'], stroke_fill=st['stroke'])
ab = cv.split()[3].getbbox()
L.append('')
L.append('手算 size=24: bbox=%s m=%d canvas=%s ox=%d oy=%d alpha_bbox=%s' % (bb, m, cv.size, ox, oy, ab))

out = '\n'.join(L)
open(os.path.join(HERE, 'fit_diag.txt'), 'w', encoding='utf-8').write(out)
print(out)
