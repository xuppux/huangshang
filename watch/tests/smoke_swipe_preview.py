# -*- coding: utf-8 -*-
"""
smoke_swipe_preview.py -- Python port of tests/smoke-swipe-preview.ps1

Why a Python twin exists
------------------------
On this machine the PowerShell tool swallows stdout and mangles redirected
output (native command output re-encoded, ANSI colour escapes left in), which
made the .ps1 version impossible to *read* even when it passed. Python's I/O is
byte-exact here, so the same assertions live in a script whose result we can
actually see.

Same two hard-won rules as the .ps1:
  1. Assert on ASCII key=value markers the page writes into document.title --
     never on rendered text (ANSI/UTF-8 encoding traps).
  2. Never wait a fixed time for the animation; poll to a generous budget.

Usage:
    <python> tests/smoke_swipe_preview.py
Exit code 0 = all checks passed.
"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PREVIEW = os.path.join(ROOT, "ui-preview")
SRC = os.path.join(PREVIEW, "swipe-back.html")
DST = os.path.join(PREVIEW, "__smoke.html")

BROWSERS = [
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
]

HARNESS = r"""
<script>
(function(){
  var log = [];
  function rec(s){ log.push(s); }
  function finish(){ document.title = "SMOKE|" + log.join(" | "); }
  function dbg(){ return window.__dbg || {phase:"none",dx:-1,armed:false,mid:-1}; }
  function cls(){
    var v = document.getElementById("verdict");
    return v ? v.className.replace(/\s+/g, "_") : "NULL";
  }
  function tx(){
    var a = document.getElementById("scr");
    if (!a) return "NaN";
    var m = /translateX\((-?[\d.]+)px\)/.exec(a.style.transform || "");
    return m ? m[1] : "0";
  }
  function indCls(){
    var i = document.getElementById("ind");
    return i ? i.className.replace(/\s+/g, "_") : "NULL";
  }
  function waitFor(cond, done, ticks){
    var i = 0;
    (function poll(){
      if (cond())            { done(true);  return; }
      if (i++ >= ticks)      { done(false); return; }
      setTimeout(poll, 20);
    })();
  }

  try {
    var phone = document.getElementById("phone");
    if (!phone) { rec("phone=NULL"); finish(); return; }

    var r  = phone.getBoundingClientRect();
    var Y  = r.top + r.height / 2;
    var X0 = r.right - 8;

    function ev(t, x){ return new MouseEvent(t, {clientX:x, clientY:Y, bubbles:true, cancelable:true}); }

    function drag(dx){
      phone.dispatchEvent(ev("mousedown", X0));
      for (var i = 1; i <= 4; i++) phone.dispatchEvent(ev("mousemove", X0 - dx * i / 4));
      var d = dbg();
      return { mid: d.mid, armed: d.armed, tx: tx(), ind: indCls(), phase: d.phase };
    }
    function up(dx){ window.dispatchEvent(ev("mouseup", X0 - dx)); }

    rec("rect=" + Math.round(r.width) + "x" + Math.round(r.height));

    var a = drag(220);
    rec("A_mid=" + a.mid + " A_armed=" + (a.armed ? 1 : 0) + " A_tx=" + a.tx + " A_ind=" + a.ind);
    up(220);
    rec("A_cls=" + cls());
    rec("A_phase=" + dbg().phase);

    waitFor(function(){ return dbg().phase === "idle"; }, function(ok){
      rec("A_reset=" + (ok ? "yes" : "timeout") + " A_end_tx=" + tx());

      var b = drag(40);
      rec("B_ind=" + b.ind);
      up(40);
      rec("B_cls=" + cls());
      rec("B_phase=" + dbg().phase + " B_tx=" + tx());

      var c = drag(2);
      rec("C_ind=" + c.ind);
      up(2);
      rec("C_cls=" + cls() + " C_dx=" + dbg().dx);
      rec("DONE");
      finish();
    }, 600);

  } catch (e) {
    document.title = "SMOKE-ERR|" + e.message + "|" + log.join(" | ");
  }
})();
</script>
"""

CHECKS = [
    ("harness did not throw",              lambda t, l: not t.startswith("SMOKE-ERR")),
    ("phone element found",                lambda t, l: "rect=390x450" in l),
    ("all cases ran",                      lambda t, l: re.search(r"\bDONE\b", l) is not None),
    ("A drag tracked = 220px",             lambda t, l: re.search(r"A_mid=220\b", l) is not None),
    ("A armed past the 130px line",        lambda t, l: re.search(r"A_armed=1\b", l) is not None),
    ("A indicator armed",                  lambda t, l: re.search(r"A_ind=ind_on_armed\b", l) is not None),
    ("A page did NOT move while dragging", lambda t, l: re.search(r"A_tx=-?0(\.0+)?\b", l) is not None),
    ("A verdict = go back",                lambda t, l: re.search(r"A_cls=verdict_go\b", l) is not None),
    ("A played the return animation",      lambda t, l: re.search(r"A_phase=anim\b", l) is not None),
    ("A animation completed + reset",      lambda t, l: re.search(r"A_reset=yes\b", l) is not None),
    ("A back at rest",                     lambda t, l: re.search(r"A_end_tx=-?0(\.0+)?\b", l) is not None),
    ("B indicator shown (over 8px)",       lambda t, l: re.search(r"B_ind=ind_on\b", l) is not None),
    ("B verdict = not enough",             lambda t, l: re.search(r"B_cls=verdict_back\b", l) is not None),
    ("B did NOT animate",                  lambda t, l: re.search(r"B_phase=idle\b", l) is not None),
    ("B page still at rest",               lambda t, l: re.search(r"B_tx=-?0(\.0+)?\b", l) is not None),
    ("C indicator stayed hidden",          lambda t, l: re.search(r"C_ind=ind\b", l) is not None),
    ("C verdict untouched",                lambda t, l: re.search(r"C_cls=verdict\b", l) is not None),
    ("C distance recorded = 2px",          lambda t, l: re.search(r"C_dx=2\b", l) is not None),
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
    profile = tempfile.mkdtemp(prefix="smk_")
    dom = os.path.join(tempfile.gettempdir(), "smk_dom_%d.html" % os.getpid())
    url = "file:///" + PREVIEW.replace("\\", "/") + "/__smoke.html"
    args = [
        browser,
        "--headless=new", "--disable-gpu", "--no-sandbox", "--no-first-run",
        "--log-level=3", "--disable-logging", "--window-size=1440,1500",
        "--user-data-dir=" + profile,
        "--virtual-time-budget=30000",
        "--dump-dom", url,
    ]
    with open(dom, "wb") as out:
        subprocess.run(args, stdout=out, stderr=subprocess.DEVNULL, timeout=180)
    with open(dom, "rb") as f:
        raw = f.read().decode("utf-8", "replace")
    # cleanup
    try:
        import shutil
        shutil.rmtree(profile, ignore_errors=True)
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
