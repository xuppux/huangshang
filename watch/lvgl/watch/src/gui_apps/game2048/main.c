/*
 * 2048 App（game2048）
 * ===================
 *
 * 标准的「内置应用」，和 calculator / torch 一样通过 BUILTIN_APP_EXPORT
 * 挂到主菜单的蜂窝里：
 *
 *   BUILTIN_APP_EXPORT(显示名, 图标, app id, 入口函数)
 *
 * 图标资源符号 img_game2048 由构建脚本从这张 PNG 自动生成：
 *   src/resource/images/common/ezip/img_game2048.png
 * （ezip/ 下每张 PNG 都会自动编成同名 lv_img_dsc_t，不用改任何构建脚本）
 *
 * ⚠ 主菜单只显示「icon 不是 NULL」的 app。图标传 NULL → 代码全对、编译也过，
 *   但蜂窝里就是没有这一格。这是最容易白折腾半天的坑。
 *
 *
 * 一、设计取舍（为什么是现在这个样子）
 * ----------------------------------
 *
 * 1. **配色用 2048 官方那套暖木色，不跟计算器的深色。**
 *    工程里本来就各 app 各取色（计算器深色、手电筒纯白）。2048 这副牌面
 *    （米底 + 灰棕棋盘 + 橙黄阶梯）是它辨识度的一部分，换深色反而认不出来。
 *    文字色按底色深浅分两档（浅底深字 / 暖底米白字），这是原版的做法 ——
 *    自己乱配会出现「64 上看不清数字」。
 *
 * 2. **滑动自己算，不用 LV_EVENT_GESTURE。**
 *    GESTURE 要求滚动对象参与、有一堆前置条件，链路上一环被别的对象插一脚
 *    就静默失效（和计算器里弃用 LV_EVENT_CLICKED 是同一个理由）。这里直接收
 *    PRESSED / RELEASED 自己比位移和时长 —— 每次滑动都有一行日志，
 *    「划了没反应」能一眼看出是没到 root，还是逻辑没走。
 *
 * 3. **命中层做得很薄：棋盘和 16 个格子全部 clear CLICKABLE / SCROLLABLE。**
 *    只有 root 可点。这样指头落哪儿都命中 root，不用做坐标换算去猜是哪个格子，
 *    也不会出现「某个格子把滑动吃掉」的怪事。
 *
 * 4. **只在「值变了」的格子上刷文本。** 16 格全刷要走 16 次
 *    lv_ext_set_local_font（freetype 本地字体），每格记住上次画的值，没变就跳过。
 *
 * 5. **有撤销，但只有一步。** 一步足够救手滑；再多就要存状态栈，不值当。
 *
 *
 * 二、⚠ 两个在别的 app 里踩过、这里必须避开的坑
 * -------------------------------------------
 *   · **顶部 34px 是上划手势条，右边缘 x>=374 是左滑返回条。**
 *     棋盘右边界必须 < 374，否则最右一列往右划会变成「退出 app」。
 *     本文件 BOARD_X=33 / BOARD_W=324 → 右边界 357，留 17px 余量，
 *     并且用 #if 在编译期就把这条卡死。
 *   · **防息屏定时器必须在 on_pause / on_stop 里删掉。** 2048 恰恰是
 *     「盯着屏幕想半天」的用法，不挡就是 10 秒后自己黑屏。
 *   · **label 占的高度是「行高」，不是字号。** freetype 下 SDK 把它写死成
 *     `size + size/3 + 4`（见 lvsf_font.c 的 lv_freetype_set_font_size），
 *     16px 的 label 实际高 25px、20px 的实际高 30px —— 都比字号大一圈。
 *     按「字号」去给面板留位置，汉字小标题和数字就会叠在一起。
 *     （这一条是**真机上撞出来的**：面板 46px 高塞 25+30=55px 的两行字。
 *       现在行高提成宏、面板高度由行高算出来，塞不下在编译期就报错。）
 *
 * 依赖：只用到 LVGL 和图元绘制，不需要任何运行时资源（除主菜单那个图标）。
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
 *  屏幕 390x450。顶部 34px 让给上划手势条，右边缘 x>=374 让给左滑返回条。
 * ================================================================ */
#define N           4
#define GAP         8
#define TILE        71
#define BOARD_W     (TILE * N + GAP * (N + 1))                  /* 71*4+8*5 = 324 */
#define BOARD_X     ((LV_HOR_RES_MAX - BOARD_W) / 2)            /* 33 -> 右边界 357 */
/* ⚠ BOARD_Y **不在这里** —— 它由顶部信息条算出来，见下面 BAND_GAP 那一段。
 *   手填一个 89 曾经让"面板底边和棋盘顶边严丝合缝贴住"活了好几天：
 *   两个都是对的数，但中间一个像素的缝都没有。*/

/* ── 字体行高 ─────────────────────────────────────────────────────
 * ⚠ 这是本 app 唯一一次真机 UI 事故的根子，改字号前先读这段。
 *
 * lv_ext_set_local_font(obj, FONT_X, color) 在 freetype 下给出的字体，
 * 它的 line_height **不等于** 字号那个数。SDK 里是一条写死的公式：
 *
 *     sifli-sdk/middleware/lvgl/lvsf/lvsf_font.c
 *     void lv_freetype_set_font_size(lv_font_t *font, uint16_t size)
 *     {
 *         font->line_height = size + size * 1 / 3 + 4;
 *         font->base_line   = size * 1 / 3;
 *     }
 *
 * 于是一个 label 实际占的高度：
 *     FONT_SMALL (16) -> 25px      FONT_NORMAL (20) -> 30px
 *     FONT_SUBTITLE(24)-> 36px     FONT_TITLE  (28) -> 41px     FONT_BIGL (36) -> 52px
 *
 * 第一版把「分数 / 最佳」面板固定成 46 高（PANEL_H = TOP_H = 46），
 * 标题用 LV_ALIGN_TOP_MID +5、数值用 LV_ALIGN_BOTTOM_MID -5，
 * 两块文本可用高度只有 46-10 = 36px，却要塞 25 + 30 = 55px 的字 ——
 * 真机上就是汉字小标题和分数叠在一起。
 *
 * 现在：行高写成宏，面板高度由行高算出来，谁也别再手填数字。 */
#define LINE_H(size) ((size) + (size) / 3 + 4)

/* ⚠ #if 在预处理期看不见 enum（FONT_SMALL 会被当成 0），所以下面这几条
 *   编译期检查必须用字面量。这几个字面量由编译器再校一次是否和真实字号
 *   对得上 —— 对不上就编译报错，不会悄悄放过去。 */
#define LH_CAPTION  25      /* FONT_SMALL 16 -> 16 + 16/3 + 4 = 25 */
#define LH_VALUE    30      /* FONT_NORMAL 20 -> 20 + 20/3 + 4 = 30 */

typedef char lh_caption_check[(LH_CAPTION == LINE_H(FONT_SMALL))  ? 1 : -1];
typedef char lh_value_check  [(LH_VALUE   == LINE_H(FONT_NORMAL)) ? 1 : -1];

/* ── 顶部信息条：两个「小标题 + 数值」的面板，并排 + 整体水平居中 ──
 *
 * 2026-09-23 真机反馈第二轮：「分数框和最佳框底部和方格框重叠了」。
 * 当时两个面板挤在右侧（x=189 / 277），底边 88 和棋盘顶边 89
 * **余量正好 0** —— 两个色块直接贴在一起，看着就是面板压在棋盘上。
 * 而它们之所以只能挤在右边，是因为左上角还顶着一个「2048」标题
 * （FONT_TITLE 28 -> 行高 41px），左边一整块都被它占了。
 *
 * 用户建议：「可以不要左上角 2048 字样，让那两个框顶部居中」—— 采纳。
 *
 * 现在：没有标题，两个面板整体居中；**棋盘位置由信息条算出来**
 * （BOARD_Y = TOP_Y + PANEL_H + BAND_GAP），改一个数另一个自己跟着走。
 * 也就是说"面板和棋盘又贴到一起"这件事，从构造上就不可能再发生了。*/
#define TOP_Y       28          /* 面板顶边（34 -> 28：往上让出和棋盘的间隙）*/
#define PANEL_W     80
#define PANEL_GAP   8           /* 两个面板之间的缝 */
#define PANEL_H     (LH_CAPTION + LH_VALUE)                      /* 25 + 30 = 55 */

#define PANEL_TOTAL (PANEL_W * 2 + PANEL_GAP)                    /* 168 */
#define PANEL_X0    ((LV_HOR_RES_MAX - PANEL_TOTAL) / 2)         /* (390-168)/2 = 111 */
#define PANEL_X1    (PANEL_X0 + PANEL_W + PANEL_GAP)             /* 199 -> 右边界 278 */

#define CAP_Y       0                       /* 小标题：贴着面板顶 */
#define VAL_Y       LH_CAPTION              /* 数值：紧接小标题下面（25）*/

#define BAND_GAP    6           /* 信息条底边 -> 棋盘顶边的可见间隙（>= 4 才不显得贴住）*/
#define BOARD_Y     (TOP_Y + PANEL_H + BAND_GAP)                 /* 28+55+6 = 89，没动 */

#define BOT_Y       416                                         /* 底部按钮条 */
#define BOT_H       30
#define BTN_W       84
#define BTN_GAP     12

#define RIGHT_BAR_X 374     /* 右边缘左滑返回条的起点（实测 layer_top 16px 宽） */

/* 编译期就把这几条卡死 —— 出错时是编译错误，不是"用着才发现不对劲" */

/* 信息条和棋盘之间**必须留出看得见的缝**。
 * 余量 0（改这次之前就是）在真机上读起来就是"面板压在棋盘上" ——
 * 两块同色系（#BBADA0）的方块贴在一起，边线直接糊成一条。 */
#if (BAND_GAP < 4)
#error "less than 4px between the score panels and the board: they will look glued together"
#endif
/* 顶部信息条必须整条塞在棋盘上方。"塞不下"的表现就是标题文字和数字重叠，
 * 所以这里直接把它变成编译错误，而不是靠肉眼看。 */
#if (TOP_Y + PANEL_H) > BOARD_Y
#error "top info band does not fit above the board: caption/value labels will overlap"
#endif
/* 信息条不能顶到屏幕最上沿（那里是上划手势条），也不能左右出界 */
#if (TOP_Y < 12)
#error "top info band is too close to the screen edge (the top swipe-down gesture strip lives there)"
#endif
#if (PANEL_X0 < 20)
#error "score panels overlap the left-edge gesture bar"
#endif
#if ((PANEL_X1 + PANEL_W) > RIGHT_BAR_X)
#error "score panels overlap the right-edge swipe-back bar"
#endif
/* 两个面板必须真的水平居中（整对的中点 = 屏幕中点）*/
#if ((PANEL_X0 + PANEL_X1 + PANEL_W - 1) / 2 != (LV_HOR_RES_MAX - 1) / 2)
#error "score panels are not horizontally centered"
#endif
/* 面板里两块文本区不能交叠 */
#if (CAP_Y + LH_CAPTION) > VAL_Y
#error "score panel: caption and value text boxes overlap"
#endif
#if (VAL_Y + LH_VALUE) > PANEL_H
#error "score panel: value text overflows the panel bottom"
#endif
#if (BOARD_X + BOARD_W) >= RIGHT_BAR_X
#error "board overlaps the right-edge swipe-back bar (x >= 374)"
#endif
#if (BOARD_Y + BOARD_W) > BOT_Y
#error "board overlaps the bottom button bar"
#endif
#if (BOARD_X < 20)
#error "board overlaps the left-edge gesture bar (x < 20)"
#endif


/* ================================================================
 *  二、配色（2048 官方牌面）
 * ================================================================ */
#define C_PAGE      lv_color_make(0xFA, 0xF8, 0xEF)   /* 页面：米白 */
#define C_BOARD     lv_color_make(0xBB, 0xAD, 0xA0)   /* 棋盘底：灰棕 */
#define C_EMPTY     lv_color_make(0xCD, 0xC1, 0xB4)   /* 空格子 */
#define C_DARKTEXT  lv_color_make(0x77, 0x6E, 0x65)   /* 浅底上的字 */
#define C_LIGHTTEXT lv_color_make(0xF9, 0xF6, 0xF2)   /* 暖底上的字 */
#define C_LABEL     lv_color_make(0xEE, 0xE4, 0xDA)   /* 分数面板的小标题 */
#define C_INK       lv_color_make(0x77, 0x6E, 0x65)   /* 页面上的深字 */
#define C_BTN       lv_color_make(0x8F, 0x7A, 0x66)   /* 按钮 */
#define C_MASK      lv_color_make(0xFA, 0xF8, 0xEF)   /* 结束/达成 的遮罩 */

typedef struct
{
    int         v;
    rt_uint32_t rgb;
    int         dark_text;      /* 1 = 用深字 */
} tile_style_t;

static const tile_style_t TILE_STYLE[] =
{
    {    2, 0xEEE4DA, 1 },
    {    4, 0xEDE0C8, 1 },
    {    8, 0xF2B179, 0 },
    {   16, 0xF59563, 0 },
    {   32, 0xF67C5F, 0 },
    {   64, 0xF65E3B, 0 },
    {  128, 0xEDCF72, 0 },
    {  256, 0xEDCC61, 0 },
    {  512, 0xEDC850, 0 },
    { 1024, 0xEDC53F, 0 },
    { 2048, 0xEDC22E, 0 },
};
#define TILE_STYLE_N  ((int)(sizeof(TILE_STYLE) / sizeof(TILE_STYLE[0])))

#define RGB_SUPER   0x3C3A32        /* >2048 之后的兜底色 */
#define WIN_VALUE   2048


/* ================================================================
 *  三、棋盘数据与算法
 *
 *  棋盘存的是**数值**（不是 2 的幂指数）：2/4/8... 直接可读，串口 dump
 *  出来人能看懂，也不用到处写 1<<x。内存代价 int[4][4]=64B，无所谓。
 * ================================================================ */
static int          s_board[N][N];
static int          s_snap[N][N];       /* 撤销用的一步快照 */
static rt_uint32_t  s_score      = 0;
static rt_uint32_t  s_best       = 0;
static rt_uint32_t  s_snap_score = 0;
static rt_uint32_t  s_moves      = 0;
static int          s_over       = 0;   /* 已经不能动了 */
static int          s_won        = 0;   /* 达到过 2048 */
static int          s_continued  = 0;   /* 达成后选了「继续玩」，别再弹遮罩 */
static int          s_snap_valid = 0;   /* 有可撤销的一步 */
static int          s_mask_mode  = 0;   /* 0=无 1=达成 2=结束 */
static int          s_spawn_on   = 1;   /* 0 = 不自动生成新块（串口摆残局用） */

/* 随机数：自带 xorshift32，不依赖 libc 的 rand（省得纠结有没有被裁掉） */
static rt_uint32_t  s_rng = 0x1234567u;

static rt_uint32_t rng_next(void)
{
    rt_uint32_t x = s_rng;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng = x;
    return x;
}

/* 按位数选字号。格子 71px 宽，得让 6 位数（131072）也放得下：
 *   1-2 位 -> 36px   3 位 -> 28px   4 位 -> 24px   5 位 -> 20px   6 位 -> 16px
 * ⚠ 这些是估过的：36px 数字宽约 20px，"64" 占 40px < 71 能进；24px 的
 *   "2048" 约 52px 也能进。6 位那档 16px 约 53px，勉强 —— 实际打不到那么高
 *   （要连出 16 个 2），纯粹兜底防止数字溢出画到隔壁格子上。 */
static int font_for(int v)
{
    if (v < 100)    return FONT_BIGL;       /* 36 */
    if (v < 1000)   return FONT_TITLE;      /* 28 */
    if (v < 10000)  return FONT_SUBTITLE;   /* 24 */
    if (v < 100000) return FONT_NORMAL;     /* 20 */
    return FONT_SMALL;                      /* 16 */
}

/* 一条线（4 格）向"前"压缩 + 合并。
 *
 * 规则：同一次滑动里每个块**最多参与一次合并** —— 这是 2048 的核心规则。
 * 这里靠「挪位 + 下标只走一格」来保证：
 *   · 合并后 out[i] 是结果，后面几格往前挪了一格；
 *   · 下标 +1 之后，比较的是 (i+1, i+2) 这一对**新邻居**，
 *     结果那颗停在 out[i]，再也不会被看上。
 *
 * ⚠ 两个都要避开的写法（都会算错，而且在屏幕上都只是"数字变大了"，看不出来）：
 *   · 合并后**下标不动** —— 刚合出来的 4 又被拿去和挪过来的 4 合：
 *       [2,2,4,0] -> [8,0,0,0]（对的是 [4,4,0,0]）
 *   · 合并后 **i += 2**  —— 看着更"保险"，其实跳过挪过来的那一对：
 *       [4,4,2,2] -> [8,2,2,0]（对的是 [8,4,0,0]）
 *       [4,4,4,4] -> [8,4,4,0]（对的是 [8,8,0,0]）
 *       [2,2,2,2] -> [4,2,2,0]（对的是 [4,4,0,0]）
 *     这版最早就是写的 i += 2，被 ui-preview/game2048.html 的自检抓出来的。
 * 返回 1 表示这一线发生了变化。 */
static int line_step(int *line, int *gained)
{
    int out[N];
    int i, j, n = 0, changed = 0;

    /* ① 非零的挤到前面 */
    for (i = 0; i < N; i++)
    {
        if (line[i])
        {
            out[n++] = line[i];
        }
    }
    while (n < N)
    {
        out[n++] = 0;
    }

    /* ② 相邻相同就合并 */
    i = 0;
    while (i < N - 1)
    {
        if (out[i] && out[i] == out[i + 1])
        {
            out[i] *= 2;
            *gained += out[i];
            if (out[i] >= WIN_VALUE && !s_won)
            {
                s_won       = 1;
                s_continued = 0;
            }
            for (j = i + 1; j < N - 1; j++)     /* 后面的往前挪一格 */
            {
                out[j] = out[j + 1];
            }
            out[N - 1] = 0;
        }
        i += 1;                                 /* ⚠ 就一格。理由见函数头 */
    }

    for (i = 0; i < N; i++)
    {
        if (line[i] != out[i])
        {
            changed = 1;
            line[i] = out[i];
        }
    }
    return changed;
}

/* 朝一个方向滑。dir: 0=上 1=右 2=下 3=左
 *
 * 做法：把每一行/列按滑动方向抽成一条"从后往前"的线，交给 line_step()
 * 统一处理，再按同样的顺序写回去。四个方向共用同一段合并逻辑 ——
 * 分别写四遍是 bug 的温床。 */
static int board_slide(int dir)
{
    int i, j, line[N];
    int gained = 0, changed = 0;

    for (i = 0; i < N; i++)
    {
        for (j = 0; j < N; j++)
        {
            switch (dir)
            {
            case 0:  line[j] = s_board[j][i];             break;  /* 上 */
            case 2:  line[j] = s_board[N - 1 - j][i];     break;  /* 下 */
            case 3:  line[j] = s_board[i][j];             break;  /* 左 */
            default: line[j] = s_board[i][N - 1 - j];     break;  /* 右 */
            }
        }

        if (line_step(line, &gained))
        {
            changed = 1;
        }

        for (j = 0; j < N; j++)
        {
            switch (dir)
            {
            case 0:  s_board[j][i]           = line[j]; break;
            case 2:  s_board[N - 1 - j][i]   = line[j]; break;
            case 3:  s_board[i][j]           = line[j]; break;
            default: s_board[i][N - 1 - j]   = line[j]; break;
            }
        }
    }

    if (changed)
    {
        s_score += (rt_uint32_t)gained;
    }
    return changed;
}

/* 还有空位、或者相邻还能合并 */
static int moves_left(void)
{
    int r, c;

    for (r = 0; r < N; r++)
    {
        for (c = 0; c < N; c++)
        {
            if (!s_board[r][c])
            {
                return 1;
            }
            if (c + 1 < N && s_board[r][c] == s_board[r][c + 1])
            {
                return 1;
            }
            if (r + 1 < N && s_board[r][c] == s_board[r + 1][c])
            {
                return 1;
            }
        }
    }
    return 0;
}

/* 在随机空格里放一个 2（90%）或 4（10%） */
static void spawn_tile(void)
{
    int pos[N * N][2];
    int n = 0, r, c, k;

    for (r = 0; r < N; r++)
    {
        for (c = 0; c < N; c++)
        {
            if (!s_board[r][c])
            {
                pos[n][0] = r;
                pos[n][1] = c;
                n++;
            }
        }
    }
    if (!n)
    {
        return;
    }

    k = (int)(rng_next() % (rt_uint32_t)n);
    s_board[pos[k][0]][pos[k][1]] = (rng_next() % 10u == 0u) ? 4 : 2;
}

static int tile_count(void)
{
    int r, c, n = 0;

    for (r = 0; r < N; r++)
    {
        for (c = 0; c < N; c++)
        {
            if (s_board[r][c])
            {
                n++;
            }
        }
    }
    return n;
}

static int max_tile(void)
{
    int r, c, m = 0;

    for (r = 0; r < N; r++)
    {
        for (c = 0; c < N; c++)
        {
            if (s_board[r][c] > m)
            {
                m = s_board[r][c];
            }
        }
    }
    return m;
}


/* ================================================================
 *  四、界面
 * ================================================================ */
static lv_obj_t *g_root    = NULL;
static lv_obj_t *g_board   = NULL;
static lv_obj_t *g_tile[N][N];
static lv_obj_t *g_tile_lb[N][N];
static int       g_shown[N][N];         /* 每格上次画的值，-1 = 需要重画 */

static lv_obj_t *g_lb_score = NULL;     /* 「分数」的数值 */
static lv_obj_t *g_lb_best  = NULL;     /* 「最佳」的数值 */
static lv_obj_t *g_cap_score = NULL;    /* 「分数」这两个汉字（几何自检用）*/
static lv_obj_t *g_cap_best  = NULL;    /* 「最佳」这两个汉字 */

static lv_obj_t *g_mask    = NULL;      /* 结束 / 达成 的遮罩 */
static lv_obj_t *g_mask_t1 = NULL;
static lv_obj_t *g_mask_t2 = NULL;
static lv_obj_t *g_mask_ok = NULL;      /* 主按钮：继续玩 / 再来一局 */
static lv_obj_t *g_mask_ok_lb = NULL;
static lv_obj_t *g_mask_no = NULL;      /* 副按钮：重开（仅"达成"时显示） */
static lv_obj_t *g_mask_no_lb = NULL;

static lv_timer_t *g_keep = NULL;       /* 防息屏定时器（⚠ 是 lv_timer_t，不是 lv_obj_t） */

static rt_uint32_t s_start_cnt = 0;

/* 触摸记账（和 torch 同一套：让"划了没反应"可判） */
static rt_uint32_t s_evt_down = 0;
static rt_uint32_t s_evt_up   = 0;
static rt_uint32_t s_evt_sw   = 0;      /* 认出来的滑动次数 */
static lv_point_t  s_press_pt = { 0, 0 };
static rt_uint32_t s_press_tk = 0;
static int         s_pressed  = 0;
static int         s_evt_log  = 1;

#define SWIPE_MIN_PX    26              /* 小于这个位移不算滑动 */
#define SWIPE_MAX_MS    1000            /* 超过这么久的拖拽不算滑动（防误触） */

static const char *dim_name(int d)
{
    switch (d)
    {
    case 0:  return "up";
    case 1:  return "right";
    case 2:  return "down";
    default: return "left";
    }
}

/* 解析方向参数：u/0=上 r/1=右 d/2=下 l/3=左（"down"/"left"/"up"/"right" 都吃） */
static int parse_dir(const char *s)
{
    if (!s)
    {
        return -1;
    }
    switch (s[0])
    {
    case '0': case 'u': case 'U': return 0;
    case '1': case 'r': case 'R': return 1;
    case '2': case 'd': case 'D': return 2;
    case '3': case 'l': case 'L': return 3;
    default: return -1;
    }
}

static void tile_look(int v, lv_color_t *bg, lv_color_t *fg, int *font)
{
    int i;

    if (v <= 0)
    {
        *bg   = C_EMPTY;
        *fg   = C_EMPTY;
        *font = FONT_SMALL;
        return;
    }

    for (i = 0; i < TILE_STYLE_N; i++)
    {
        if (TILE_STYLE[i].v == v)
        {
            *bg   = lv_color_make((TILE_STYLE[i].rgb >> 16) & 0xFF,
                                  (TILE_STYLE[i].rgb >> 8) & 0xFF,
                                  TILE_STYLE[i].rgb & 0xFF);
            *fg   = TILE_STYLE[i].dark_text ? C_DARKTEXT : C_LIGHTTEXT;
            *font = font_for(v);
            return;
        }
    }

    /* >2048：深底米字兜底 */
    *bg   = lv_color_make((RGB_SUPER >> 16) & 0xFF,
                          (RGB_SUPER >> 8) & 0xFF,
                          RGB_SUPER & 0xFF);
    *fg   = C_LIGHTTEXT;
    *font = font_for(v);
}

static void refresh_tiles(void)
{
    int r, c;

    if (!g_root)
    {
        return;
    }

    for (r = 0; r < N; r++)
    {
        for (c = 0; c < N; c++)
        {
            lv_color_t bg, fg;
            int        font;
            char       buf[16];

            /* 值没变就整格跳过：16 格全刷要走 16 次 lv_ext_set_local_font */
            if (g_shown[r][c] == s_board[r][c])
            {
                continue;
            }
            g_shown[r][c] = s_board[r][c];

            tile_look(s_board[r][c], &bg, &fg, &font);
            lv_obj_set_style_bg_color(g_tile[r][c], bg,
                                      LV_PART_MAIN | LV_STATE_DEFAULT);

            if (s_board[r][c] <= 0)
            {
                lv_label_set_text(g_tile_lb[r][c], "");
                continue;
            }
            rt_snprintf(buf, sizeof(buf), "%d", s_board[r][c]);
            lv_label_set_text(g_tile_lb[r][c], buf);
            lv_ext_set_local_font(g_tile_lb[r][c], font, fg);
        }
    }
}

static void refresh_hud(void)
{
    char buf[40];

    if (!g_root)
    {
        return;
    }

    if (g_lb_score)
    {
        rt_snprintf(buf, sizeof(buf), "%u", (unsigned)s_score);
        lv_label_set_text(g_lb_score, buf);
    }
    if (g_lb_best)
    {
        rt_snprintf(buf, sizeof(buf), "%u", (unsigned)s_best);
        lv_label_set_text(g_lb_best, buf);
    }
    /* 这里原来还有一行「第 N 步 · 撤销可回上一步」的底部状态文字。
     * 去掉了：一是没必要（撤销按钮就在旁边），二是它 align 到 BOT_Y-26 = 390，
     * 而棋盘占 89..412 —— 那行字本来就压在棋盘最后一行格子上。 */
}

/* 遮罩：0=隐藏 1=达成 2048 2=无路可走 */
static void refresh_mask(void)
{
    char buf[40];

    if (!g_mask)
    {
        return;
    }

    if (s_mask_mode == 0)
    {
        lv_obj_add_flag(g_mask, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (s_mask_mode == 1)
    {
        lv_label_set_text(g_mask_t1, "达成 2048！");
        rt_snprintf(buf, sizeof(buf), "得分 %u", (unsigned)s_score);
        lv_label_set_text(g_mask_t2, buf);
        lv_label_set_text(g_mask_ok_lb, "继续玩");
        lv_obj_clear_flag(g_mask_no, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(g_mask_no_lb, "重开");
    }
    else
    {
        lv_label_set_text(g_mask_t1, "没法再动了");
        rt_snprintf(buf, sizeof(buf), "得分 %u · 最大 %d",
                    (unsigned)s_score, max_tile());
        lv_label_set_text(g_mask_t2, buf);
        lv_label_set_text(g_mask_ok_lb, "再来一局");
        lv_obj_add_flag(g_mask_no, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_clear_flag(g_mask, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_mask);
}

static void refresh_all(void)
{
    refresh_tiles();
    refresh_hud();
    refresh_mask();
}


/* ── 游戏动作 ─────────────────────────────────────────────────── */
static void new_game(void)
{
    memset(s_board, 0, sizeof(s_board));
    memset(s_snap, 0, sizeof(s_snap));
    s_score      = 0;
    s_snap_score = 0;
    s_moves      = 0;
    s_over       = 0;
    s_won        = 0;
    s_continued  = 0;
    s_snap_valid = 0;
    s_mask_mode  = 0;

    /* 换一局就重新播种，不然每次开局的块位置完全一样 */
    s_rng = rt_tick_get() * 2654435761u + 1u;

    spawn_tile();
    spawn_tile();

    rt_kprintf("2048: new game seed=0x%08X tiles=%d\n",
               (unsigned)s_rng, tile_count());
    refresh_all();
}

static void do_move(int dir)
{
    int         tmp[N][N];
    rt_uint32_t ts;

    if (s_mask_mode || s_over)
    {
        return;
    }

    memcpy(tmp, s_board, sizeof(tmp));
    ts = s_score;

    if (!board_slide(dir))
    {
        /* 没变：把快照状态**保持原样**，别把上一步的有效快照冲掉 */
        memcpy(s_board, tmp, sizeof(tmp));
        s_score = ts;
        if (s_evt_log)
        {
            rt_kprintf("2048: move %s -> no change\n", dim_name(dir));
        }
        return;
    }

    memcpy(s_snap, tmp, sizeof(tmp));
    s_snap_score = ts;
    s_snap_valid = 1;
    s_moves++;

    if (s_spawn_on)
    {
        spawn_tile();
    }
    if (s_score > s_best)
    {
        s_best = s_score;
    }

    if (s_won && !s_continued)
    {
        s_mask_mode = 1;                    /* 首次达成 2048 */
    }
    else if (!moves_left())
    {
        s_over      = 1;
        s_mask_mode = 2;
    }

    if (s_evt_log)
    {
        rt_kprintf("2048: move %s -> ok moves=%u score=%u tiles=%d max=%d over=%d\n",
                   dim_name(dir), (unsigned)s_moves, (unsigned)s_score,
                   tile_count(), max_tile(), s_over);
    }
}

static void do_undo(void)
{
    if (!s_snap_valid)
    {
        rt_kprintf("2048: undo -> nothing to undo\n");
        return;
    }

    memcpy(s_board, s_snap, sizeof(s_board));
    s_score      = s_snap_score;
    s_snap_valid = 0;
    s_over       = 0;
    /* 撤销后如果还停在"达成"遮罩上就留着，否则一律收掉 */
    if (s_mask_mode == 2)
    {
        s_mask_mode = 0;
    }
    if (s_moves)
    {
        s_moves--;
    }

    rt_kprintf("2048: undo -> moves=%u score=%u tiles=%d\n",
               (unsigned)s_moves, (unsigned)s_score, tile_count());
    refresh_all();
}


/* ── 触摸：自己认滑动 ───────────────────────────────────────────── */
static void root_event_cb(lv_event_t *e)
{
    lv_event_code_t code  = lv_event_get_code(e);
    lv_indev_t     *indev = lv_indev_get_act();
    lv_point_t      p     = { 0, 0 };

    if (indev)
    {
        lv_indev_get_point(indev, &p);
    }

    switch (code)
    {
    case LV_EVENT_PRESSED:
        s_pressed  = 1;
        s_press_pt = p;
        s_press_tk = rt_tick_get();
        s_evt_down++;
        if (s_evt_log)
        {
            rt_kprintf("2048: press x=%d y=%d\n", (int)p.x, (int)p.y);
        }
        break;

    case LV_EVENT_RELEASED:
    {
        lv_coord_t  dx, dy;
        rt_uint32_t dt;

        if (!s_pressed)
        {
            break;
        }
        s_pressed = 0;
        s_evt_up++;

        dx = (p.x > s_press_pt.x) ? (p.x - s_press_pt.x) : (s_press_pt.x - p.x);
        dy = (p.y > s_press_pt.y) ? (p.y - s_press_pt.y) : (s_press_pt.y - p.y);
        dt = (rt_uint32_t)(rt_tick_get() - s_press_tk) * 1000u / RT_TICK_PER_SECOND;

        if (s_evt_log)
        {
            rt_kprintf("2048: release dx=%d dy=%d dt=%ums\n",
                       (int)dx, (int)dy, (unsigned)dt);
        }

        /* 位移够、时间够短才算滑动。横竖谁位移大就走谁 —— 斜着划也能出结果，
         * 比要求"纯横/纯竖"好用得多。 */
        if ((dx >= SWIPE_MIN_PX || dy >= SWIPE_MIN_PX) && dt < SWIPE_MAX_MS)
        {
            int dir;

            if (dx >= dy)
            {
                dir = (p.x > s_press_pt.x) ? 1 : 3;     /* 右 / 左 */
            }
            else
            {
                dir = (p.y > s_press_pt.y) ? 2 : 0;     /* 下 / 上 */
            }

            s_evt_sw++;
            if (s_evt_log)
            {
                rt_kprintf("2048: swipe -> %s\n", dim_name(dir));
            }
            do_move(dir);
            refresh_all();
        }
        break;
    }

    case LV_EVENT_PRESS_LOST:
        s_pressed = 0;
        break;

    default:
        break;
    }
}

static void keep_awake_cb(lv_timer_t *t)
{
    (void)t;
    /* 2048 是"停下来想"的游戏，不重置空闲计时就是 10 秒后黑屏。
     * 1s 周期对 10s 阈值有 10 倍余量。 */
    lv_disp_trig_activity(NULL);
}

/* ── 按钮 ─────────────────────────────────────────────────────── */
static void btn_undo_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }
    do_undo();
}

static void btn_new_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }
    new_game();
}

static void mask_ok_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }

    if (s_mask_mode == 1)
    {
        /* 达成后选择继续：收起遮罩，本局不再弹 */
        s_continued = 1;
        s_mask_mode = 0;
        rt_kprintf("2048: continue after win\n");
        refresh_all();
    }
    else
    {
        new_game();
    }
}

static void mask_no_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }
    new_game();
}

static lv_obj_t *mk_btn(lv_obj_t *par, const char *txt, int x, int y,
                        int w, int h, lv_event_cb_t cb, lv_obj_t **out_lbl)
{
    lv_obj_t *b = lv_obj_create(par);
    lv_obj_t *l;

    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_radius(b, 999, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(b, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(b, C_BTN, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    if (cb)
    {
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    }

    l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_ext_set_local_font(l, FONT_SMALL, C_LIGHTTEXT);
    lv_obj_center(l);
    /* 点字也等于点按钮 */
    lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);

    if (out_lbl)
    {
        *out_lbl = l;
    }
    return b;
}

/* 搭一个「小标题 + 数值」的面板。
 *
 * ⚠ 两块文本的位置必须用**顶对齐 + 各自的 y 偏移**，不能一块 TOP_MID、
 *   一块 BOTTOM_MID —— 那种写法在"以为字号就是高度"的前提下才成立，
 *   而行高比字号大一圈（见 LINE_H 那一段），结果就是两块叠在一起。
 *   这里 CAP_Y=0、VAL_Y=LINE_H(FONT_SMALL)=25，谁也不用猜另一块多高。 */
static void mk_value_panel(lv_obj_t *par, const char *cap, int x,
                           lv_obj_t **out_cap, lv_obj_t **out_val)
{
    lv_obj_t *box = lv_obj_create(par);
    lv_obj_t *c1, *c2;

    lv_obj_set_size(box, PANEL_W, PANEL_H);
    lv_obj_set_pos(box, x, TOP_Y);
    lv_obj_set_style_radius(box, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(box, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(box, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(box, C_BOARD, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    c1 = lv_label_create(box);
    lv_label_set_text(c1, cap);
    lv_ext_set_local_font(c1, FONT_SMALL, C_LABEL);
    lv_obj_align(c1, LV_ALIGN_TOP_MID, 0, CAP_Y);
    lv_obj_clear_flag(c1, LV_OBJ_FLAG_CLICKABLE);

    c2 = lv_label_create(box);
    lv_label_set_text(c2, "0");
    lv_ext_set_local_font(c2, FONT_NORMAL, C_LIGHTTEXT);
    lv_obj_align(c2, LV_ALIGN_TOP_MID, 0, VAL_Y);
    /* 分数只会越来越长：不换行、不自己撑高，免得把面板挤变形 */
    lv_label_set_long_mode(c2, LV_LABEL_LONG_CLIP);
    lv_obj_clear_flag(c2, LV_OBJ_FLAG_CLICKABLE);

    if (out_cap)
    {
        *out_cap = c1;
    }
    if (out_val)
    {
        *out_val = c2;
    }
}

/* 把一块面板的真实几何打出来，并判断小标题和数值有没有叠。
 *
 * ⚠ 打印的是 LVGL 算完的 coords（回显期望值等于没验）。
 *   gap = 数值顶边 - 小标题底边 - 1，<0 就是叠了。
 *   x 也一起打 —— 2026-09-23 第二轮真机反馈是"面板底部压在棋盘上"，
 *   改成居中之后，横向位置也得能被数字验，不能只看 y。
 *   这块只能靠数字验 —— 浏览器预览页虽然锁死了 line-height，
 *   但它终究是另一套排版引擎。 */
static int dump_panel_geom(const char *tag, lv_obj_t *cap, lv_obj_t *val)
{
    lv_obj_t *box = cap ? lv_obj_get_parent(cap) : NULL;
    int       gap = (cap && val) ? (val->coords.y1 - cap->coords.y2 - 1) : -999;
    int       ok  = (cap && val && val->coords.y1 > cap->coords.y2);

    rt_kprintf("2048: panel[%s] box x=%d..%d y=%d..%d | cap y=%d..%d h=%d | "
               "val y=%d..%d h=%d | gap=%d %s\n",
               tag,
               box ? box->coords.x1 : -1, box ? box->coords.x2 : -1,
               box ? box->coords.y1 : -1, box ? box->coords.y2 : -1,
               cap ? cap->coords.y1 : -1, cap ? cap->coords.y2 : -1,
               cap ? lv_obj_get_height(cap) : -1,
               val ? val->coords.y1 : -1, val ? val->coords.y2 : -1,
               val ? lv_obj_get_height(val) : -1,
               gap, ok ? "OK" : "OVERLAP");
    return ok;
}

/* 把所有布局事实一次打全。
 *
 * ⚠ 为什么要在**两个地方**调它（board_create 和 g2048_info）：
 *   框架在 app 已经"在运行列表里"时，gui_app_run 走的是 **Resume**，
 *   压根不会重新 create —— 于是"create 时那一次 dump"就不会出现。
 *   而烧进表里的固件必须能被**随时**验证，所以几何也要能从 shell 查。
 *   （2026-09-23 就是这么被咬的：第二次抓串口时 app 是 resume 进来的，
 *     一串几何断言因为"那一节里没有 dump"集体假失败。）
 *
 * ⚠ 只有 board_create 那条路径要先 lv_obj_update_layout()；
 *   从 shell 线程来的这次**不能再调**（LVGL 不是线程安全的，
 *   而且 create 之后 coords 早就稳定了）。
 */
static void dump_geometry(void)
{
    int ok1, ok2;

    if (!g_root || !g_board)
    {
        return;
    }

    rt_kprintf("2048: geom screen=%dx%d root=%dx%d board x=%d..%d y=%d..%d "
               "cell=%dx%d right_edge=%d\n",
               LV_HOR_RES_MAX, LV_VER_RES_MAX,
               lv_obj_get_width(g_root), lv_obj_get_height(g_root),
               g_board->coords.x1, g_board->coords.x2,
               g_board->coords.y1, g_board->coords.y2,
               lv_obj_get_width(g_tile[0][0]), lv_obj_get_height(g_tile[0][0]),
               BOARD_X + BOARD_W);
    rt_kprintf("2048: top band y=%d..%d (h=%d) board_top=%d gap=%d "
               "lh_cap=%d lh_val=%d band_gap_min=4\n",
               TOP_Y, TOP_Y + PANEL_H - 1, PANEL_H, BOARD_Y,
               BOARD_Y - (TOP_Y + PANEL_H), LH_CAPTION, LH_VALUE);

    /* 两个面板是不是真的水平居中（整对的中点 vs 屏幕中点）。
     * "顶部居中"是 2026-09-23 真机反馈明确要的效果，光看代码里的宏不算验证。 */
    if (g_cap_score && g_cap_best)
    {
        lv_obj_t *b1 = lv_obj_get_parent(g_cap_score);
        lv_obj_t *b2 = lv_obj_get_parent(g_cap_best);
        int       mid    = (b1->coords.x1 + b2->coords.x2) / 2;
        int       center = (LV_HOR_RES_MAX - 1) / 2;
        int       gapxy  = b2->coords.x1 - b1->coords.x2 - 1;

        rt_kprintf("2048: panels x=%d..%d and %d..%d | pair_mid=%d center=%d | "
                   "xgap=%d | %s\n",
                   b1->coords.x1, b1->coords.x2, b2->coords.x1, b2->coords.x2,
                   mid, center, gapxy,
                   (mid == center) ? "CENTERED" : "OFF-CENTER");
    }

    ok1 = dump_panel_geom("score", g_cap_score, g_lb_score);
    ok2 = dump_panel_geom("best",  g_cap_best,  g_lb_best);

    rt_kprintf("2048: score-panel overlap check: %s\n",
               (ok1 && ok2) ? "PASS" : "FAIL");
}

static void board_create(void)
{
    lv_obj_t *scr = lv_scr_act();
    int       r, c;

    lv_obj_set_style_bg_color(scr, C_PAGE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    g_root = lv_obj_create(scr);
    lv_obj_set_size(g_root, LV_HOR_RES_MAX, LV_VER_RES_MAX);
    lv_obj_set_pos(g_root, 0, 0);
    lv_obj_set_style_radius(g_root, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(g_root, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(g_root, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(g_root, C_PAGE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    /* ⚠ 必须清 SCROLLABLE：不然 LVGL 会把它选成 scroll_obj，
     *   滚动处理会插在 PRESSED/PRESSING 之间把事件抢走。 */
    lv_obj_clear_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_root, root_event_cb, LV_EVENT_ALL, NULL);

    /* ── 顶部：分数 / 最佳（并排、整体居中，没有标题）──
     *
     * 这里原来还有一个左上角的「2048」标题（FONT_TITLE，行高 41px）。
     * 它把左边整块都占了，两个面板只能挤在右侧 x=189/277 —— 也就因此
     * 面板底边 88 顶死在棋盘顶边 89 上，真机看着就是"压在棋盘上"。
     * 2026-09-23 按真机反馈去掉它：两个面板整体居中，左右对称。 */
    mk_value_panel(g_root, "分数", PANEL_X0, &g_cap_score, &g_lb_score);
    mk_value_panel(g_root, "最佳", PANEL_X1, &g_cap_best, &g_lb_best);

    /* ── 棋盘 ── */
    g_board = lv_obj_create(g_root);
    lv_obj_set_size(g_board, BOARD_W, BOARD_W);
    lv_obj_set_pos(g_board, BOARD_X, BOARD_Y);
    lv_obj_set_style_radius(g_board, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(g_board, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(g_board, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(g_board, C_BOARD, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(g_board, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    /* 棋盘本身不可点：事件要落到 root 上（见文件头设计取舍 3） */
    lv_obj_clear_flag(g_board, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g_board, LV_OBJ_FLAG_SCROLLABLE);

    for (r = 0; r < N; r++)
    {
        for (c = 0; c < N; c++)
        {
            lv_obj_t *cell = lv_obj_create(g_board);
            lv_obj_t *lb;

            lv_obj_set_size(cell, TILE, TILE);
            lv_obj_set_pos(cell, GAP + c * (TILE + GAP), GAP + r * (TILE + GAP));
            lv_obj_set_style_radius(cell, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(cell, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_all(cell, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(cell, C_EMPTY, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

            lb = lv_label_create(cell);
            lv_label_set_text(lb, "");
            lv_ext_set_local_font(lb, FONT_BIGL, C_DARKTEXT);
            lv_obj_center(lb);
            lv_obj_clear_flag(lb, LV_OBJ_FLAG_CLICKABLE);

            g_tile[r][c]    = cell;
            g_tile_lb[r][c] = lb;
            g_shown[r][c]   = -1;           /* -1 = 强制重画一次 */
        }
    }

    /* ── 底部：撤销 / 重开 ──
     * （这里原来还有一行「第 N 步 · 撤销可回上一步」的小字，已去掉。去掉它
     *   同时修掉一个布局错误：那行 align 到 BOT_Y-26 = 390，而棋盘占
     *   89..412，它其实压在棋盘最后一行格子上。） */
    {
        int total = BTN_W * 2 + BTN_GAP;
        int x0    = (LV_HOR_RES_MAX - total) / 2;

        mk_btn(g_root, "撤销", x0, BOT_Y, BTN_W, BOT_H, btn_undo_cb, NULL);
        mk_btn(g_root, "重开", x0 + BTN_W + BTN_GAP, BOT_Y, BTN_W, BOT_H,
               btn_new_cb, NULL);
    }

    /* ── 遮罩（默认隐藏）── */
    g_mask = lv_obj_create(g_root);
    lv_obj_set_size(g_mask, BOARD_W, BOARD_W);
    lv_obj_set_pos(g_mask, BOARD_X, BOARD_Y);
    lv_obj_set_style_radius(g_mask, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(g_mask, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(g_mask, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(g_mask, C_MASK, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(g_mask, LV_OPA_90, LV_PART_MAIN | LV_STATE_DEFAULT);
    /* 遮罩要**可点**：挡住底下的棋盘，免得结束之后还能划 */
    lv_obj_add_flag(g_mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g_mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_mask, LV_OBJ_FLAG_HIDDEN);

    g_mask_t1 = lv_label_create(g_mask);
    lv_label_set_text(g_mask_t1, "");
    lv_ext_set_local_font(g_mask_t1, FONT_TITLE, C_INK);
    lv_obj_align(g_mask_t1, LV_ALIGN_TOP_MID, 0, 74);
    lv_obj_clear_flag(g_mask_t1, LV_OBJ_FLAG_CLICKABLE);

    g_mask_t2 = lv_label_create(g_mask);
    lv_label_set_text(g_mask_t2, "");
    lv_ext_set_local_font(g_mask_t2, FONT_NORMAL, C_INK);
    lv_obj_align(g_mask_t2, LV_ALIGN_TOP_MID, 0, 118);
    lv_obj_clear_flag(g_mask_t2, LV_OBJ_FLAG_CLICKABLE);

    {
        int bw = 116, bh = 40, gapy = 12;
        int bx = (BOARD_W - bw) / 2;

        g_mask_ok = mk_btn(g_mask, "再来一局", bx, 178, bw, bh,
                           mask_ok_cb, &g_mask_ok_lb);
        g_mask_no = mk_btn(g_mask, "重开", bx, 178 + bh + gapy, bw, bh,
                           mask_no_cb, &g_mask_no_lb);
    }

    /* 几何自检：这一版最容易出的是"棋盘压到右边缘返回条 / 底部按钮条"，
     * 所以把关键几何打出来。
     * ⚠ 读 lv_obj_get_* 而不是宏（打印期望值等于没验），
     *   且必须先 update_layout —— 刚 create 完 coords 还是空的，
     *   不加这句打出来是 0x0，看着像"界面尺寸错了"。 */
    lv_obj_update_layout(g_root);
    dump_geometry();

    if (!g_keep)
    {
        g_keep = lv_timer_create(keep_awake_cb, 1000, NULL);
        rt_kprintf("2048: keep-awake timer %p 1000ms (idle screen-off is 10s)\n",
                   g_keep);
    }
}

/* on_stop 用：UI 被框架销毁了，这里的指针必须一起清掉，否则下一轮是野指针。
 * （棋盘数值刻意**不清** —— 退出再进来还能接着玩。） */
static void board_reset_ui(void)
{
    int r, c;

    g_root = NULL;
    g_board = NULL;
    g_lb_score = NULL;
    g_lb_best = NULL;
    g_cap_score = NULL;
    g_cap_best = NULL;
    g_mask = NULL;
    g_mask_t1 = NULL;
    g_mask_t2 = NULL;
    g_mask_ok = NULL;
    g_mask_ok_lb = NULL;
    g_mask_no = NULL;
    g_mask_no_lb = NULL;

    for (r = 0; r < N; r++)
    {
        for (c = 0; c < N; c++)
        {
            g_tile[r][c]    = NULL;
            g_tile_lb[r][c] = NULL;
            g_shown[r][c]   = -1;
        }
    }
}


/* ================================================================
 *  五、app 生命周期
 *
 *  框架会为每个 app 单独 create/load/del 一块屏幕，所以这里在 on_start 里
 *  往 lv_scr_act() 上画就行，on_stop 不用手动删控件。
 * ================================================================ */
static void on_start(void)
{
    s_start_cnt++;
    rt_kprintf("2048: start (#%u)\n", (unsigned)s_start_cnt);

    board_create();

    /* 有残局就接着下，没有才开新局 —— 退出去看一眼再回来不该从头开始 */
    if (tile_count() == 0)
    {
        new_game();
    }
    else
    {
        rt_kprintf("2048: resume in-progress game tiles=%d score=%u\n",
                   tile_count(), (unsigned)s_score);
        refresh_all();
    }
}

static void on_resume(void)
{
    rt_kprintf("2048: resume\n");

    /* pause 会把防息屏定时器删掉，这里补回来（幂等） */
    if (!g_keep)
    {
        g_keep = lv_timer_create(keep_awake_cb, 1000, NULL);
    }
    if (g_root)
    {
        refresh_all();
    }
}

static void on_pause(void)
{
    rt_kprintf("2048: pause\n");

    /* ⚠ 停表：不然息屏保护会在别的 app 前台时继续跑，把系统一直钉醒 */
    if (g_keep)
    {
        lv_timer_del(g_keep);
        g_keep = NULL;
    }
}

static void on_stop(void)
{
    rt_kprintf("2048: stop\n");

    if (g_keep)
    {
        lv_timer_del(g_keep);
        g_keep = NULL;
    }
    board_reset_ui();
}

static void msg_handler(gui_app_msg_type_t msg, void *param)
{
    (void)param;

    switch (msg)
    {
    case GUI_APP_MSG_ONSTART:  on_start();  break;
    case GUI_APP_MSG_ONRESUME: on_resume(); break;
    case GUI_APP_MSG_ONPAUSE:  on_pause();  break;
    case GUI_APP_MSG_ONSTOP:   on_stop();   break;
    default: break;
    }
}

LV_IMG_DECLARE(img_game2048);

#define APP_ID "game2048"

static int app_main(intent_t i)
{
    (void)i;
    gui_app_regist_msg_handler(APP_ID, msg_handler);
    return 0;
}

/* 图标不是 NULL 才会出现在主菜单的蜂窝里，这条务必别漏 */
BUILTIN_APP_EXPORT(LV_EXT_STR_ID(game2048), LV_EXT_IMG_GET(img_game2048),
                   APP_ID, app_main);


/* ================================================================
 *  六、串口自检
 *
 *  2048 的算法错法很隐蔽 —— 2 2 4 合成 8（错）和合成 4 4（对），
 *  在屏幕上都是"数字变大了"，肉眼根本看不出来。这类东西必须在串口上
 *  用确定的局面验一遍，所以这里有一整套：
 *
 *    g2048_info            棋盘 + 分数 + 状态 + 几何，一行行 dump
 *    g2048_set r c v       摆一块（配 g2048_spawn 0 用，做确定残局）
 *    g2048_move u|d|l|r    走一步，盯着返回值看合没合对
 *    g2048_spawn 0|1       关掉/打开随机生成（关掉才能复现）
 *    g2048_undo / g2048_new / g2048_clear
 *    g2048_seed n          固定随机种子（要"每次都一样"时用）
 *    g2048_auto n          随机走 n 步（压测用）
 *    g2048_ui              从串口直接进这个 app
 *
 *  两个必须验的残局（都关 spawn）：
 *    2 2 4 0 往左  ->  4 4 0 0    判"合并后下标不动"这个错
 *    4 4 2 2 0...  ->  8 4 0 0    判"合并后 i += 2"这个错（更隐蔽）
 *
 *  ⚠ 会碰 LVGL 的动作一律 lv_async_call 派回 GUI 线程 —— 在 shell 线程里
 *    改控件会撞 CHECK_CUR_RTOS_TASK() 断言（和 torch / swipe_sim 同一个理由）。
 *    只读的 g2048_info 可以直接调。
 *
 *  ⚠ MSH_CMD_EXPORT 的 desc 里不能有逗号 —— 会被预处理器当成宏的参数分隔符，
 *    报成「passed 4 arguments, but takes just 2」，看着完全不像逗号的问题。
 * ================================================================ */
#ifdef RT_USING_FINSH

#include <finsh.h>

#define G2_NONE   0
#define G2_NEW    1
#define G2_MOVE   2
#define G2_UNDO   3
#define G2_SET    4
#define G2_CLEAR  5
#define G2_AUTO   6

static int s_act = G2_NONE;
static int s_a1  = 0;
static int s_a2  = 0;
static int s_a3  = 0;

static void g2_async(void *arg)
{
    (void)arg;

    switch (s_act)
    {
    case G2_NEW:
        new_game();
        break;

    case G2_MOVE:
        do_move(s_a1);
        refresh_all();
        break;

    case G2_UNDO:
        do_undo();
        break;

    case G2_SET:
        if (s_a1 >= 0 && s_a1 < N && s_a2 >= 0 && s_a2 < N)
        {
            s_board[s_a1][s_a2] = s_a3;
            rt_kprintf("2048: set [%d][%d]=%d tiles=%d\n",
                       s_a1, s_a2, s_a3, tile_count());
            refresh_all();
        }
        else
        {
            rt_kprintf("2048: set -> row/col out of range (0..%d)\n", N - 1);
        }
        break;

    case G2_CLEAR:
        memset(s_board, 0, sizeof(s_board));
        s_snap_valid = 0;
        s_over       = 0;
        s_mask_mode  = 0;
        rt_kprintf("2048: clear -> empty board\n");
        refresh_all();
        break;

    case G2_AUTO:
    {
        int i, done = 0;

        for (i = 0; i < s_a1 && !s_over; i++)
        {
            int d = (int)(rng_next() % 4u);

            if (s_mask_mode == 1)
            {
                s_continued = 1;        /* 自动模式碰到 2048 就当作"继续玩" */
                s_mask_mode = 0;
            }
            do_move(d);
            done++;
        }
        rt_kprintf("2048: auto %d -> done=%d moves=%u score=%u tiles=%d max=%d over=%d\n",
                   s_a1, done, (unsigned)s_moves, (unsigned)s_score,
                   tile_count(), max_tile(), s_over);
        refresh_all();
        break;
    }

    default:
        break;
    }

    s_act = G2_NONE;
}

static void g2_post(int act, int a1, int a2, int a3)
{
    s_act = act;
    s_a1  = a1;
    s_a2  = a2;
    s_a3  = a3;
    lv_async_call(g2_async, NULL);
}

/* ── 状态总览（只读，可以直接在 shell 线程调）──────────────────── */
static int g2048_info(int argc, char **argv)
{
    int r, c;

    (void)argc;
    (void)argv;

    rt_kprintf("2048: ui=%s starts=%u score=%u best=%u moves=%u tiles=%d max=%d\n",
               g_root ? "on" : "off", (unsigned)s_start_cnt,
               (unsigned)s_score, (unsigned)s_best, (unsigned)s_moves,
               tile_count(), max_tile());
    rt_kprintf("2048: over=%d won=%d cont=%d mask=%d snap=%d spawn=%d hand=%d\n",
               s_over, s_won, s_continued, s_mask_mode, s_snap_valid,
               s_spawn_on, s_pressed);
    rt_kprintf("2048: keep_timer=%p rng=0x%08X evt_log=%d\n",
               g_keep, (unsigned)s_rng, s_evt_log);
    rt_kprintf("2048: touch down=%u up=%u swipe=%u last_press=(%d,%d)\n",
               (unsigned)s_evt_down, (unsigned)s_evt_up, (unsigned)s_evt_sw,
               (int)s_press_pt.x, (int)s_press_pt.y);

    /* 几何 + 面板重叠判定（UI 不在时 dump_geometry 自己会跳过）。
     * ⚠ 别只把这些放在 board_create 里 —— app 被 Resume 进来时根本不走
     *   create，那时候就什么都查不到了。 */
    dump_geometry();

    for (r = 0; r < N; r++)
    {
        rt_kprintf("2048: r%d", r);
        for (c = 0; c < N; c++)
        {
            rt_kprintf("%7d", s_board[r][c]);
        }
        rt_kprintf("\n");
    }
    return 0;
}
MSH_CMD_EXPORT(g2048_info, 2048 state: board/score/flags/geometry);

static int g2048_new(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    g2_post(G2_NEW, 0, 0, 0);
    return 0;
}
MSH_CMD_EXPORT(g2048_new, start a new 2048 game);

static int g2048_undo(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    g2_post(G2_UNDO, 0, 0, 0);
    return 0;
}
MSH_CMD_EXPORT(g2048_undo, undo the last 2048 move);

static int g2048_clear(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    g2_post(G2_CLEAR, 0, 0, 0);
    return 0;
}
MSH_CMD_EXPORT(g2048_clear, empty the 2048 board);

static int g2048_move(int argc, char **argv)
{
    int d;

    if (argc < 2)
    {
        rt_kprintf("usage: g2048_move <u|r|d|l>   (0=up 1=right 2=down 3=left)\n");
        return -1;
    }
    d = parse_dir(argv[1]);
    if (d < 0)
    {
        rt_kprintf("2048: bad direction '%s'\n", argv[1]);
        return -1;
    }
    g2_post(G2_MOVE, d, 0, 0);
    return 0;
}
MSH_CMD_EXPORT(g2048_move, g2048_move <u|r|d|l> : slide the board once);

static int g2048_set(int argc, char **argv)
{
    if (argc < 4)
    {
        rt_kprintf("usage: g2048_set <row> <col> <value>   e.g. g2048_set 0 0 2\n");
        return -1;
    }
    g2_post(G2_SET, atoi(argv[1]), atoi(argv[2]), atoi(argv[3]));
    return 0;
}
MSH_CMD_EXPORT(g2048_set, g2048_set <r> <c> <v> : place one tile);

static int g2048_spawn(int argc, char **argv)
{
    if (argc < 2)
    {
        rt_kprintf("2048: spawn=%d (1 = 每次滑动后随机生成新块)\n", s_spawn_on);
        return 0;
    }
    s_spawn_on = (atoi(argv[1]) != 0);
    rt_kprintf("2048: spawn=%d\n", s_spawn_on);
    return 0;
}
MSH_CMD_EXPORT(g2048_spawn, g2048_spawn <0|1> : toggle random tile spawning);

static int g2048_seed(int argc, char **argv)
{
    if (argc < 2)
    {
        rt_kprintf("2048: rng=0x%08X\n", (unsigned)s_rng);
        return 0;
    }
    s_rng = (rt_uint32_t)strtoul(argv[1], NULL, 0);
    rt_kprintf("2048: rng=0x%08X\n", (unsigned)s_rng);
    return 0;
}
MSH_CMD_EXPORT(g2048_seed, g2048_seed <n> : fix the RNG seed for repeatable runs);

static int g2048_auto(int argc, char **argv)
{
    int n;

    if (argc < 2)
    {
        rt_kprintf("usage: g2048_auto <n>   (random legal moves)\n");
        return -1;
    }
    n = atoi(argv[1]);
    if (n <= 0)
    {
        n = 1;
    }
    if (n > 500)
    {
        n = 500;
    }
    g2_post(G2_AUTO, n, 0, 0);
    return 0;
}
MSH_CMD_EXPORT(g2048_auto, g2048_auto <n> : play n random moves);

static int g2048_log(int argc, char **argv)
{
    if (argc >= 2)
    {
        s_evt_log = (atoi(argv[1]) != 0);
    }
    rt_kprintf("2048: evt_log=%d\n", s_evt_log);
    return 0;
}
MSH_CMD_EXPORT(g2048_log, g2048_log <0|1> : toggle touch event logging);

static int g2048_ui(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    rt_kprintf("2048: gui_app_run(\"%s\") -> start app\n", APP_ID);
    gui_app_run(APP_ID);
    return 0;
}
MSH_CMD_EXPORT(g2048_ui, switch to the 2048 app (same as tapping its icon));

#endif /* RT_USING_FINSH */
