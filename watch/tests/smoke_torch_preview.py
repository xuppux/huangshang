# -*- coding: utf-8 -*-
"""
smoke_torch_preview.py -- render + interaction smoke test for
                          ui-preview/torch.html

为什么用 Python 而不是 .ps1：本机 PowerShell 工具会吞 stdout，重定向出来的文件
还会被重新编码并留下 ANSI 色码，导致"测试明明通过却读不出来"。
Python 的 I/O 在这台机器上是字节精确的。同样的坑在 smoke_swipe_preview.py 里记过。

页面是被真实鼠标事件驱动的：点模拟屏切 LED、点分段控件切"改前/改后"。
断言只看 window.__dbg 里的 ASCII 数字（不看渲染出来的中文），
避免编码问题把测试搞成假错。

用法：
    <python> tests/smoke_torch_preview.py
退出码 0 = 全部通过。
"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PREVIEW = os.path.join(ROOT, "ui-preview")
SRC = os.path.join(PREVIEW, "torch.html")
DST = os.path.join(PREVIEW, "__smoke_torch.html")

BROWSERS = [
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
]

# 只写 ASCII key=value 到 document.title
HARNESS = r"""
<script>
(function(){
  var log = [];
  function rec(s){ log.push(s); }
  function done(){ document.title = "SMOKE|" + log.join(" | "); }
  function d(){ return window.__dbg || {}; }
  function phoneBox(){
    var p = document.getElementById("phone");
    if (!p) return null;
    return p.getBoundingClientRect();
  }
  function click(el){
    if (!el) return;
    var r = el.getBoundingClientRect();
    el.dispatchEvent(new MouseEvent("click", {
      clientX: r.left + r.width/2, clientY: r.top + r.height/2,
      bubbles: true, cancelable: true
    }));
  }
  function txt(id){ var e = document.getElementById(id); return e ? e.textContent : "NULL"; }
  function has(id, cls){ var e = document.getElementById(id); return (e && e.classList.contains(cls)) ? 1 : 0; }
  /* ⚠ 不要断言"动画结束后的 opacity"。
     headless + --virtual-time-budget 下没有真实的合成/渲染帧，CSS transition
     的进度不会推进：点完等 1.2 秒再读 getComputedStyle，拿到的还是起始值
     （实测 D_opa 一直是 1）。那是在测浏览器的动画时钟，不是测页面。
     要断言就断言**不参与动画的那些声明** —— 这里用 pointer-events:
     .cback.gone{opacity:0;pointer-events:none} 里只有 opacity 是动画的。 */
  function inert(id){
    var e = document.getElementById(id);
    return (e && getComputedStyle(e).pointerEvents === "none") ? 1 : 0;
  }

  try {
    var phone = document.getElementById("phone");
    if (!phone) { rec("phone=NULL"); done(); return; }

    var r = phoneBox();
    rec("rect=" + Math.round(r.width) + "x" + Math.round(r.height));

    /* ---- A: 初始 ---- */
    rec("A_mode=" + d().mode + " A_back=" + d().back + " A_exprW=" + d().exprW + " A_rfLeft=" + d().rfLeft);
    rec("A_keys=" + document.querySelectorAll("#cpad .ckey").length);
    rec("A_led=" + d().torchMode + " A_glow=" + has("ledglow", "on") + " A_flash=" + has("ledglow", "flash"));

    /* ---- B/C/D: 轻触走完一圈 关 -> 常亮 -> 频闪 -> 关 ----
       ⚠ 断言只看 window.__dbg 的 ASCII 值 + class 名，不看渲染出来的中文：
         headless 下 CSS 动画不推进，断言"动画后的 computed style"必假错。 */
    click(phone);
    rec("B_led=" + d().torchMode + " B_glow=" + has("ledglow", "on") + " B_flash=" + has("ledglow", "flash"));
    rec("B_badge_on=" + (txt("badge").indexOf("常亮") >= 0 ? 1 : 0));
    click(phone);
    rec("C_led=" + d().torchMode + " C_glow=" + has("ledglow", "on") + " C_flash=" + has("ledglow", "flash"));
    click(phone);
    rec("D_led=" + d().torchMode + " D_glow=" + has("ledglow", "on"));

    /* ---- E: 切到"改之后" ---- */
    var btns = document.querySelectorAll("#seg button");
    rec("E_nbtn=" + btns.length);
    click(btns[1]);
    rec("E_mode=" + d().mode + " E_back=" + d().back + " E_exprW=" + d().exprW + " E_rfLeft=" + d().rfLeft);
    rec("E_cls=" + has("cback", "gone") + " E_inert=" + inert("cback"));

    /* ---- F: 切回"改之前" ---- */
    click(btns[0]);
    rec("F_mode=" + d().mode + " F_back=" + d().back + " F_exprW=" + d().exprW);
    rec("F_cls=" + has("cback", "gone") + " F_live=" + (1 - inert("cback")));

    rec("DONE");
    done();

  } catch (e) {
    document.title = "SMOKE-ERR|" + e.message + "|" + log.join(" | ");
  }
})();
</script>
"""

CHECKS = [
    ("harness did not throw",            lambda t, l: not t.startswith("SMOKE-ERR")),
    ("watch screen is 390x450",          lambda t, l: "rect=390x450" in l),
    ("all cases ran",                    lambda t, l: re.search(r"\bDONE\b", l) is not None),
    ("starts in 'before' mode",          lambda t, l: re.search(r"A_mode=before\b", l) is not None),
    ("before: back button present",      lambda t, l: re.search(r"A_back=1\b", l) is not None),
    ("before: expr width = 254",         lambda t, l: re.search(r"A_exprW=254\b", l) is not None),
    ("before: rule at x=108",            lambda t, l: re.search(r"A_rfLeft=108\b", l) is not None),
    ("keypad has 22 keys",               lambda t, l: re.search(r"A_keys=22\b", l) is not None),
    ("led starts off",                   lambda t, l: re.search(r"A_led=off A_glow=0 A_flash=0\b", l) is not None),
    ("tap 1 -> steady on",               lambda t, l: re.search(r"B_led=on B_glow=1 B_flash=0\b", l) is not None),
    ("badge says chang-liang when on",   lambda t, l: re.search(r"B_badge_on=1\b", l) is not None),
    ("tap 2 -> strobe",                  lambda t, l: re.search(r"C_led=strobe C_glow=1 C_flash=1\b", l) is not None),
    ("tap 3 -> cycles back off",         lambda t, l: re.search(r"D_led=off D_glow=0\b", l) is not None),
    ("segment has 2 buttons",            lambda t, l: re.search(r"E_nbtn=2\b", l) is not None),
    ("after: back button class set",     lambda t, l: re.search(r"E_cls=1\b", l) is not None),
    ("after: back button inert",         lambda t, l: re.search(r"E_inert=1\b", l) is not None),
    ("after: expr width = 334",          lambda t, l: re.search(r"E_exprW=334\b", l) is not None),
    ("after: rule at x=28",              lambda t, l: re.search(r"E_rfLeft=28\b", l) is not None),
    ("switch back to 'before'",          lambda t, l: re.search(r"F_mode=before\b", l) is not None),
    ("before: back button live again",   lambda t, l: re.search(r"F_cls=0 F_live=1\b", l) is not None),
]


def find_browser():
    for b in BROWSERS:
        if os.path.exists(b):
            return b
    return None


def build_page():
    with open(SRC, "rb") as f:
        html = f.read().decode("utf-8")
    if "</body>" not in html:
        raise SystemExit("[ERROR] no </body> in %s" % SRC)
    html = re.sub(r"(?s)</body>\s*</html>\s*$", "", html)
    with open(DST, "wb") as f:
        f.write((html + "\n" + HARNESS + "\n</body></html>\n").encode("utf-8"))


def run_headless(browser):
    profile = tempfile.mkdtemp(prefix="tk_")
    dom = os.path.join(tempfile.gettempdir(), "tk_dom_%d.html" % os.getpid())
    url = "file:///" + PREVIEW.replace("\\", "/") + "/__smoke_torch.html"
    args = [
        browser,
        "--headless=new", "--disable-gpu", "--no-sandbox", "--no-first-run",
        "--log-level=3", "--disable-logging", "--window-size=1440,2400",
        "--user-data-dir=" + profile,
        "--virtual-time-budget=30000",
        "--dump-dom", url,
    ]
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
    return raw


def main():
    if not os.path.exists(SRC):
        print("[ERROR] missing %s" % SRC)
        return 2
    browser = find_browser()
    if not browser:
        print("[SKIP] no Edge/Chrome found")
        return 0

    build_page()
    raw = run_headless(browser)
    m = re.search(r"<title>([\s\S]*?)</title>", raw)
    title = m.group(1) if m else ""
    log = title[6:] if title.startswith("SMOKE|") else ""

    print("harness: " + title)
    print("")

    failures = 0
    for name, fn in CHECKS:
        try:
            ok = fn(title, log)
        except Exception:
            ok = False
        print(("  PASS  " if ok else "  FAIL  ") + name)
        if not ok:
            failures += 1

    print("")
    if failures == 0:
        print("ALL PASS  (%d checks)" % len(CHECKS))
        return 0
    print("FAILED: %d of %d" % (failures, len(CHECKS)))
    print("raw: " + title)
    return 1


if __name__ == "__main__":
    sys.exit(main())
