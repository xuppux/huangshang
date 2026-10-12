/*
 * 计算器 App（calculator）
 * ========================
 *
 * 这是一个标准的「内置应用」，和 clock / rotation3d 一样通过
 * BUILTIN_APP_EXPORT 挂到主菜单的蜂窝里：
 *
 *   BUILTIN_APP_EXPORT(显示名, 图标, app id, 入口函数)
 *
 * 图标必须是主菜单能画出来的东西。icon 参数存的是「资源符号的地址」：
 *   LV_EXT_IMG_GET(img_calculator)  ->  (const void *)&img_calculator
 * 而 img_calculator 这个 lv_img_dsc_t 是构建脚本从下面这张 PNG 自动生成的：
 *   src/resource/images/common/ezip/img_calculator.png
 * 所以换图标只要换那张 PNG，不用动任何代码。
 *
 * ⚠ 主菜单只显示「icon 不是 NULL」的 app。这里图标传 NULL 的话，
 *   代码全对、编译也过，但蜂窝里就是看不到 —— 这是最容易白折腾半天的坑。
 *
 * 设计取舍（为什么是现在这个样子）：
 *   1. 走「算式 + 实时预览」而不是「老式计算器的一步步立刻出结果」。
 *      按 2+3×4 时，结果行直接显示 14 —— 用户不用猜优先级是怎么算的。
 *   2. 求值用递归下降（不是 eval、不是逆波兰），一层一个函数，安全、无栈溢出风险，
 *      括号深度限死 8 层。
 *   3. 数字格式化自己手写，不用 %f / %g：
 *      - 板子上的 printf 不一定支持浮点格式符；
 *      - %f 会打出 0.30000000000000004 这种尾巴（0.1+0.2 就是）。
 *      手写：先按 6 位小数四舍五入，再去掉末尾的 0。
 *   4. 按键不用 lv_btnmatrix，用独立的 lv_btn —— 因为 `=` 要跨 3 列、
 *      `0` 要跨 2 列，而且每个键的颜色不一样。btnmatrix 做这两件事都要绕。
 *
 * 依赖：只用到 LVGL 和图元绘制，不需要任何图片资源（除主菜单那个图标）。
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>
#include "littlevgl2rtt.h"
#include "lvgl.h"
#include "lvsf_comp.h"
#include "gui_app_fwk.h"
#include "lv_ext_resource_manager.h"
#include "lv_ex_data.h"
#include "app_mem.h"


/* ================================================================
 *  一、布局常量
 *  屏幕 390x450（LCD_HOR_RES_MAX x LCD_VER_RES_MAX）。
 *  顶部 34px 留给上划手势条（和表盘里电池块的避让逻辑一致）。
 * ================================================================ */
#define CARD_X      14
#define CARD_Y      34
#define CARD_W      (LV_HOR_RES_MAX - CARD_X * 2)                       /* 362 */
#define CARD_H      136

#define KEY_COLS    5
#define KEY_ROWS    5
#define KEY_GAP_X   6
#define KEY_GAP_Y   5
#define KEY_W       ((CARD_W - KEY_GAP_X * (KEY_COLS - 1)) / KEY_COLS)  /* 67  */
#define KEY_H       48

/* 键盘整体在卡片下方居中：362 宽，实际用了 359，两边各让 1px */
#define PAD_X       (CARD_X + (CARD_W - (KEY_W * KEY_COLS + KEY_GAP_X * (KEY_COLS - 1))) / 2)
#define PAD_Y       (CARD_Y + CARD_H + 10)                              /* 180 */

/* 返回键已删除（2026-09-22）。
 *
 * 原先卡片左上角有一个 64x40 的「←」按钮，点它 gui_app_goback() 回主菜单。
 * 现在右边缘左滑手势（app_utils/swipe_back.c）已经覆盖了这条路径，多一个
 * 按钮只是占地方、还多一个可能和手势冲突的触摸目标，所以整个去掉了。
 *
 * ⚠ 别和键盘第一行那个「←」搞混：那个是**退格**（in_back），是计算器功能，
 *   和"退出 app"毫无关系，必须留着。
 *
 * 腾出来的宽度给了算式行 —— 见下面 g_expr 的 max_width。
 */


/* ================================================================
 *  二、配色
 *  跟表盘校时面板同一套色（app_clock_band.c 里的 CAL_C_*），
 *  这样从表盘划到计算器不会有「换了台设备」的感觉。
 * ================================================================ */
#define C_PAGE      lv_color_make(0x14, 0x17, 0x1E)   /* 页面底 */
#define C_CARD      lv_color_make(0x1B, 0x1F, 0x27)   /* 显示屏卡片 */
#define C_KEY       lv_color_make(0x2A, 0x2F, 0x3A)   /* 普通键 */
#define C_KEY_OP    lv_color_make(0x34, 0x3B, 0x48)   /* 运算符键 */
#define C_KEY_DOWN  lv_color_make(0x45, 0x4E, 0x60)   /* 按下态 */
#define C_ACCENT    lv_color_make(0xFF, 0xC1, 0x07)   /* 琥珀：强调 */
#define C_TEXT      lv_color_make(0xFF, 0xFF, 0xFF)
#define C_DIM       lv_color_make(0x9A, 0xA3, 0xB2)
#define C_ERR       lv_color_make(0xFF, 0x5A, 0x52)

#define C_EQ_FG     lv_color_make(0x14, 0x17, 0x1E)   /* `=` 上的深色字 */


/* ================================================================
 *  三、求值引擎
 *  语法（带优先级，无空格）：
 *      expr   := term (('+' | '-') term)*
 *      term   := unary (('×' | '÷') unary)*
 *      unary  := ('+' | '-')* primary
 *      primary:= number ['%'] | '(' expr ')'
 *      number := 数字+ ['.' 数字*]
 * ================================================================ */
#define CM_MAX_DEPTH    8       /* 括号最多套 8 层 */
#define CM_MAX_DIGITS   15      /* 一个数最多 15 位（double 的整数精度） */

/* 错误码 */
#define CM_OK       0
#define CM_E_SYNTAX 1
#define CM_E_DIV0   2
#define CM_E_RANGE  3

typedef struct
{
    const char *p;
    int         err;
    int         depth;
} cm_ctx_t;

static int cm_is_digit(char c)
{
    return (c >= '0' && c <= '9');
}

/* 把当前位置的运算符规范成 ASCII，× ÷ 是 UTF-8 两字节也要认。
 * 返回 0 表示不是运算符；*len 给出这个字符占几个字节。 */
static char cm_op_at(const char *s, int i, int *len)
{
    unsigned char c = (unsigned char)s[i];

    if (c == '+' || c == '-' || c == '*' || c == '/')
    {
        *len = 1;
        return (char)c;
    }
    if (c == 0xC3 && (unsigned char)s[i + 1] == 0x97)   /* U+00D7 × */
    {
        *len = 2;
        return '*';
    }
    if (c == 0xC3 && (unsigned char)s[i + 1] == 0xB7)   /* U+00F7 ÷ */
    {
        *len = 2;
        return '/';
    }

    *len = 0;
    return 0;
}

static double cm_expr(cm_ctx_t *c);

static double cm_primary(cm_ctx_t *c)
{
    double v = 0.0;
    double scale;
    int    neg = 0;
    int    digits = 0;

    /* 一元正负号（"−5"、"(3)" 前面的负号） */
    while (*c->p == '-' || *c->p == '+')
    {
        if (*c->p == '-')
        {
            neg = !neg;
        }
        c->p++;
    }

    if (*c->p == '(')
    {
        c->p++;
        if (++c->depth > CM_MAX_DEPTH)
        {
            c->err = CM_E_SYNTAX;
            return 0.0;
        }
        v = cm_expr(c);
        c->depth--;
        if (c->err)
        {
            return 0.0;
        }
        if (*c->p != ')')
        {
            c->err = CM_E_SYNTAX;
            return 0.0;
        }
        c->p++;
    }
    else
    {
        while (cm_is_digit(*c->p))
        {
            v = v * 10.0 + (double)(*c->p - '0');
            c->p++;
            if (++digits > CM_MAX_DIGITS)
            {
                c->err = CM_E_RANGE;
                return 0.0;
            }
        }
        if (*c->p == '.')
        {
            c->p++;
            scale = 0.1;
            while (cm_is_digit(*c->p))
            {
                v += (double)(*c->p - '0') * scale;
                scale *= 0.1;
                c->p++;
                if (++digits > CM_MAX_DIGITS)
                {
                    c->err = CM_E_RANGE;
                    return 0.0;
                }
            }
        }
        if (digits == 0)
        {
            c->err = CM_E_SYNTAX;
            return 0.0;
        }
    }

    /* 后缀百分号：50% = 0.5，200×10% = 20 —— 和手机计算器一致 */
    if (*c->p == '%')
    {
        c->p++;
        v /= 100.0;
    }

    return neg ? -v : v;
}

static double cm_term(cm_ctx_t *c)
{
    double v = cm_primary(c);

    while (!c->err)
    {
        int    len = 0;
        char   op  = cm_op_at(c->p, 0, &len);
        double r;

        if (op == '*')
        {
            c->p += len;
            r = cm_primary(c);
            v *= r;
        }
        else if (op == '/')
        {
            c->p += len;
            r = cm_primary(c);
            if (c->err)
            {
                break;
            }
            if (r == 0.0)
            {
                c->err = CM_E_DIV0;
                break;
            }
            v /= r;
        }
        else
        {
            break;
        }

        if (v > 1e300 || v < -1e300 || v != v)
        {
            c->err = CM_E_RANGE;
            break;
        }
    }

    return v;
}

static double cm_expr(cm_ctx_t *c)
{
    double v = cm_term(c);

    while (!c->err)
    {
        int  len = 0;
        char op  = cm_op_at(c->p, 0, &len);

        if (op == '+')
        {
            c->p += len;
            v += cm_term(c);
        }
        else if (op == '-')
        {
            c->p += len;
            v -= cm_term(c);
        }
        else
        {
            break;
        }

        if (v > 1e300 || v < -1e300 || v != v)
        {
            c->err = CM_E_RANGE;
            break;
        }
    }

    return v;
}

/* 求值。返回 0 表示成功。 */
static int cm_eval(const char *s, double *out)
{
    cm_ctx_t c;

    c.p     = s;
    c.err   = CM_OK;
    c.depth = 0;

    *out = cm_expr(&c);

    if (!c.err && *c.p != '\0')
    {
        c.err = CM_E_SYNTAX;        /* 算式尾巴上有没吃掉的东西，比如 "3+4)" */
    }

    return c.err;
}

/* ----------------------------------------------------------------
 * 数字 -> 字符串。不用 %f / %g，见文件头的说明。
 * ---------------------------------------------------------------- */
#define CM_FRAC_DIGITS  6

static void cm_str_copy(char *d, const char *s, int n)
{
    int i = 0;

    while (i < n - 1 && s[i])
    {
        d[i] = s[i];
        i++;
    }
    d[i] = '\0';
}

static void cm_fmt(double v, char *buf, int n)
{
    long long ip, fr;
    char      tmp[32];
    char      fs[CM_FRAC_DIGITS];
    int       neg, o = 0, k, i, fend;

    if (n <= 0)
    {
        return;
    }

    if (v != v)                     /* NaN */
    {
        cm_str_copy(buf, "错误", n);
        return;
    }

    if (v > 1e300 || v < -1e300)    /* inf */
    {
        cm_str_copy(buf, "溢出", n);
        return;
    }

    neg = (v < 0.0);
    if (neg)
    {
        v = -v;
    }

    if (v < 5e-7)                   /* 太小的当 0，免得显示成 1e-07 */
    {
        cm_str_copy(buf, "0", n);
        return;
    }

    if (v >= 1e15)
    {
        cm_str_copy(buf, "溢出", n);
        return;
    }

    ip = (long long)v;

    /* 1e12 以上 double 的尾数已经分不出小数了，硬算会算出垃圾数字 */
    if (v >= 1e12)
    {
        fr = 0;
    }
    else
    {
        /* 关键一步：按 6 位小数四舍五入。0.1+0.2 = 0.30000000000000004
         * 到这里就变成 0.3，尾巴没了。 */
        fr = (long long)((v - (double)ip) * 1000000.0 + 0.5);
        if (fr >= 1000000)
        {
            ip++;
            fr = 0;
        }
    }

    /* 整数部分：逐位取出来再倒着放（不想用 %lld，理由同上） */
    k = 0;
    if (ip == 0)
    {
        tmp[k++] = '0';
    }
    while (ip > 0)
    {
        tmp[k++] = (char)('0' + (int)(ip % 10));
        ip /= 10;
    }

    if (neg && o < n - 1)
    {
        buf[o++] = '-';
    }
    while (k > 0 && o < n - 1)
    {
        buf[o++] = tmp[--k];
    }

    /* 小数部分：补足 6 位 -> 去掉末尾的 0 */
    if (fr > 0)
    {
        for (i = CM_FRAC_DIGITS - 1; i >= 0; i--)
        {
            fs[i] = (char)('0' + (int)(fr % 10));
            fr /= 10;
        }
        fend = CM_FRAC_DIGITS;
        while (fend > 0 && fs[fend - 1] == '0')
        {
            fend--;
        }

        if (o < n - 1)
        {
            buf[o++] = '.';
        }
        for (i = 0; i < fend && o < n - 1; i++)
        {
            buf[o++] = fs[i];
        }
    }

    buf[o] = '\0';
}


/* ================================================================
 *  四、输入状态
 *  表达式里存的是「显示形式」：× ÷ 直接是那两字节的 UTF-8，
 *  所以删字符、找运算符都要按 UTF-8 的字符边界来（否则会删出半个字，
 *  屏幕上就是一个乱码方块）。
 * ================================================================ */
#define CALC_EXPR_MAX   48

static char s_expr[CALC_EXPR_MAX];      /* 用户敲的算式 */
static char s_hist[CALC_EXPR_MAX];      /* 上一次按 = 时的算式（按完 = 后显示 "xx ="） */
static int  s_done;                     /* 1 = 刚按完 = ，s_expr 里放的是结果 */
static int  s_err;                      /* 最近一次的错误码，非 0 时结果行显示「错误」 */

/* 最后一个「字符」的起始下标。× ÷ 是两字节，要退回到首字节。 */
static int last_ch_start(void)
{
    int n = (int)strlen(s_expr);
    unsigned char c;

    if (n == 0)
    {
        return -1;
    }

    c = (unsigned char)s_expr[n - 1];
    if ((c == 0x97 || c == 0xB7) && n >= 2 && (unsigned char)s_expr[n - 2] == 0xC3)
    {
        return n - 2;
    }

    return n - 1;
}

/* 取出「结束于 end（不含）」的那个字符的起始下标 */
static int ch_start_before(const char *s, int end)
{
    unsigned char c;

    if (end <= 0)
    {
        return -1;
    }

    c = (unsigned char)s[end - 1];
    if ((c == 0x97 || c == 0xB7) && end >= 2 && (unsigned char)s[end - 2] == 0xC3)
    {
        return end - 2;
    }

    return end - 1;
}

/* 末位是不是「一个数已经写完」的样子（数字 / 小数点 / % / 右括号）。
 * "3(" 这种要自动补乘号、"(" 后面按小数点要补 0，都靠它判断。 */
static int last_is_value_end(void)
{
    int  st = last_ch_start();
    char c;

    if (st < 0)
    {
        return 0;
    }

    c = s_expr[st];
    return (cm_is_digit(c) || c == '.' || c == '%' || c == ')');
}

static void ex_append(const char *s)
{
    int n = (int)strlen(s_expr);
    int l = (int)strlen(s);

    if (n + l >= CALC_EXPR_MAX - 1)
    {
        rt_kprintf("calc: expression is full (%d chars)\n", CALC_EXPR_MAX - 1);
        return;
    }

    strcpy(s_expr + n, s);
}

/* 把「还没写完」的算式补成能算的，给实时预览用：
 *   1. 末尾的运算符 / 悬空小数点 / 空左括号 剪掉 —— "12+" 先让用户看到 12
 *   2. 左括号没配平就补右括号 —— "2×(3+4" 也能预览
 * 返回 0 表示没有可预览的内容。 */
static int cm_preview_input(const char *src, char *dst, int n)
{
    int i = 0, k = 0, open = 0;

    while (src[i] && k < n - 1)
    {
        dst[k++] = src[i++];
    }
    dst[k] = '\0';

    /* 剪掉末尾写了一半的东西。`(` 也要剪 —— "2×(" 是没法算的。 */
    for (;;)
    {
        char last;

        if (k <= 0)
        {
            break;
        }

        last = dst[k - 1];
        if (last == '+' || last == '-' || last == '*' || last == '/' ||
                last == '.' || last == '(')
        {
            dst[--k] = '\0';
            continue;
        }
        /* 末尾是 ×/÷ 的第二个字节，要连首字节一起剪 */
        if ((unsigned char)last == 0x97 || (unsigned char)last == 0xB7)
        {
            if (k >= 2 && (unsigned char)dst[k - 2] == 0xC3)
            {
                k -= 2;
                dst[k] = '\0';
                continue;
            }
        }
        break;
    }

    /* 重新数一遍没配平的左括号 */
    for (i = 0; i < k; i++)
    {
        if (dst[i] == '(')
        {
            open++;
        }
        else if (dst[i] == ')')
        {
            open--;
        }
    }
    while (open-- > 0 && k < n - 1)
    {
        dst[k++] = ')';
    }
    dst[k] = '\0';

    return (k > 0) ? 1 : 0;
}


/* ================================================================
 *  五、UI 对象（只在本文件用）
 * ================================================================ */
static lv_obj_t *g_expr;        /* 算式行 */
static lv_obj_t *g_res;         /* 结果行 */
static lv_obj_t *g_root;        /* 本 app 的根容器（框架自己会回收屏幕，这里只是留个引用好打日志） */

static void refresh_ui(void);

/* ---------------- 按键处理 ---------------- */

static void in_digit(char d)
{
    int n, i;

    if (s_done)
    {
        s_expr[0] = '\0';
        s_hist[0] = '\0';
        s_done = 0;
    }
    s_err = 0;

    /* 末位是 % 或 ) 的时候直接接数字会让算式不成立（"50%3"、"3)4"），
     * 按隐含乘法接上 —— 和 "5(" 自动补 × 是同一个规则。 */
    if (last_is_value_end())
    {
        char c = s_expr[last_ch_start()];
        if (c == '%' || c == ')')
        {
            ex_append("×");
        }
    }

    /* 前导 0：当前这个数整个就是 "0" 的时候，把这一位换掉，免得出现 0007。
     * 注意要排除 "1.0" 这种 —— 末尾的 0 只是小数位的 0，得往后加而不是替换。 */
    n = (int)strlen(s_expr);
    i = n;
    while (i > 0 && cm_is_digit(s_expr[i - 1]))
    {
        i--;
    }
    if (n - i == 1 && s_expr[i] == '0' && (i == 0 || s_expr[i - 1] != '.'))
    {
        s_expr[i] = d;
        return;
    }

    {
        char one[2];
        one[0] = d;
        one[1] = '\0';
        ex_append(one);
    }
}

static void in_dot(void)
{
    int n, i;

    if (s_done)
    {
        s_expr[0] = '\0';
        s_hist[0] = '\0';
        s_done = 0;
    }
    s_err = 0;

    /* 当前这个数里已经有小数点了就不再加 */
    n = (int)strlen(s_expr);
    i = n;
    while (i > 0 && (cm_is_digit(s_expr[i - 1]) || s_expr[i - 1] == '.'))
    {
        if (s_expr[i - 1] == '.')
        {
            return;
        }
        i--;
    }

    if (!last_is_value_end())
    {
        ex_append("0.");        /* "5×" 后面按小数点 -> "5×0." */
        return;
    }

    ex_append(".");
}

static void in_op(const char *sym)
{
    int i, st, len;

    s_err = 0;
    s_done = 0;                 /* 结果是接着往下算的，不清空 */

    if (s_expr[0] == '\0')
    {
        if (sym[0] == '-')
        {
            ex_append("-");     /* 允许从负数开始 */
        }
        return;
    }

    st = last_ch_start();
    if (st >= 0)
    {
        char prev = cm_op_at(s_expr, st, &len);

        /* 5× 后面按减号：这是"负数"，不是"减号" —— 直接接上
         * （没有歧义：× 右边不可能再跟一个二元减号）。
         * 想要 5×(3-1) 那种就用括号键。 */
        if (sym[0] == '-' && (prev == '*' || prev == '/'))
        {
            ex_append("-");
            return;
        }

        /* 末尾是运算符就把它清掉，最多清两层。
         * 这样 "5+×3" 不会出现，按错运算符可以直接改。 */
        for (i = 0; i < 2; i++)
        {
            st = last_ch_start();
            if (st < 0 || !cm_op_at(s_expr, st, &len))
            {
                break;
            }
            s_expr[st] = '\0';
        }
    }

    if (s_expr[0] == '\0')
    {
        if (sym[0] == '-')
        {
            ex_append("-");
        }
        return;
    }

    ex_append(sym);
}

static void in_percent(void)
{
    if (s_done)
    {
        return;                 /* 结果后面不接百分号 */
    }
    s_err = 0;
    if (!last_is_value_end())
    {
        return;
    }
    ex_append("%");
}

static void in_lparen(void)
{
    if (s_done)
    {
        s_expr[0] = '\0';
        s_hist[0] = '\0';
        s_done = 0;
    }
    s_err = 0;

    /* "5(" 自动补成 "5×("，符合直觉 */
    if (last_is_value_end())
    {
        ex_append("×");
    }
    ex_append("(");
}

static void in_rparen(void)
{
    int i, open = 0;

    if (s_done)
    {
        return;
    }
    s_err = 0;

    for (i = 0; s_expr[i]; i++)
    {
        if (s_expr[i] == '(')
        {
            open++;
        }
        else if (s_expr[i] == ')')
        {
            open--;
        }
    }

    if (open <= 0 || !last_is_value_end())
    {
        return;                 /* 没有可闭合的括号，或者括号里还没写完 */
    }

    ex_append(")");
}

static void in_back(void)
{
    int n = (int)strlen(s_expr);

    s_err = 0;

    if (n == 0)
    {
        s_done = 0;
        s_hist[0] = '\0';
        return;
    }

    /* × ÷ 是两个字节，整块删掉，否则会留下半个字符（屏幕上就是乱码） */
    if (n >= 2 && (unsigned char)s_expr[n - 1] == 0x97 &&
            (unsigned char)s_expr[n - 2] == 0xC3)
    {
        s_expr[n - 2] = '\0';
    }
    else if (n >= 2 && (unsigned char)s_expr[n - 1] == 0xB7 &&
             (unsigned char)s_expr[n - 2] == 0xC3)
    {
        s_expr[n - 2] = '\0';
    }
    else
    {
        s_expr[n - 1] = '\0';
    }

    s_done = 0;
}

static void in_sign(void)
{
    int n, i, len, st;
    char *p;

    if (s_expr[0] == '\0')
    {
        return;
    }
    s_err = 0;

    n = (int)strlen(s_expr);

    /* 找到最后一个数的起点 */
    i = n;
    while (i > 0 && (cm_is_digit(s_expr[i - 1]) || s_expr[i - 1] == '.'))
    {
        i--;
    }
    if (i == n)
    {
        return;                 /* 末尾不是数字，忽略 */
    }

    /* 前面已经有个负号 -> 去掉它 */
    if (i > 0 && s_expr[i - 1] == '-')
    {
        int ps = ch_start_before(s_expr, i - 1);
        int unary = (ps < 0) || (s_expr[ps] == '(') || (cm_op_at(s_expr, ps, &len) != 0);

        if (unary)
        {
            memmove(s_expr + i - 1, s_expr + i, strlen(s_expr + i) + 1);
            return;
        }
    }

    /* 否则插一个负号进去（"5-3" -> "5--3"，值就是 8） */
    if ((int)strlen(s_expr) + 1 < CALC_EXPR_MAX - 1)
    {
        p = s_expr + i;
        memmove(p + 1, p, strlen(p) + 1);
        *p = '-';
    }
}

static void in_eq(void)
{
    char   buf[CALC_EXPR_MAX];
    char   out[40];
    double v;
    int    e;

    if (!cm_preview_input(s_expr, buf, sizeof(buf)))
    {
        return;                 /* 空算式，什么都不做 */
    }

    e = cm_eval(buf, &v);
    if (e)
    {
        s_err = e;
        rt_kprintf("calc: eval \"%s\" -> error %d\n", buf, e);
        return;                 /* 算不出来就什么都不改，用户还能接着改 */
    }

    cm_fmt(v, out, sizeof(out));

    rt_kprintf("calc: %s = %s\n", buf, out);

    /* 把结果变成新的算式，可以接着按运算符往下算 */
    cm_str_copy(s_hist, buf, sizeof(s_hist));
    cm_str_copy(s_expr, out, sizeof(s_expr));
    s_done = 1;
    s_err  = 0;
}

static void calc_input(char code)
{
    switch (code)
    {
    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
        in_digit(code);
        break;

    case '.':
        in_dot();
        break;

    case '+':
        in_op("+");
        break;
    case '-':
        in_op("-");
        break;
    case '*':
        in_op("×");
        break;
    case '/':
        in_op("÷");
        break;

    case '%':
        in_percent();
        break;
    case '(':
        in_lparen();
        break;
    case ')':
        in_rparen();
        break;

    case 'N':
        in_sign();
        break;
    case 'B':
        in_back();
        break;

    case 'C':
        s_expr[0] = '\0';
        s_hist[0] = '\0';
        s_done = 0;
        s_err  = 0;
        rt_kprintf("calc: cleared\n");
        break;

    case 'E':
        in_eq();
        break;

    default:
        return;
    }

    /* 每次按键都算一次「用户活动」，免得算到一半屏幕自己熄了 */
    lv_disp_trig_activity(NULL);
    refresh_ui();
}


/* ================================================================
 *  六、界面
 * ================================================================ */

/* 显示屏上的两行字。改完文字要重新挂一次字体：
 * lv_ext_set_local_font 挂的是局部样式，而且长算式要换小一号字。 */
static void refresh_ui(void)
{
    char   buf[CALC_EXPR_MAX];
    char   out[40];
    char   line[CALC_EXPR_MAX + 8];
    double v;
    int    e;

    /* ---- 算式行 ---- */
    if (s_done && s_hist[0])
    {
        /* 刚按完 = ：上面那行显示"原来的算式 ="，一眼能对上是哪道题 */
        int l = (int)strlen(s_hist);
        if (l + 2 < (int)sizeof(line))
        {
            memcpy(line, s_hist, l);
            line[l]     = ' ';
            line[l + 1] = '=';
            line[l + 2] = '\0';
        }
        else
        {
            cm_str_copy(line, s_hist, sizeof(line));
        }
        lv_label_set_text(g_expr, line);
        lv_ext_set_local_font(g_expr, FONT_SMALL, C_DIM);
    }
    else if (s_expr[0] == '\0')
    {
        lv_label_set_text(g_expr, "");
    }
    else
    {
        lv_label_set_text(g_expr, s_expr);
        /* 长算式换小一号字，不让它冲出卡片 */
        lv_ext_set_local_font(g_expr,
                              (strlen(s_expr) > 20) ? FONT_SMALL : FONT_NORMAL,
                              C_DIM);
    }

    /* ---- 结果行 ---- */
    if (s_err)
    {
        cm_str_copy(out, "错误", sizeof(out));
    }
    else if (s_expr[0] == '\0')
    {
        cm_str_copy(out, "0", sizeof(out));
    }
    else
    {
        out[0] = '\0';
        if (cm_preview_input(s_expr, buf, sizeof(buf)))
        {
            e = cm_eval(buf, &v);
            if (e == 0)
            {
                cm_fmt(v, out, sizeof(out));
            }
            /* 算不出来就留空 —— 算式还没写完呢，别闪一个"错误"吓人 */
        }
    }

    lv_label_set_text(g_res, out);
    /* 结果太长就换小一号字（"1234567.89" 在 36px 下就快顶满了） */
    lv_ext_set_local_font(g_res,
                          (strlen(out) > 11) ? FONT_TITLE : FONT_BIGL,
                          s_err ? C_ERR : (s_done ? C_ACCENT : C_TEXT));
}


static void key_event_cb(lv_event_t *e)
{
    char code = (char)(rt_ubase_t)lv_event_get_user_data(e);

    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        calc_input(code);
    }
}


/* ================================================================
 *  七、键盘定义
 *  kind: 0 数字  1 运算符  2 功能  3 等号
 *  span: 占几列
 * ================================================================ */
typedef struct
{
    const char *label;
    char        code;
    rt_uint8_t  kind;
    rt_uint8_t  span;
} calc_key_t;

static const calc_key_t key_tab[] =
{
    /* 第 0 行 */
    { "C",  'C', 2, 1 }, { "←", 'B', 2, 1 }, { "(", '(', 2, 1 }, { ")", ')', 2, 1 }, { "÷", '/', 1, 1 },
    /* 第 1 行 */
    { "7",  '7', 0, 1 }, { "8", '8', 0, 1 }, { "9", '9', 0, 1 }, { "%", '%', 1, 1 }, { "×", '*', 1, 1 },
    /* 第 2 行 */
    { "4",  '4', 0, 1 }, { "5", '5', 0, 1 }, { "6", '6', 0, 1 }, { "±", 'N', 2, 1 }, { "-", '-', 1, 1 },
    /* 第 3 行 */
    { "1",  '1', 0, 1 }, { "2", '2', 0, 1 }, { "3", '3', 0, 1 }, { ".", '.', 0, 1 }, { "+", '+', 1, 1 },
    /* 第 4 行：0 跨两列，= 跨三列 */
    { "0",  '0', 0, 2 }, { "=",  'E', 3, 3 },
};

#define KEY_N   (sizeof(key_tab) / sizeof(key_tab[0]))

static lv_obj_t *mk_btn(lv_obj_t *par, const char *text, int font,
                        lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                        lv_color_t bg, lv_color_t fg)
{
    lv_obj_t *btn = lv_btn_create(par);
    lv_obj_t *lbl;

    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_radius(btn, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, bg, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    /* 按下时提亮一档，手指有反馈 */
    lv_obj_set_style_bg_color(btn, C_KEY_DOWN, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    /* 减少误触：手掌/手指滑过不触发 */
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_PRESS_LOCK);

    lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_ext_set_local_font(lbl, font, fg);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

    return btn;
}

static void calc_create(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_t *card;
    lv_obj_t *o;
    int       i, col = 0, row = 0;

    lv_obj_set_style_bg_color(scr, C_PAGE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    g_root = lv_obj_create(scr);
    lv_obj_set_size(g_root, LV_HOR_RES_MAX, LV_VER_RES_MAX);
    lv_obj_set_pos(g_root, 0, 0);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(g_root, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(g_root, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 显示屏卡片 ---- */
    card = lv_obj_create(g_root);
    lv_obj_set_size(card, CARD_W, CARD_H);
    lv_obj_set_pos(card, CARD_X, CARD_Y);
    lv_obj_set_style_radius(card, 16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(card, C_CARD, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(card, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 算式行（右上角）
     * 返回键删掉之后不再需要为它留 84px，左右各留 14px 即可 —— 长算式
     * 现在能多显示 84px，正好是原来那个按钮占掉的宽度。 ---- */
    g_expr = lv_label_create(card);
    lv_label_set_text(g_expr, "");
    lv_obj_set_style_max_width(g_expr, CARD_W - 28, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_long_mode(g_expr, LV_LABEL_LONG_CLIP);
    lv_obj_align(g_expr, LV_ALIGN_TOP_RIGHT, -14, 14);
    lv_ext_set_local_font(g_expr, FONT_NORMAL, C_DIM);

    /* ---- 结果行（右下角） ---- */
    g_res = lv_label_create(card);
    lv_label_set_text(g_res, "0");
    lv_obj_set_style_max_width(g_res, CARD_W - 28, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_long_mode(g_res, LV_LABEL_LONG_CLIP);
    lv_obj_align(g_res, LV_ALIGN_BOTTOM_RIGHT, -14, -12);
    lv_ext_set_local_font(g_res, FONT_BIGL, C_TEXT);

    /* ---- 键盘 ---- */
    for (i = 0; i < (int)KEY_N; i++)
    {
        int      span = key_tab[i].span ? key_tab[i].span : 1;
        int      w    = KEY_W * span + KEY_GAP_X * (span - 1);
        lv_coord_t x  = PAD_X + col * (KEY_W + KEY_GAP_X);
        lv_coord_t y  = PAD_Y + row * (KEY_H + KEY_GAP_Y);
        lv_color_t bg, fg;

        switch (key_tab[i].kind)
        {
        case 0:                                 /* 数字 */
            bg = C_KEY;
            fg = C_TEXT;
            break;
        case 1:                                 /* 运算符 */
            bg = C_KEY_OP;
            fg = C_ACCENT;
            break;
        case 3:                                 /* = */
            bg = C_ACCENT;
            fg = C_EQ_FG;
            break;
        default:                                /* 功能键 */
            bg = C_KEY;
            fg = (key_tab[i].code == 'C') ? C_ERR : C_DIM;
            break;
        }

        o = mk_btn(g_root, key_tab[i].label, FONT_TITLE, x, y, w, KEY_H, bg, fg);
        lv_obj_add_event_cb(o, key_event_cb, LV_EVENT_CLICKED,
                            (void *)(rt_ubase_t)(unsigned char)key_tab[i].code);

        col += span;
        if (col >= KEY_COLS)
        {
            col = 0;
            row++;
        }
    }

    /* ---- 几何自检：摆在哪里、有没有压到显示屏，从串口就能确认 ---- */
    rt_kprintf("calc: card  x=%d..%d y=%d..%d (%dx%d)\n",
               CARD_X, CARD_X + CARD_W - 1, CARD_Y, CARD_Y + CARD_H - 1, CARD_W, CARD_H);
    rt_kprintf("calc: pad   x=%d..%d y=%d..%d  key %dx%d gap %dx%d  rows=%d cols=%d\n",
               PAD_X, PAD_X + KEY_W * KEY_COLS + KEY_GAP_X * (KEY_COLS - 1) - 1,
               PAD_Y, PAD_Y + KEY_H * KEY_ROWS + KEY_GAP_Y * (KEY_ROWS - 1) - 1,
               KEY_W, KEY_H, KEY_GAP_X, KEY_GAP_Y, KEY_ROWS, KEY_COLS);
    rt_kprintf("calc: keys=%d  screen=%dx%d  card_bottom=%d pad_top=%d (gap %d)\n",
               (int)KEY_N, LV_HOR_RES_MAX, LV_VER_RES_MAX,
               CARD_Y + CARD_H, PAD_Y, PAD_Y - (CARD_Y + CARD_H));
}

static void calc_reset(void)
{
    s_expr[0] = '\0';
    s_hist[0] = '\0';
    s_done = 0;
    s_err  = 0;
    g_expr = NULL;
    g_res  = NULL;
    g_root = NULL;
}


/* ================================================================
 *  八、app 生命周期
 *  框架会为每个 app 单独 create/load/del 一块屏幕
 *  （见 sifli-sdk/middleware/app_fwk/app_schedule_port.c），
 *  所以这里在 on_start 里往 lv_scr_act() 上画就行，on_stop 不用手动删。
 * ================================================================ */
static void on_start(void)
{
    rt_kprintf("calc: start\n");
    calc_reset();
    calc_create();
}

static void on_resume(void)
{
    rt_kprintf("calc: resume\n");
}

static void on_pause(void)
{
    rt_kprintf("calc: pause\n");
}

static void on_stop(void)
{
    rt_kprintf("calc: stop\n");
    calc_reset();
}

static void msg_handler(gui_app_msg_type_t msg, void *param)
{
    switch (msg)
    {
    case GUI_APP_MSG_ONSTART:
        on_start();
        break;

    case GUI_APP_MSG_ONRESUME:
        on_resume();
        break;

    case GUI_APP_MSG_ONPAUSE:
        on_pause();
        break;

    case GUI_APP_MSG_ONSTOP:
        on_stop();
        break;

    default:
        break;
    }
}

LV_IMG_DECLARE(img_calculator);

#define APP_ID "calculator"

static int app_main(intent_t i)
{
    gui_app_regist_msg_handler(APP_ID, msg_handler);

    return 0;
}

/* 图标不是 NULL 才会出现在主菜单的蜂窝里，这条务必别漏 */
BUILTIN_APP_EXPORT(LV_EXT_STR_ID(calculator), LV_EXT_IMG_GET(img_calculator),
                   APP_ID, app_main);


/* ================================================================
 *  九、串口自检
 *  算术这种东西光看屏幕数不对是看不出来的（少一个 0 一样像对的），
 *  所以把求值引擎和输入规则都用固定用例跑一遍，串口里逐条对。
 *  用法： calc_test
 * ================================================================ */
#ifdef RT_USING_FINSH

#include <finsh.h>

typedef struct
{
    const char *seq;        /* 按键序列，一个字符一个键；= 是等号键 */
    const char *want;       /* 期望屏幕上算式行/结果行的内容（want 为空表示只看不报错） */
    const char *note;
} calc_case_t;

/* 跑一串按键 */
static void calc_run_seq(const char *seq)
{
    s_expr[0] = '\0';
    s_hist[0] = '\0';
    s_done = 0;
    s_err  = 0;

    while (*seq)
    {
        switch (*seq)
        {
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            in_digit(*seq);
            break;
        case '.': in_dot();      break;
        case '+': in_op("+");    break;
        case '-': in_op("-");    break;
        case '*': in_op("×");    break;
        case '/': in_op("÷");    break;
        case '%': in_percent();  break;
        case '(': in_lparen();   break;
        case ')': in_rparen();   break;
        case 'N': in_sign();     break;
        case 'B': in_back();     break;
        case 'E': in_eq();       break;
        case 'C':
            s_expr[0] = '\0';
            s_hist[0] = '\0';
            s_done = 0;
            s_err  = 0;
            break;
        default: break;
        }
        seq++;
    }
}

/* 把当前算式与结果渲染成给串口看的字符串 */
static void calc_snapshot(char *expr_line, int en, char *res_line, int rn)
{
    char   buf[CALC_EXPR_MAX];
    char   out[40];
    double v;

    /* 算式行 */
    if (s_done && s_hist[0])
    {
        rt_snprintf(expr_line, en, "%s =", s_hist);
    }
    else
    {
        cm_str_copy(expr_line, s_expr, en);
    }

    /* 结果行 */
    if (s_err)
    {
        cm_str_copy(res_line, "错误", rn);
        return;
    }
    if (s_expr[0] == '\0')
    {
        cm_str_copy(res_line, "0", rn);
        return;
    }
    res_line[0] = '\0';
    if (cm_preview_input(s_expr, buf, sizeof(buf)) && cm_eval(buf, &v) == 0)
    {
        cm_fmt(v, out, sizeof(out));
        cm_str_copy(res_line, out, rn);
    }
}

/* 按键序列 -> 结果行应显示什么（"" 表示显示 0，"-" 表示留空）
 *
 * 序列里每个字符就是一个键，注意两件事：
 *   1. `(` `)` `*` `/` 是**真的括号键和运算符键**，不是标点装饰。
 *      想表达「按那个 ± 键」要写裸的 N —— 写成 "5(N)E" 会被当成
 *      「5 × ( )」，括号里啥也没有，最后预览出个 5，测试就会假报错。
 *   2. 功能键：N = ±   B = ←(退格)   E = =   C = 清空
 */
static const calc_case_t key_cases[] =
{
    { "1+2E",           "3",          "最简单的一步" },
    { "12+34E",         "46",         "两位数" },
    { "2+3*4E",         "14",         "优先级：不是 20" },
    { "10-2*3E",        "4",          "优先级 + 减号" },
    { "100/8E",         "12.5",       "除法出小数" },
    { "0.1+0.2E",       "0.3",        "浮点尾巴要吃掉" },
    { "1/3E",           "0.333333",   "无限小数截到 6 位" },
    { "2*3.5E",         "7",          "小数乘法，末尾 0 要剪掉" },
    { "200*10%E",       "20",         "百分号：10% = 0.1" },
    { "50%E",           "0.5",        "裸百分号" },
    { "2*(3+4)E",       "14",         "括号" },
    { "5NE",            "-5",         "± 变负数（裸数字）" },
    { "5NNE",           "5",          "± 按两次回正" },
    { "12+5NE",         "7",          "± 对算式中间的数也管用：12+(-5)" },
    { "5+NE",           "5",          "末尾是运算符时 ± 忽略（不产生半截算式）" },
    { "9+9E1",          "1",          "= 之后按数字重新开始" },
    { "9+9E+1E",        "19",         "= 之后按运算符接着算" },
    { "12B",            "1",          "退格删一位" },
    { "12*B",           "12",         "退格要把 × 整块删掉" },
    { "5+*3E",          "15",         "连按两个运算符，后者替换前者" },
    { "5*-3E",          "-15",        "× 后面的减号当负号" },
    { "5+-3E",          "2",          "减号按两次 = 替换，不是叠加" },
    { "5-3NE",          "8",          "± 插负号：5-(-3) = 8" },
    { "1/0E",           "错误",       "除以零" },
    { "5/1000E",        "0.005",      "小数值：mV 换 V" },
    { "1/(1/1000+1/1000)E", "500",    "两层括号：并联电阻 1000//1000" },
    { "C",              "0",          "清空" },
    { "12+",            "12",         "没写完也能预览" },
    { "2*(3+4",         "14",         "没配平的括号能预览" },
    { "5(",             "5",          "5( 自动补乘号" },
    { "1+2)",           "3",          "多余的右括号被忽略" },
    { "0.1+0.2",        "0.3",        "不按 = 也有实时预览" },
    { ".5",             "0.5",        "小数点开头补 0" },
    { "007",            "7",          "前导 0 被吃掉" },
    { "1/3*3E",         "1",          "1÷3×3 回到 1" },
};

static int calc_test(int argc, char **argv)
{
    int i, pass = 0, fail = 0;
    char ex[CALC_EXPR_MAX + 8];
    char rs[40];

    rt_kprintf("calc: ===== input + arithmetic self test =====\n");

    for (i = 0; i < (int)(sizeof(key_cases) / sizeof(key_cases[0])); i++)
    {
        calc_run_seq(key_cases[i].seq);
        calc_snapshot(ex, sizeof(ex), rs, sizeof(rs));

        if (strcmp(rs, key_cases[i].want) == 0)
        {
            pass++;
            rt_kprintf("calc: [ ok ] %-12s -> %-10s (%s)\n",
                       key_cases[i].seq, rs, key_cases[i].note);
        }
        else
        {
            fail++;
            rt_kprintf("calc: [FAIL] %-12s -> got \"%s\" want \"%s\"  (%s)\n",
                       key_cases[i].seq, rs, key_cases[i].want, key_cases[i].note);
        }
    }

    rt_kprintf("calc: ===== %d passed, %d failed =====\n", pass, fail);

    return (fail == 0) ? 0 : -1;
}

MSH_CMD_EXPORT(calc_test, calculator input and arithmetic self test);

/* 不用手点也能验证界面：直接把一个算式喂进去再打印两行字的内容 */
static int calc_demo(int argc, char **argv)
{
    const char *seq = (argc > 1) ? argv[1] : "2+3*4E";
    char ex[CALC_EXPR_MAX + 8];
    char rs[40];

    calc_run_seq(seq);
    calc_snapshot(ex, sizeof(ex), rs, sizeof(rs));

    rt_kprintf("calc: demo \"%s\" -> expr_line=\"%s\"  result_line=\"%s\"\n",
               seq, ex, rs);

    return 0;
}

MSH_CMD_EXPORT(calc_demo, calc_demo <key sequence> : run keys and dump both display lines);


/* ----------------------------------------------------------------
 * 主菜单自检：这个 app 到底会不会出现在蜂窝里？
 *
 * 「图标不是 NULL 才会被画出来」这件事，光看自己的代码是证明不了的 ——
 * 它取决于链接进 BuiltinAppTab 段的顺序、以及 icon 指针有没有真的解析到。
 * 所以这条命令直接遍历框架自己的内置 app 列表，把每个 app 的
 * id / icon 指针 / 落到蜂窝第几格 打印出来，cell 的算法完全复刻
 * app_mainmenu.c 的 reorder_clock_icon（clock 钉在第 0 格，Main 不占格）。
 *
 * 用法： calc_menu
 * ---------------------------------------------------------------- */
static int calc_menu(int argc, char **argv)
{
    const builtin_app_desc_t *app;
    int idx = 0;                /* 主菜单分配游标 */
    int n = 0, found = 0, calc_cell = -1;

    rt_kprintf("calc: ===== builtin app list (what the honeycomb will draw) =====\n");

    for (app = gui_builtin_app_list_open(); app; app = gui_builtin_app_list_get_next(app))
    {
        int         cell = -1;
        const char *note;

        if (0 == strcmp(app->id, "clock"))
        {
            cell = 0;                       /* 固定第 0 格 */
            note = "pinned to cell 0";
        }
        else if (0 == strcmp(app->id, "Main"))
        {
            /* 主菜单自己不占格（app_mainmenu.c 里跳过的是它的 APP_ID） */
            note = "this is the menu itself";
        }
        else if (app->icon)
        {
            if (idx == 0)
            {
                idx = 1;                    /* 第 0 格被 clock 占了 */
            }
            cell = idx++;
            note = "drawn";
        }
        else
        {
            note = "SKIPPED (icon == NULL)"; /* 蜂窝里看不到 —— 最常见的坑 */
        }

        if (0 == strcmp(app->id, APP_ID))
        {
            found     = 1;
            calc_cell = cell;
        }

        if (cell >= 0)
        {
            rt_kprintf("calc: cell %2d  id=%-16s icon=%p  %s\n",
                       cell, app->id, app->icon, note);
        }
        else
        {
            rt_kprintf("calc: cell  -  id=%-16s icon=%p  %s\n",
                       app->id, app->icon, note);
        }
        n++;
    }

    rt_kprintf("calc: %d builtin apps  ->  calculator %s, cell=%d\n",
               n, found ? "FOUND" : "MISSING (!!)", calc_cell);

    return found ? 0 : -1;
}

MSH_CMD_EXPORT(calc_menu, list builtin apps and which honeycomb cell each one lands on);

#endif /* RT_USING_FINSH */
