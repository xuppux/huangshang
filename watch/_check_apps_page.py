# -*- coding: utf-8 -*-
"""
_check_apps_page.py -- 冒烟检查 能做什么应用.html

为什么需要：这份清单的 60 条内容是 **JS 渲染**的（GROUPS 数组 → #groups）。
我手工改过 GROUPS 里的字符串（往 gate 字段里塞了 \\n 和 \\" 转义），
一旦转义写坏，整个 <script> 解析失败，页面就只剩下标题和空白 ——
而浏览器不会报错给你看。这个脚本用无头浏览器 dump-dom，
直接看"渲染出来的 DOM 里有没有那 60 条"。

判据（必须全部成立）：
  * 渲染出的 .app 条目数 == 60
  * 10 个分组标题都在
  * #listCnt 不是空的
  * #stats 里出现了数字（统计条渲染成功）
用法：<python> _check_apps_page.py
"""
import os
import re
import subprocess
import sys
import tempfile

PAGE = r"C:\Users\18058\Desktop\huangshanpai\能做什么应用.html"
BROWSERS = [
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
]

# 10 个分组标题（与源文件 GROUPS[].name 一致）
GROUPS = ["传感器与体感", "电源与硬件状态", "时间与工具",
          "声音（喇叭要自己接一个）", "灯光",
          "游戏（屏幕、触摸、IMU 全都在）", "蓝牙与连接",
          "存储与数据（要插一张 TF 卡）", "系统",
          "板子没有的"]


def find_browser():
    for b in BROWSERS:
        if os.path.exists(b):
            return b
    return None


def main():
    if not os.path.exists(PAGE):
        print("[ERROR] missing %s" % PAGE)
        return 2
    br = find_browser()
    if not br:
        print("[SKIP] no Edge/Chrome")
        return 0

    profile = tempfile.mkdtemp(prefix="apps_")
    dom = os.path.join(tempfile.gettempdir(), "apps_dom_%d.html" % os.getpid())
    url = "file:///" + PAGE.replace("\\", "/")
    args = [br, "--headless=new", "--disable-gpu", "--no-sandbox", "--no-first-run",
            "--log-level=3", "--disable-logging", "--window-size=1400,2000",
            "--user-data-dir=" + profile, "--virtual-time-budget=20000",
            "--dump-dom", url]
    with open(dom, "wb") as out:
        subprocess.run(args, stdout=out, stderr=subprocess.DEVNULL, timeout=180)
    with open(dom, "rb") as f:
        raw = f.read().decode("utf-8", "replace")
    import shutil
    shutil.rmtree(profile, ignore_errors=True)
    try:
        os.remove(dom)
    except OSError:
        pass

    fails = []

    # ⚠ dump 出来的 DOM 里**也包含 <script> 源码本身**，而模板字符串里就有
    #   `'" data-id="'` 和 `'<div class="gate">'` 各一处 —— 直接全文档数会多算 1。
    #   先把 <script>…</script> 整段剥掉，只数真正渲染出来的 DOM。
    body = re.sub(r"(?s)<script\b.*?</script>", "", raw)

    # 1) 条目数。每张 app 卡片渲染成 <div class="card[ done]" data-id="名字">
    n_entries = len(re.findall(r'data-id="', body))
    print("  entries rendered = %d  (expect 60)" % n_entries)
    if n_entries != 60:
        fails.append("entry count")

    # 1b) gate 区块也要真的渲染出来（我改的就是这个字段）
    n_gate = len(re.findall(r'class="gate"', body))
    print("  gate blocks      = %d  (expect 60)" % n_gate)
    if n_gate != 60:
        fails.append("gate blocks")

    # 2) 分组标题
    missing = [g for g in GROUPS if g not in raw]
    print("  groups found     = %d/%d" % (len(GROUPS) - len(missing), len(GROUPS)))
    if missing:
        fails.append("missing groups: %s" % ", ".join(missing))

    # 3) 计数与统计条
    m = re.search(r'id="listCnt"[^>]*>([^<]*)<', raw)
    cnt = (m.group(1).strip() if m else "")
    print("  listCnt text     = %r" % cnt)
    if not re.search(r"\d", cnt):
        fails.append("listCnt empty")

    m2 = re.search(r'id="stats"[^>]*>([\s\S]{0,400}?)</div>', raw)
    stats = m2.group(1) if m2 else ""
    print("  stats has digit  = %s" % bool(re.search(r"\d", stats)))
    if not re.search(r"\d", stats):
        fails.append("stats empty")

    # 4) 我刚改的那条 gate 有没有真的**渲染**出来（用 body，不用 raw ——
    #    否则匹配到 <script> 里的源码也算"通过"，等于没验）
    if "BSP_PWM3_UPDATE_USING_DMA" not in body:
        fails.append("new gate text not rendered")
        print("  PWM3 gate        = MISSING")
    else:
        print("  PWM3 gate        = present")

    print("")
    if fails:
        print("FAILED: " + "; ".join(fails))
        return 1
    print("ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
