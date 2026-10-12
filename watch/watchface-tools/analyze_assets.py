#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
表盘资源自检器
==============
make_assets.py 跑完后跑这个，回答三个问题：

  1) 每个贴图有没有被裁掉？ —— 描边/投影贴到画布边缘就是被裁了，会看到缺角。
  2) 时间行/日期行拼起来多宽？会不会超出 390？
  3) 等宽单元的居中是否一致？ —— 数字在各自单元里必须水平居中，否则换秒会左右抖。

输出写到 asset_metrics.txt。
"""
import os
import re
import sys
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import make_assets as MA      # 复用它的「日期行元素顺序」，两边不再各写一份
PROJ = os.path.dirname(HERE)
EZIP = os.path.join(PROJ, 'lvgl', 'watch', 'src', 'resource', 'images', 'common', 'ezip')
LARGE = os.path.join(PROJ, 'lvgl', 'watch', 'src', 'resource', 'images', 'common', 'large_ezip')
LAYOUT = os.path.join(PROJ, 'lvgl', 'watch', 'src', 'gui_apps', 'clock', 'wf_layout.h')

SCR_W = 390

names = ['wf_d%d' % i for i in range(10)] + ['wf_colon'] \
        + ['wf_s%d' % i for i in range(10)] \
        + ['wf_ym', 'wf_yd'] + ['wf_w%d' % i for i in range(1, 8)]

L = []
info = {}
bad = 0

L.append('===== 1) 裁切检查 (padL/padR/padT/padB = 0 表示贴到边缘，有裁切风险) =====')
for n in names:
    im = Image.open(os.path.join(EZIP, n + '.png')).convert('RGBA')
    W, H = im.size
    bb = im.split()[3].getbbox()
    info[n] = (W, H, bb)
    pl, pt = bb[0], bb[1]
    pr, pb = W - bb[2], H - bb[3]
    flag = ''
    if min(pl, pr, pt, pb) <= 0:
        flag = '   <== 被裁切!'
        bad += 1
    L.append('%-10s %3dx%-3d  ink=(%3d,%3d)-(%3d,%3d)  pad L%2d R%2d T%2d B%2d%s'
             % (n, W, H, bb[0], bb[1], bb[2], bb[3], pl, pr, pt, pb, flag))
L.append('裁切问题: %d 个' % bad)

for n, d in (('wf_bg', LARGE), ('wf_scrim', LARGE)):
    im = Image.open(os.path.join(d, n + '.png'))
    L.append('%-10s %3dx%-3d mode=%s' % (n, im.size[0], im.size[1], im.mode))

# 读 wf_layout.h 里的常量
lay = {}
if os.path.isfile(LAYOUT):
    for line in open(LAYOUT, encoding='utf-8'):
        m = re.match(r'#define\s+(WF_\w+)\s+(\d+)', line)
        if m:
            lay[m.group(1)] = int(m.group(2))

L.append('')
L.append('===== 2) 时间行 / 日期行 宽度 =====')
dw, cw = info['wf_d0'][0], info['wf_colon'][0]
row = 4 * dw + cw
x0 = (SCR_W - row) // 2
L.append('时间行 = 4*%d + %d = %d   左起 x=%d  右留白=%d   %s'
         % (dw, cw, row, x0, SCR_W - x0 - row, 'OK' if row <= SCR_W - 20 else '超宽!'))

L.append('日期行 = 各贴图宽之和 + 元素间隔%d x (n-1) + 星期间隔%d + 周X宽'
         % (MA.DATE_ITEM_GAP, MA.WEEK_SEP))
for mon, day in ((1, 2), (9, 20), (10, 8), (12, 30)):
    toks = MA.mon_day_tokens(mon, day)
    w = (sum(info[t[1]][0] for t in toks) + MA.DATE_ITEM_GAP * (len(toks) - 1)
         + MA.WEEK_SEP + info['wf_w7'][0])
    L.append('  %2d月%-2d日 周日  %d 个元素  宽 %3d  两侧留白 %d  %s'
             % (mon, day, len(toks), w, (SCR_W - w) // 2,
                'OK' if w <= SCR_W - 20 else '超宽!'))

# 每个数字在自己的等宽单元里是否居中
L.append('')
L.append('===== 3) 等宽单元居中一致性 (数字墨迹中心 相对 单元中心 的偏移) =====')
L.append('大号数字单元宽 = %d  中心 = %.1f' % (dw, dw / 2))
for i in range(10):
    W, H, bb = info['wf_d%d' % i]
    c = (bb[0] + bb[2]) / 2
    L.append('  d%d  墨迹中心 %6.1f   偏移 %+5.1f px' % (i, c, c - W / 2))
L.append('小号数字单元宽 = %d' % info['wf_s0'][0])
for i in range(10):
    W, H, bb = info['wf_s%d' % i]
    c = (bb[0] + bb[2]) / 2
    L.append('  s%d  墨迹中心 %6.1f   偏移 %+5.1f px' % (i, c, c - W / 2))

L.append('')
L.append('===== 4) wf_layout.h 关键常量 =====')
for k in sorted(lay):
    L.append('  %-18s %d' % (k, lay[k]))
ink_h = max(info['wf_d%d' % i][2][3] - info['wf_d%d' % i][2][1] for i in range(10))
L.append('')
L.append('视觉占比: 数字墨迹高 %d px / 屏高 450 = %.0f%%   数字占宽 %d / 390 = %.0f%%'
         % (ink_h, 100.0 * ink_h / 450, row, 100.0 * row / 390))

L.append('')
L.append('===== 5) 可读性检查 (白字压在背景上，够不够看清) =====')


def rel_lum(rgb):
    def ch(c):
        c /= 255.0
        return c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4
    r, g, b = (ch(v) for v in rgb)
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def mean_rgb(im, box):
    """把区域缩小到 1x1 取均值，避开逐像素循环"""
    return im.crop(box).convert('RGB').resize((1, 1), Image.BOX).getpixel((0, 0))


try:
    bg = Image.open(os.path.join(LARGE, 'wf_bg.png')).convert('RGB')
    sc = Image.open(os.path.join(LARGE, 'wf_scrim.png')).convert('RGBA')
    comp = bg.convert('RGBA')
    comp.alpha_composite(sc)
    comp = comp.convert('RGB')

    bands = [('状态栏/上方', (0, 30, 390, 130)),
             ('时间行', (int(lay.get('WF_TIME_X0', 11)), lay.get('WF_TIME_Y', 140),
                         int(lay.get('WF_TIME_X0', 11)) + 4 * lay.get('WF_DIGIT_W', 82)
                         + lay.get('WF_COLON_W', 39),
                         lay.get('WF_TIME_Y', 140) + lay.get('WF_DIGIT_H', 105))),
             ('日期行', (40, lay.get('WF_DATE_Y', 259), 350,
                         lay.get('WF_DATE_Y', 259) + lay.get('WF_DATE_H', 37))),
             ('下方空白', (0, 310, 390, 450))]
    L.append('  WCAG 对比度 (前景 = 白色 #FFFFFF，>=4.5 为正文合格，>=3.0 大字合格)')
    for name, box in bands:
        a = mean_rgb(bg, box)
        b = mean_rgb(comp, box)
        cr_a = (1.0 + 0.05) / (rel_lum(a) + 0.05)
        cr_b = (1.0 + 0.05) / (rel_lum(b) + 0.05)
        L.append('  %-10s 背景均值 %-16s 对比度 %.2f  ->  加压暗层后 %-16s 对比度 %.2f  %s'
                 % (name, str(a), cr_a, str(b), cr_b,
                    'OK' if cr_b >= 3.0 else '偏亮! 需要更强的压暗层'))
except Exception as e:
    L.append('  检查失败: %r' % (e,))

L.append('')
L.append('===== 6) 日期行元素顺序自检 (独立解析「<月>月<日>日」再比对) =====')
try:
    L.append('  [OK] 数字在前、单位在后 —— %s'
             % MA.check_date_order([(1, 2), (9, 20), (10, 8), (11, 9), (12, 30), (3, 31)]))
except SystemExit as e:
    L.append('  [FAIL] %s' % e)

def scan_c(src, lay, day_gap):
    """扫 C 源码，返回 [(描述, ok), ...]。抽成函数是为了能用故意写错的源码自测。"""
    lines = src.splitlines()

    def cline(pat):
        rx = re.compile(pat)
        for i, ln in enumerate(lines):
            if rx.search(ln):
                return i
        return -1

    i_mon = cline(r'small_digit\[mon % 10\]')
    i_ym = cline(r'LV_EXT_IMG_GET\(wf_ym\)')
    i_day = cline(r'small_digit\[day % 10\]')
    i_yd = cline(r'LV_EXT_IMG_GET\(wf_yd\)')
    return [
        ('冒号真的设了图源 (lv_img_set_src/band_place 里出现 colon)',
         bool(re.search(r'(lv_img_set_src|band_place)\s*\([^,]*colon\s*,', src))),
        ('月份个位排进去了  small_digit[mon % 10]', i_mon >= 0),
        ('日期个位排进去了  small_digit[day % 10]', i_day >= 0),
        ('「月」排在月份数字之后', i_ym > i_mon >= 0),
        ('「日」排在日期数字之后', i_yd > i_day >= 0),
        ('wf_layout.h 的 WF_DAY_GAP 与生成器 DATE_ITEM_GAP 一致',
         lay.get('WF_DAY_GAP') == day_gap),
    ]


# 上一版真烧到板子上的写法：月份个位漏排 + 「月」写在数字前面 + 冒号没设图源。
# 用它来证明检查器不是摆设。
BAD_SRC = '''
    if (mon >= 10)
    {
        seq[n] = LV_EXT_IMG_GET(wf_ym);
        n++;
    }
    if (day >= 10)
    {
        seq[n] = small_digit[day / 10];
        n++;
    }
    seq[n] = small_digit[day % 10];
    n++;
    seq[n] = LV_EXT_IMG_GET(wf_yd);
    n++;
'''

L.append('')
L.append('===== 7) C 侧布局检查 (app_clock_band.c) =====')
CPP = os.path.join(PROJ, 'lvgl', 'watch', 'src', 'gui_apps', 'clock', 'app_clock_band.c')
cken = scan_c(open(CPP, encoding='utf-8').read(), lay, MA.DATE_ITEM_GAP)
for name, ok in cken:
    L.append('  [%s] %s' % ('OK' if ok else 'FAIL', name))
if not all(ok for _, ok in cken):
    L.append('  ^ 出现 FAIL：这类顺序/漏设图源的错误在 PC 预览上看不出来，上板才发现，')
    L.append('    必须修干净再编译烧录。')

neg = [n for n, ok in scan_c(BAD_SRC, lay, MA.DATE_ITEM_GAP) if not ok]
must = [n for n in neg if ('月份个位' in n) or ('「月」排在' in n) or ('冒号' in n)]
L.append('  负向自测: 喂进上一版写错的代码 -> 抓出 %d 项 FAIL' % len(neg))
L.append('    %s' % ('；'.join(neg) if neg else '<== 检查器失效，检查项是摆设!'))
L.append('    关键项是否被抓住: %s' % ('是' if len(must) >= 3 else '否 <== 检查器不够严!'))


# ── 8) 校时面板字段标签的顺序 ───────────────────────────────────────────────
# 「年份写在汉字前面」这个错在表盘日期行上犯过一次（写成了「月920日」），
# 2026-09-20 做校时面板时又犯了一次（「年 2024」）。同一个错犯两次说明它
# 靠记性管不住，必须落到检查里。
def scan_cal(src):
    """校时面板标签必须是「数字在前、单位在后」：2024 年 / 09 月。"""
    bad = re.search(r'"%s\s+%0?2?d"', src)         # 「年 2024」的旧写法
    good_y = re.search(r'"%d\s+%s"', src)
    good_2 = re.search(r'"%02d\s+%s"', src)
    args = re.search(r'cal_val\[i\],\s*cal_field_name\[i\]', src)
    return [
        ('校时面板: 没有「单位在前」的旧格式 "%s %d" / "%s %02d"', bad is None),
        ('校时面板: 年份用 "%d %s"', bool(good_y)),
        ('校时面板: 其余字段用 "%02d %s"', bool(good_2)),
        ('校时面板: 实参顺序 cal_val 在 cal_field_name 之前', bool(args)),
    ]


BAD_CAL_SRC = '''
        rt_snprintf(buf, sizeof(buf), "%s %02d", cal_field_name[i], b->cal_val[i]);
'''

L.append('')
L.append('===== 8) 校时面板字段顺序检查 (数字在前、单位在后) =====')
calsrc = open(CPP, encoding='utf-8').read()
calen = scan_cal(calsrc)
for name, ok in calen:
    L.append('  [%s] %s' % ('OK' if ok else 'FAIL', name))
if not all(ok for _, ok in calen):
    L.append('  ^ 出现 FAIL：面板上会显示成「年 2024」，语序和表盘的「9月20日」不一致。')

calneg = [n for n, ok in scan_cal(BAD_CAL_SRC) if not ok]
L.append('  负向自测: 喂进「年 2024」的旧写法 -> 抓出 %d 项 FAIL' % len(calneg))
L.append('    %s' % ('；'.join(calneg) if calneg else '<== 检查器失效，检查项是摆设!'))

out = '\n'.join(L)
with open(os.path.join(HERE, 'asset_metrics.txt'), 'w', encoding='utf-8') as f:
    f.write(out)
print(out)
