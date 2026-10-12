<#
  smoke-swipe-preview.ps1 -- render + interaction smoke test for
                             ui-preview/swipe-back.html

  Why this exists
  ---------------
  The preview page is hand-written HTML/JS, and twice now one broken character
  silently killed the whole <script> block: the page still *looked* fine (the
  styles and markup are independent of the script), but nothing reacted to the
  mouse. Static reading did not catch it; driving the page did.

  So this loads the page in headless Edge, dispatches real mouse events at the
  right-edge hit band, and asserts on ASCII key=value markers the page writes
  into document.title. Exit code 0 = everything passed.

  Two hard-won rules baked in here
  --------------------------------
  1. PURE ASCII ONLY. Windows PowerShell 5.1 reads a .ps1 as ANSI unless it has
     a UTF-8 BOM, so Chinese text in this file turns into parse errors. Every
     assertion therefore keys off ASCII markers, never off rendered text.
  2. NEVER wait a fixed number of milliseconds for an animation. Headless
     Chromium throttles requestAnimationFrame unpredictably, so the page's
     settle animation lands at a random frame count. Poll for the end state
     with a generous budget instead. (The page drives its animation with
     setTimeout, not rAF, exactly so that this stays deterministic.)

  Usage:
      .\smoke-swipe-preview.ps1
#>
$ErrorActionPreference = "Stop"

$dir = Split-Path $PSScriptRoot -Parent            # ...\watch
$dir = Join-Path $dir "ui-preview"
$src = Join-Path $dir "swipe-back.html"
$dst = Join-Path $dir "__smoke.html"

if (-not (Test-Path $src)) { Write-Host "[ERROR] missing $src" -ForegroundColor Red; exit 2 }

# ---- locate a headless browser ---------------------------------------------
$edge = $null
foreach ($c in @(
    "C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    "C:\Program Files\Microsoft\Edge\Application\msedge.exe",
    "C:\Program Files\Google\Chrome\Application\chrome.exe",
    "C:\Program Files (x86)\Google\Chrome\Application\chrome.exe")) {
    if (Test-Path $c) { $edge = $c; break }
}
if (-not $edge) { Write-Host "[SKIP] no Edge/Chrome found" -ForegroundColor Yellow; exit 0 }

# ---- injected harness -------------------------------------------------------
# Emits ASCII key=value markers only. All state is read from window.__dbg (the
# page's own debug object) and from the CSS transform, so nothing depends on
# rendered text.
$harness = @'
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
  function tx(){                       /* translateX of the live page */
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
    var X0 = r.right - 8;             /* inside the right-hand 16px band */

    function ev(t, x){ return new MouseEvent(t, {clientX:x, clientY:Y, bubbles:true, cancelable:true}); }

    /* Press in the edge band, drag left by dx, and report what the page did
       *while the finger is still down*. */
    function drag(dx){
      phone.dispatchEvent(ev("mousedown", X0));
      for (var i = 1; i <= 4; i++) phone.dispatchEvent(ev("mousemove", X0 - dx * i / 4));
      var d = dbg();
      return { mid: d.mid, armed: d.armed, tx: tx(), ind: indCls(), phase: d.phase };
    }
    function up(dx){ window.dispatchEvent(ev("mouseup", X0 - dx)); }

    rec("rect=" + Math.round(r.width) + "x" + Math.round(r.height));

    /* ---- A: 220px past the 130px line -> the return animation must run ---- */
    var a = drag(220);
    rec("A_mid=" + a.mid + " A_armed=" + (a.armed ? 1 : 0) + " A_tx=" + a.tx + " A_ind=" + a.ind);
    up(220);
    rec("A_cls=" + cls());
    rec("A_phase=" + dbg().phase);

    waitFor(function(){ return dbg().phase === "idle"; }, function(ok){
      rec("A_reset=" + (ok ? "yes" : "timeout") + " A_end_tx=" + tx());

      /* ---- B: 40px -> over the 8px floor, under the 130px line ---------- */
      var b = drag(40);
      rec("B_ind=" + b.ind);
      up(40);
      rec("B_cls=" + cls());
      rec("B_phase=" + dbg().phase + " B_tx=" + tx());

      /* ---- C: 2px -> under the floor, must not even show the indicator -- */
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
'@

# ---- build the smoke page ---------------------------------------------------
$html = [IO.File]::ReadAllText($src, (New-Object Text.UTF8Encoding $false))
if ($html -notmatch '</body>') { Write-Host "[ERROR] no </body> in $src" -ForegroundColor Red; exit 2 }
$html = $html -replace '(?s)</body>\s*</html>\s*$', ''
[IO.File]::WriteAllText($dst, ($html + "`n" + $harness + "`n</body></html>`n"),
                        (New-Object Text.UTF8Encoding $false))

# ---- run it headless --------------------------------------------------------
# Redirect Edge's stdout to a file instead of catching it in a PowerShell
# variable: on this box the pipeline re-encodes the native output and mangles
# it, while a plain file redirect stays byte-exact. stderr (Chromium chatter)
# goes to nul on purpose -- with $ErrorActionPreference = "Stop" any line Edge
# writes there would otherwise abort the script.
$profile = Join-Path $env:TEMP ("smk_" + [guid]::NewGuid().ToString("N"))
$domOut  = Join-Path $env:TEMP ("smk_" + [guid]::NewGuid().ToString("N") + ".html")
$url     = "file:///" + ($dir -replace '\\', '/') + "/__smoke.html"

$edgeArgs = "--headless=new --disable-gpu --no-sandbox --no-first-run " +
        "--log-level=3 --disable-logging --window-size=1440,1500 " +
        "--user-data-dir=`"$profile`" --virtual-time-budget=30000 --dump-dom `"$url`""

& cmd /c "`"$edge`" $edgeArgs > `"$domOut`" 2>nul"

if (-not (Test-Path $domOut)) {
    Write-Host "[ERROR] headless render produced nothing" -ForegroundColor Red
    Remove-Item $profile -Recurse -Force -ErrorAction SilentlyContinue
    exit 2
}
$raw = [IO.File]::ReadAllText($domOut, (New-Object Text.UTF8Encoding $false))
Remove-Item $profile -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item $domOut  -Force -ErrorAction SilentlyContinue

$title = [regex]::Match($raw, '<title>([\s\S]*?)</title>').Groups[1].Value
$log   = if ($title -match '^SMOKE\|') { $title.Substring(6) } else { "" }

Write-Host ("harness: " + $title)
Write-Host ""

$checks = @(
  @{ n = "harness did not throw";          ok = ($title -notlike "SMOKE-ERR*") },
  @{ n = "phone element found";            ok = ($log -match 'rect=390x450') },
  @{ n = "all cases ran";                  ok = ($log -match '\bDONE\b') },
  @{ n = "A drag tracked = 220px";         ok = ($log -match 'A_mid=220\b') },
  @{ n = "A armed past the 130px line";    ok = ($log -match 'A_armed=1\b') },
  @{ n = "A indicator armed";              ok = ($log -match 'A_ind=ind_on_armed\b') },
  @{ n = "A page did NOT move while dragging"; ok = ($log -match 'A_tx=-?0(\.0+)?\b') },
  @{ n = "A verdict = go back";            ok = ($log -match 'A_cls=verdict_go\b') },
  @{ n = "A played the return animation";  ok = ($log -match 'A_phase=anim\b') },
  @{ n = "A animation completed + reset";  ok = ($log -match 'A_reset=yes\b') },
  @{ n = "A back at rest";                 ok = ($log -match 'A_end_tx=-?0(\.0+)?\b') },
  @{ n = "B indicator shown (over 8px)";   ok = ($log -match 'B_ind=ind_on\b') },
  @{ n = "B verdict = not enough";         ok = ($log -match 'B_cls=verdict_back\b') },
  @{ n = "B did NOT animate";              ok = ($log -match 'B_phase=idle\b') },
  @{ n = "B page still at rest";           ok = ($log -match 'B_tx=-?0(\.0+)?\b') },
  @{ n = "C indicator stayed hidden";      ok = ($log -match 'C_ind=ind\b') },
  @{ n = "C verdict untouched";            ok = ($log -match 'C_cls=verdict\b') },
  @{ n = "C distance recorded = 2px";      ok = ($log -match 'C_dx=2\b') }
)

$fail = 0
foreach ($c in $checks) {
    if ($c.ok) { Write-Host ("  PASS  " + $c.n) -ForegroundColor Green }
    else       { Write-Host ("  FAIL  " + $c.n) -ForegroundColor Red; $fail++ }
}

Write-Host ""
if ($fail -eq 0) {
    Write-Host ("ALL PASS  (" + $checks.Count + " checks)") -ForegroundColor Green
    exit 0
} else {
    Write-Host ("FAILED: " + $fail + " of " + $checks.Count) -ForegroundColor Red
    Write-Host ("raw: " + $title)
    exit 1
}
