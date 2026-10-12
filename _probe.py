import os, io, sys
out = io.StringIO()
def p(*a): print(*a, file=out)

D = r"C:\Users\18058\WorkBuddy\2026-09-19-18-37-14\.workbuddy\memory"
p("exists:", os.path.isdir(D))
if os.path.isdir(D):
    for n in sorted(os.listdir(D)):
        fp = os.path.join(D, n)
        p("  %-24s %8d" % (n, os.path.getsize(fp)))
    # 今天那份的尾部
    t = os.path.join(D, "2026-09-23.md")
    if os.path.exists(t):
        lines = open(t, encoding="utf-8", errors="replace").read().splitlines()
        p("")
        p("2026-09-23.md 共 %d 行，最后 12 行：" % len(lines))
        for ln in lines[-12:]:
            p("  | " + ln)
    else:
        p("")
        p("(2026-09-23.md 不存在)")
sys.stdout.write(out.getvalue())
open(r"C:\Users\18058\Desktop\huangshanpai\_probe.txt", "w", encoding="utf-8").write(out.getvalue())
