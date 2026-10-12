# -*- coding: utf-8 -*-
"""
smoke_2048_preview.py -- render + interaction smoke test for
                         ui-preview/game2048.html

为什么用 Python 而不是 .ps1：本机 PowerShell 工具会吞 stdout，重定向出来的文件
还会被重新编码并留下 ANSI 色码，导致「测试明明通过却读不出来」。Python 的 I/O 在
这台机器上是字节精确的。同样的坑在 smoke_torch_preview.py / smoke_swipe_preview.py
里都记过。

这一份测两件事：

  A. 渲染 —— 模拟屏是不是 390x450、棋盘是不是真的画在 (33,89) 且 324x324、
     16 个格子是不是 71x71 间距 8、两条手势条的位置对不对，以及顶部两个
     分数面板里的**汉字小标题和数字有没有叠**（真机出过这个事）。
     ⚠ 这些是**读出来的几何**（getBoundingClientRect），不是页面里写的常量。
       读常量等于没验 —— 常量错了读出来照样"对"。

  B. 算法 —— 走的是页面自己跑的 __hsp2048.selfTest（含"错误版必须真的错"那条），
     再加上一条**端到端**的：摆好 2 2 4 0 → move 左 → 读**渲染出来的 DOM 文本**，
     必须是 4 4 0 0。只看内存里的 board 不够 —— 那样界面没跟着变也测不出来。

  C. 顶部面板不许叠、不许贴住棋盘、必须整体居中 —— 见下面 L 组。
     真机两次返工的产物，别删。

⚠ 总检查数 **60**（2026-09-23 加 L 组之前是 49，加"不叠"那版是 56，
  加"和棋盘有缝 + 居中 + 标题已删"这版是 60）。改了这个列表记得同步这一行。

  ⚠ 数"渲染出来的节点"之前务必先剥掉 <script> 源码：dump-dom 会把 JS 里
    模板字符串的字面量一起吐出来，直接数会多算。见 static-web-render-verify 技能库。

用法：
    <python> tests/smoke_2048_preview.py
退出码 0 = 全部通过。
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PREVIEW = os.path.join(ROOT, "ui-preview")
SRC = os.path.join(PREVIEW, "game2048.html")
DST = os.path.join(PREVIEW, "__smoke_2048.html")

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
  function A(){ return window.__hsp2048 || {}; }
  function rnd(v){ return Math.round(v); }

  function boxOf(id){ var e=document.getElementById(id); return e?e.getBoundingClientRect():null; }

  /* ⚠ .phone 有 1px 边框，直接减 left/top 会整体偏 1px（实测 board=34,89 而不是 33,88）。
     要减掉 phone 自己的 border，量的才是"站在表屏里"的坐标。 */
  function phoneOrigin(){
    var p = document.getElementById("phone").getBoundingClientRect();
    var cs = getComputedStyle(document.getElementById("phone"));
    return { x: p.left + (parseFloat(cs.borderLeftWidth)||0),
             y: p.top  + (parseFloat(cs.borderTopWidth)||0) };
  }

  function relBox(id){
    var e = boxOf(id); if (!e) return null;
    var o = phoneOrigin();
    return { x:rnd(e.left-o.x), y:rnd(e.top-o.y), w:rnd(e.width), h:rnd(e.height) };
  }

  function key(k){
    document.dispatchEvent(new KeyboardEvent("keydown", {key:k, bubbles:true, cancelable:true}));
  }

  /* 从渲染出来的 DOM 读某一行 4 格的文本 —— 这才是"用户看得见"的东西 */
  function rowText(r){
    var board = document.getElementById("board");
    if (!board) return "NULL";
    /* 结构：16 个 .cell 之后紧跟 16 个 .tile，顺序都是 r*4+c */
    var tiles = board.querySelectorAll(".tile");
    if (tiles.length !== 16) return "NTILE=" + tiles.length;
    var out = [];
    for (var c=0;c<4;c++){
      var el = tiles[r*4+c];
      out.push((el.textContent||"0").trim() || "0");
    }
    return out.join(",");
  }
  function tileText(r,c){
    var board = document.getElementById("board");
    var tiles = board.querySelectorAll(".tile");
    return (tiles[r*4+c].textContent||"").trim() || "0";
  }
  function fontSizeOf(r,c){
    var board = document.getElementById("board");
    var tiles = board.querySelectorAll(".tile");
    return getComputedStyle(tiles[r*4+c]).fontSize;
  }
  function bgOf(r,c){
    var board = document.getElementById("board");
    var tiles = board.querySelectorAll(".tile"), t = tiles[r*4+c];
    /* ⚠ 读 inline 而不是 computed。.tile 上有 transition:background .08s ——
       参与动画的属性，computed 值取决于浏览器的动画时钟，而 headless +
       --virtual-time-budget 下那个时钟**不可靠**（实测两种情况都出现过：
       停在起始值 rgba(0,0,0,0)，或者已经走到最终色）。
       所以：断言动画属性 = 在测浏览器，不是在测页面。读 inline 才是读页面意图。 */
    return t.style.backgroundColor || t.style.background || "NONE";
  }
  /* 有多少格是真的画了东西（有文本） */
  function painted(){
    var board = document.getElementById("board");
    var tiles = board.querySelectorAll(".tile"), n=0;
    for (var i=0;i<tiles.length;i++){ if ((tiles[i].textContent||"").trim()) n++; }
    return n;
  }

  try {
    var phone = boxOf("phone");
    if (!phone) { rec("phone=NULL"); done(); return; }

    rec("rect=" + rnd(phone.width) + "x" + rnd(phone.height));

    /* ---------- A. 几何（读出来的，不是常量） ---------- */
    var b = relBox("board"), c00, c33;
    var cells = document.querySelectorAll("#board .cell");
    rec("ncells=" + cells.length + " ntiles=" + document.querySelectorAll("#board .tile").length);
    if (cells.length === 16) {
      var o = phoneOrigin();
      var r0 = cells[0].getBoundingClientRect();
      c00 = { x: rnd(r0.left-o.x), y: rnd(r0.top-o.y), w: rnd(r0.width), h: rnd(r0.height) };
      var r15 = cells[15].getBoundingClientRect();
      c33 = { x: rnd(r15.left-o.x), y: rnd(r15.top-o.y) };
    }
    rec("board=" + (b? b.x+","+b.y+" "+b.w+"x"+b.h : "NULL"));
    rec("cell00=" + (c00? c00.x+","+c00.y+" "+c00.w+"x"+c00.h : "NULL"));
    rec("cell33=" + (c33? c33.x+","+c33.y : "NULL"));

    /* 手势条：勾上之后应该有可见的虚线框，且宽度/位置对 */
    var showBars = document.getElementById("showBars");
    showBars.checked = true;
    showBars.dispatchEvent(new Event("change", {bubbles:true}));
    var tb = relBox("bartop"), rb = relBox("barright");
    rec("bartop=" + (tb? tb.x+","+tb.y+" "+tb.w+"x"+tb.h : "NULL")
        + " on=" + (document.getElementById("bartop").classList.contains("on")?1:0));
    rec("barright=" + (rb? rb.x+","+rb.y+" "+rb.w+"x"+rb.h : "NULL"));

    /* 棋盘右边界必须 < 返回条起点 —— 这是本工程最容易返工的一条 */
    if (b && rb) { rec("rightGap=" + (rb.x - (b.x + b.w))); }

    /* ---------- B. 初始局 ---------- */
    rec("init_tiles=" + d().tiles + " init_score=" + d().score + " init_painted=" + painted());
    var cnt = 0, tt = (d().tiles||"").split("|").join(",").split(",");
    for (var i=0;i<tt.length;i++){ if (parseInt(tt[i],10) > 0) cnt++; }
    rec("init_n=" + cnt);
    /* 开局两颗，且只能是 2 或 4 */
    var okVals = 1;
    for (i=0;i<tt.length;i++){ var v = parseInt(tt[i],10); if (v && v!==2 && v!==4) okVals = 0; }
    rec("init_okvals=" + okVals);

    /* ---------- C. 端到端：摆残局 2 2 4 0 往左划 ----------
       ⚠ 先关自动生成（= 固件的 g2048_spawn 0）。开着的话每走一步会随机冒一颗，
         棋盘上既有你的结果又有新块，断言只能写成"包含"，测不出真正的东西。 */
    A().setSpawn(0);
    rec("C_spawnoff=1");
    A().setBoard([[2,2,4,0],[0,0,0,0],[0,0,0,0],[0,0,0,0]]);
    rec("C_before=" + rowText(0) + " painted=" + painted() + " score=" + d().score);
    A().move(3);                                   /* 3 = 左 */
    rec("C_after=" + rowText(0) + " score=" + d().score + " painted=" + painted());

    /* ---------- D. 键盘也是同一条路 ---------- */
    A().setBoard([[0,0,0,0],[0,0,0,0],[2,2,0,0],[0,0,0,0]]);
    key("ArrowLeft");
    rec("D_row2=" + rowText(2) + " score=" + d().score);

    /* 往上划：第 1 列有 2 和 2 */
    A().setBoard([[2,0,0,0],[2,0,0,0],[0,0,0,0],[0,0,0,0]]);
    key("ArrowUp");
    rec("D_up_col0=" + tileText(0,0) + "," + tileText(1,0) + " score=" + d().score);

    /* 划不动的时候不该改盘、也不该加分 */
    A().setBoard([[2,4,8,16],[0,0,0,0],[0,0,0,0],[0,0,0,0]]);
    var s0 = d().score;
    A().move(3);
    rec("E_nomove=" + rowText(0) + " gained=" + (d().score - s0));

    /* ---------- F. 撤销：只一步，且"划不动"不覆盖上一步快照 ---------- */
    A().setBoard([[0,0,2,2],[0,0,0,0],[0,0,0,0],[0,0,0,0]]);
    A().move(3);                                   /* 左：2 2 -> 4，score +4 */
    rec("F_moved=" + rowText(0) + " / " + rowText(1) + " score=" + d().score);
    A().undo();
    rec("F_undone=" + rowText(0) + " / " + rowText(1) + " score=" + d().score);

    /* 划不动的一步不能把有效快照冲掉：先划一步、再划一个真没变化的方向、再撤销。
       关掉 spawn 之后 [4,0,0,0] 往左划确实是"没变化"，这条才有意义。 */
    A().setBoard([[0,0,2,2],[0,0,0,0],[0,0,0,0],[0,0,0,0]]);
    A().move(3);                                   /* 有效：-> 4,0,0,0 */
    rec("F_after_valid=" + rowText(0) + " score=" + d().score);
    A().move(3);                                   /* 无效：4,0,0,0 往左划没变化 */
    rec("F_after_noop=" + rowText(0) + " score=" + d().score);
    A().undo();
    rec("F_undo_after_noop=" + rowText(0) + " score=" + d().score);

    /* ---------- G. 字号随位数变 ---------- */
    A().setBoard([[2,16,128,2048],[65536,131072,4,8],[0,0,0,0],[0,0,0,0]]);
    rec("G_fs2=" + fontSizeOf(0,0) + " G_fs2048=" + fontSizeOf(0,3)
        + " G_fs65536=" + fontSizeOf(1,0) + " G_fs131072=" + fontSizeOf(1,1));
    rec("G_text65536=" + tileText(1,0));

    /* 65536 必须是深底色（>2048 的兜底色 #3C3A32 -> rgb(60, 58, 50)） */
    rec("G_bg65536=" + bgOf(1,0) + " G_bg65536_computed=" +
        getComputedStyle(document.querySelectorAll("#board .tile")[4]).backgroundColor);
    rec("G_bg2=" + bgOf(0,0));

    /* ---------- H. 遮罩：达成 2048 要弹，点"继续玩"要收 ---------- */
    A().setBoard([[1024,1024,0,0],[0,0,0,0],[0,0,0,0],[0,0,0,0]]);
    A().move(3);
    rec("H_mask_after_win=" + d().mask + " won=" + d().won + " max=" + d().maxTile);
    if (d().mask) {
      /* 遮罩上的"继续玩"是 #mOk */
      var mok = document.getElementById("mOk");
      var r2 = mok.getBoundingClientRect();
      mok.dispatchEvent(new MouseEvent("click", {
        clientX: r2.left + r2.width/2, clientY: r2.top + r2.height/2,
        bubbles: true, cancelable: true }));
      rec("H_mask_after_continue=" + d().mask);
    } else {
      rec("H_mask_after_continue=SKIP");
    }

    /* ---------- I. 重开按钮 ---------- */
    A().setBoard([[2,2,4,8],[0,0,0,0],[0,0,0,0],[0,0,0,0]]);
    rec("I_before_new=" + rowText(0) + " score=" + d().score);
    var bn = document.getElementById("btnNew");
    var r3 = bn.getBoundingClientRect();
    bn.dispatchEvent(new MouseEvent("click", {
      clientX: r3.left + r3.width/2, clientY: r3.top + r3.height/2,
      bubbles: true, cancelable: true }));
    rec("I_after_new_score=" + d().score + " painted=" + painted());

    /* ---------- J. 算法自检（页面自己跑的） ---------- */
    var st = (A().selfTest || {});
    rec("J_self=" + st.pass + "/" + st.total);
    rec("J_failed=" + (window.__selftestFailed || []).join("; "));
    rec("J_bad1=" + JSON.stringify(A().lineStepBad([2,2,4,0])));
    rec("J_bad2=" + JSON.stringify(A().lineStepSkip2([4,4,2,2])));
    rec("J_bad2b=" + JSON.stringify(A().lineStepSkip2([4,4,4,4])));

    /* ---------- K. 算法演示条渲染出来了 ---------- */
    rec("K_cases=" + document.querySelectorAll("#segCase button").length);
    rec("K_sq_in=" + document.querySelectorAll("#inRow .sq").length
        + " K_sq_ok=" + document.querySelectorAll("#okRow .sq").length
        + " K_sq_bad1=" + document.querySelectorAll("#bad1Row .sq").length
        + " K_sq_bad2=" + document.querySelectorAll("#bad2Row .sq").length);
    rec("K_selftest_rows=" + document.querySelectorAll("#selftest tr").length);
    rec("K_seg_sw=" + (function(){
      var btns = document.querySelectorAll("#segCase button");
      var r = btns[1].getBoundingClientRect();
      btns[1].dispatchEvent(new MouseEvent("click", {clientX:r.left+r.width/2, clientY:r.top+r.height/2, bubbles:true, cancelable:true}));
      var out = [];
      var sq2 = document.querySelectorAll("#okRow .sq");
      for (var i=0;i<sq2.length;i++) out.push(sq2[i].textContent);
      var b2 = document.querySelectorAll("#bad2Row .sq");
      for (i=0;i<b2.length;i++) out.push(b2[i].textContent);
      return out.join(",");
    }()));

    /* ---------- L. 顶部两个面板：内部不叠 + 和棋盘拉开缝 + 整体居中 ----------
       真机上这块出过**两次**事，所以现在要同时验三件事：

       ① 「汉字小标题和分数叠在一起」—— 根子是 label 占的高度是**行高**不是字号。
          freetype 下 SDK 把它写死成 size + size/3 + 4，
          所以 FONT_SMALL(16px) 实际 25px 高、FONT_NORMAL(20px) 实际 30px 高。
       ② 「分数框和最佳框底部和方格框重叠了」—— 那其实**不是真叠**，
          是余量正好 0px：两个 #BBADA0 的方块贴在一起，边线糊成一条，
          看着就像压上去了。所以断言要的是 gap > 0，不是 gap >= 0。
       ③ 真机反馈要求「两个框顶部居中」—— 这一条最容易悄悄回归
          （改个 padding、加个 margin 就偏了），必须押成数字。

       ⚠ 量的全是**渲染出来的盒子**（getBoundingClientRect），不是页面里的常量。
       ⚠ 所以预览页的 line-height 必须照抄固件的 25/30 ——
         写成 16/22 那种"好看"的值，就会把真机的问题盖住，等于白测。 */
    (function(){
      function relBoxSel(sel){
        var e = document.querySelector(sel); if (!e) return null;
        var r = e.getBoundingClientRect(), o = phoneOrigin();
        return { x:rnd(r.left-o.x), y:rnd(r.top-o.y), w:rnd(r.width), h:rnd(r.height) };
      }

      var panels = document.querySelectorAll(".panel");
      rec("L_panels=" + panels.length);

      var p0 = relBoxSel(".panel"), p1 = null;
      rec("L_panel1=" + (p0? p0.x+","+p0.y+" "+p0.w+"x"+p0.h : "NULL"));
      if (panels.length > 1) {
        var r1 = panels[1].getBoundingClientRect(), o1 = phoneOrigin();
        p1 = { x:rnd(r1.left-o1.x), y:rnd(r1.top-o1.y),
               w:rnd(r1.width), h:rnd(r1.height) };
      }
      rec("L_panel2=" + (p1? p1.x+","+p1.y+" "+p1.w+"x"+p1.h : "NULL"));

      var caps = document.querySelectorAll(".panel .cap2");
      var vals = document.querySelectorAll(".panel .val");
      var o = phoneOrigin(), worst = 99999;

      for (var i=0;i<caps.length;i++){
        var cb = caps[i].getBoundingClientRect(), vb = vals[i].getBoundingClientRect();
        var cTop = rnd(cb.top-o.y),  cBot = rnd(cb.bottom-o.y);
        var vTop = rnd(vb.top-o.y),  vBot = rnd(vb.bottom-o.y);
        var gap  = vTop - cBot;                 /* >0 有缝，=0 贴着，<0 叠了 */
        rec("L_cap" + i + "=" + cTop + ".." + cBot
            + " val" + i + "=" + vTop + ".." + vBot + " gap" + i + "=" + gap);
        if (gap < worst) { worst = gap; }
      }
      rec("L_worst_gap=" + worst);
      rec("L_lh=" + getComputedStyle(caps[0]).lineHeight + "/"
                  + getComputedStyle(vals[0]).lineHeight);

      /* ② 面板底边和棋盘顶边之间那一小条缝 —— 必须真的存在 */
      var b2 = relBox("board");
      if (p0 && b2) { rec("L_bandgap=" + (b2.y - (p0.y + p0.h))); }

      /* ③ 两个面板整体水平居中：整对的中点必须等于屏幕中点 */
      if (p0 && p1) {
        rec("L_pairmid=" + Math.floor((p0.x + (p1.x + p1.w - 1)) / 2)
            + " L_center=" + Math.floor((390 - 1) / 2)
            + " L_xgap=" + (p1.x - (p0.x + p0.w)));
      }

      /* ④ 真机已经去掉的两样东西，预览页也不能还留着 */
      rec("L_gstate=" + (document.getElementById("gState") ? 1 : 0)
          + " L_showhint=" + (document.getElementById("showHint") ? 1 : 0)
          + " L_gtitle=" + (document.querySelector(".gt") ? 1 : 0));
    })();

    rec("DONE");
    done();
  } catch (e) {
    document.title = "SMOKE-ERR|" + e.message + "|" + log.join(" | ");
  }
})();
</script>
"""

def num(l, key, default=None):
    """从 harness 的 key=value 里取一个整数。取不到返回 default。"""
    m = re.search(re.escape(key) + r"=(-?\d+)", l)
    return int(m.group(1)) if m else default


CHECKS = [
    ("harness did not throw",              lambda t, l: not t.startswith("SMOKE-ERR")),
    ("watch screen is 390x450",            lambda t, l: "rect=390x450" in l),
    ("all steps ran",                      lambda t, l: re.search(r"\bDONE\b", l) is not None),

    # --- A 几何（读出来的像素，不是页面里的常量） ---
    ("16 cells + 16 tiles",                lambda t, l: re.search(r"ncells=16 ntiles=16\b", l) is not None),
    ("board drawn at 33,89 324x324",       lambda t, l: "board=33,89 324x324" in l),
    ("cell00 at 41,97 71x71",              lambda t, l: "cell00=41,97 71x71" in l),
    ("cell33 at 278,334",                  lambda t, l: "cell33=278,334" in l),
    ("top gesture bar is 390x34 at 0,0",   lambda t, l: "bartop=0,0 390x34" in l),
    ("right bar is 16px wide at x=374",    lambda t, l: re.search(r"barright=374,0 16x\d+", l) is not None),
    ("board clears the right bar (17px)",  lambda t, l: re.search(r"rightGap=17\b", l) is not None),
    ("bars are visible when toggled",      lambda t, l: "on=1" in l),

    # --- B 初始局 ---
    ("new game has exactly 2 tiles",       lambda t, l: re.search(r"init_n=2\b", l) is not None),
    ("initial tiles are 2 or 4",           lambda t, l: re.search(r"init_okvals=1\b", l) is not None),
    ("initial score is 0",                 lambda t, l: re.search(r"init_score=0\b", l) is not None),
    ("initial paint matches board",        lambda t, l: re.search(r"init_painted=2\b", l) is not None),

    # --- C 端到端：2 2 4 -> 4 4（不是 8） ---
    ("C: 2,2,4,0 before",                  lambda t, l: "C_before=2,2,4,0" in l),
    ("C: 2,2,4,0 -> 4,4,0,0  (NOT 8)",     lambda t, l: "C_after=4,4,0,0" in l),
    ("C: score +4",                        lambda t, l: re.search(r"C_after=4,4,0,0 score=4\b", l) is not None),
    ("C: DOM repainted (still 2 tiles)",   lambda t, l: re.search(r"C_after=4,4,0,0 score=4 painted=2\b", l) is not None),

    # --- D 键盘 ---
    ("D: arrow-left merges row 2",         lambda t, l: "D_row2=4,0,0,0" in l),
    ("D: arrow-up merges col 0",           lambda t, l: "D_up_col0=4,0" in l),

    # --- E 无效方向 ---
    ("E: no-op move changes nothing",      lambda t, l: "E_nomove=2,4,8,16" in l),
    ("E: no-op move scores 0",             lambda t, l: re.search(r"E_nomove=2,4,8,16 gained=0\b", l) is not None),

    # --- F 撤销 ---
    ("F: move left merges 2+2 into a 4",   lambda t, l: re.search(r"F_moved=4,0,0,0 / 0,0,0,0 score=4\b", l) is not None),
    ("F: undo restores the board",         lambda t, l: re.search(r"F_undone=0,0,2,2 / 0,0,0,0", l) is not None),
    ("F: undo restores the score",         lambda t, l: re.search(r"F_undone=.* score=0\b", l) is not None),
    ("F: valid move applies",              lambda t, l: re.search(r"F_after_valid=4,0,0,0 score=4\b", l) is not None),
    ("F: no-op move changes nothing",      lambda t, l: re.search(r"F_after_noop=4,0,0,0 score=4\b", l) is not None),
    ("F: no-op did NOT clobber the snapshot",
                                           lambda t, l: re.search(r"F_undo_after_noop=0,0,2,2 score=0\b", l) is not None),

    # --- G 字号 ---
    ("G: 2 -> 36px",                       lambda t, l: re.search(r"G_fs2=36px\b", l) is not None),
    ("G: 2048 -> 24px",                    lambda t, l: re.search(r"G_fs2048=24px\b", l) is not None),
    ("G: 65536 -> 20px",                   lambda t, l: re.search(r"G_fs65536=20px\b", l) is not None),
    ("G: 131072 -> 16px",                  lambda t, l: re.search(r"G_fs131072=16px\b", l) is not None),
    ("G: text really reads 65536",         lambda t, l: "G_text65536=65536" in l),
    ("G: >2048 uses the dark tile",        lambda t, l: re.search(r"G_bg65536=rgb\(60, 58, 50\)", l) is not None),
    ("G: 2 uses the light beige tile",     lambda t, l: re.search(r"G_bg2=rgb\(238, 228, 218\)", l) is not None),
    # ⚠ 这一条**故意不断言 computed 的具体颜色**。
    #   原来写的是"computed 一定停在过渡的起始值 rgba(0,0,0,0)"，用来证明
    #   "读 computed 会读错"。但 headless + --virtual-time-budget 下过渡到底
    #   推不推进**不稳定**：同一份页面同一天两次跑，一次读到 rgba(0,0,0,0)，
    #   一次已经变成最终色 rgb(238, 228, 218)。
    #   断言一个浏览器的实现细节 = 给自己造一个随机失败的测试（2026-09-23 实测）。
    #   真正要紧的是"我们读 inline 而不是 computed"，那条在上面。
    ("G: computed style is dumped but NOT relied on (unstable in headless)",
                                           lambda t, l: "G_bg65536_computed=" in l),

    # --- H 遮罩 ---
    ("H: 1024+1024 shows the win mask",    lambda t, l: re.search(r"H_mask_after_win=1 won=1 max=2048\b", l) is not None),
    ("H: 'continue' dismisses the mask",   lambda t, l: re.search(r"H_mask_after_continue=0\b", l) is not None),

    # --- I 重开 ---
    ("I: 重开 clears the board",           lambda t, l: re.search(r"I_after_new_score=0\b", l) is not None),

    # --- J/K 页面自检与演示条 ---
    ("J: in-page self test 16/16",         lambda t, l: re.search(r"J_self=16/16\b", l) is not None),
    # 空列表时 harness 打出来是 "J_failed= | 下一个..." —— 用这个边界判"没有失败用例"。
    # 别写成 J_failed=\s*(\||)：那个正则永远匹配（含空串），是假通过。
    ("J: no failing case",                 lambda t, l: re.search(r"J_failed=\s*\|", l) is not None),
    ("J: wrong version 1 really is wrong", lambda t, l: "J_bad1=[8,0,0,0]" in l),
    ("J: wrong version 2 really is wrong", lambda t, l: "J_bad2=[8,2,2,0]" in l),
    ("J: wrong version 2 breaks 4,4,4,4",  lambda t, l: "J_bad2b=[8,4,4,0]" in l),
    ("K: 5 demo cases",                    lambda t, l: re.search(r"K_cases=5\b", l) is not None),
    ("K: all 4 strips have 4 squares",     lambda t, l: re.search(r"K_sq_in=4 K_sq_ok=4 K_sq_bad1=4 K_sq_bad2=4\b", l) is not None),
    ("K: self-test table rendered",        lambda t, l: re.search(r"K_selftest_rows=(\d+)", l) and
                                                          int(re.search(r"K_selftest_rows=(\d+)", l).group(1)) >= 16),
    ("K: switching case redraws both strips (4,4,2,2)",
                                           lambda t, l: re.search(r"K_seg_sw=8,4,0,0,8,2,2,0\b", l) is not None),

    # --- L 顶部面板：内部不叠 + 和棋盘有缝 + 整体居中（真机出过两次事） ---
    ("L: two score panels",                lambda t, l: re.search(r"L_panels=2\b", l) is not None),
    ("L: panel1 is 80x55 at x=111 y=28 (height = line heights 25+30, NOT font sizes)",
                                           lambda t, l: "L_panel1=111,28 80x55" in l),
    ("L: panel2 is 80x55 at x=199",        lambda t, l: "L_panel2=199,28 80x55" in l),
    ("L: line-height mirrors the firmware (25px / 30px)",
                                           lambda t, l: "L_lh=25px/30px" in l),
    ("L: caption box then value box, stacked exactly",
                                           lambda t, l: "L_cap0=28..53 val0=53..83" in l),
    ("L: caption and value boxes do NOT overlap (gap >= 0)",
                                           lambda t, l: num(l, "L_worst_gap") is not None
                                                         and num(l, "L_worst_gap") >= 0),
    ("L: ★ a real 6px gap between the panel and the board (this is the bug: it was 0)",
                                           lambda t, l: num(l, "L_bandgap") == 6),
    ("L: ★ the two panels are horizontally centered (pair mid == screen mid)",
                                           lambda t, l: num(l, "L_pairmid") is not None
                                                         and num(l, "L_pairmid") == num(l, "L_center")),
    ("L: 8px gutter between the two panels",
                                           lambda t, l: num(l, "L_xgap") == 8),
    ("L: the bottom 「第 N 步」 line is gone, like on the device",
                                           lambda t, l: re.search(r"L_gstate=0 L_showhint=0", l) is not None),
    ("L: the top-left 「2048」 title is gone (removing it is what frees the centering)",
                                           lambda t, l: num(l, "L_gtitle") == 0),
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
    profile = tempfile.mkdtemp(prefix="g2048_")
    dom = os.path.join(tempfile.gettempdir(), "g2048_dom_%d.html" % os.getpid())
    url = "file:///" + PREVIEW.replace("\\", "/") + "/__smoke_2048.html"
    args = [
        browser,
        "--headless=new", "--disable-gpu", "--no-sandbox", "--no-first-run",
        "--log-level=3", "--disable-logging", "--window-size=1440,2600",
        "--user-data-dir=" + profile,
        "--virtual-time-budget=30000",
        "--dump-dom", url,
    ]
    with open(dom, "wb") as out:
        subprocess.run(args, stdout=out, stderr=subprocess.DEVNULL, timeout=180)
    with open(dom, "rb") as f:
        raw = f.read().decode("utf-8", "replace")
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
            ok = bool(fn(title, log))
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
