/**
 * @file    app_clock_band.c
 * @brief   「手环风」数字表盘 —— 大号时间 + 日期 + 可换背景图
 *
 * 设计要点
 * --------
 * 1. 时间、日期都直接读 RTC（time() / localtime()），不是照着框架的 tick 累加，
 *    所以只要 RTC 里是真时间，这里显示的就是真时间。
 * 2. 时间用「预渲染 PNG 贴图」而不是字体渲染。原因：本工程可用的大号字体只有
 *    FreeType 那条路，而 tiny55 是 16x16 点阵自压缩字体，放大必糊；数字是
 *    一次性画好的贴图，零渲染开销，也不会因为字库差异变形。
 * 3. 数字是等宽单元（每格 WF_DIGIT_W 一样宽），所以 1 和 8 占一样宽，
 *    分秒跳变时整行不会左右抖。
 * 4. 背景就是一张 wf_bg.png（390x450）。**换背景只需要替换这一个文件。**
 *    文字下面垫了一层柔和的 wf_scrim.png 压暗，所以换成任何照片都还读得清。
 *
 * 资源与排版常量由 watchface-tools/make_assets.py 生成（wf_layout.h），
 * 想调字号/配色/留白改那个脚本重跑，本文件不用动。
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <time.h>
#include "littlevgl2rtt.h"
#include "lvgl.h"
#include "lv_ext_resource_manager.h"
#include "gui_app_fwk.h"

#include "app_clock_main.h"
#include "app_mem.h"
#include "bat_mon.h"        /* 电量百分比 + 充电状态：读 ADC 和 PMU，不在这里读 */

#include "wf_layout.h"      /* 自动生成：屏幕尺寸 / 时间行 / 日期行 的所有几何常量 */

#define WF_COLON_BLINK   0  /* 1 = 冒号每秒闪一下；0 = 常亮
                             *
                             * 曾经开过 1（为了让"表盘还活着"有肉眼可见的证据，因为表盘
                             * 只在分钟变化时才重画，一分钟内画面完全静止）。
                             * 实测反馈：闪的冒号看着廉价、分散注意力，不需要。
                             * 已关回 0。要复现当时的现象就改成 1。 */
#define WF_USE_SCRIM     1  /* 1 = 文字下垫压暗层，换任何背景图都读得清 */

/* ══════════════════════════════════════════════════════════════════════════
 *  右上角的电池指示（小米手环那种：一个电池小图标 + 百分比）
 * ══════════════════════════════════════════════════════════════════════════
 *  为什么不需要图片资源
 *  --------------------
 *  这个小图标整块是**用 LVGL 的基本图形拼出来的**：外壳 = 一个只有描边的圆角矩形，
 *  电量条 = 里面一个填色的矩形，正极凸起 = 右边一个小圆角矩形，充电闪电 = 一条折线。
 *  所以不用往 resource/images 里塞 PNG，也不用跑打包脚本 —— 改颜色改尺寸直接
 *  改下面这几个常量即可。整块加起来 5 个 LVGL 对象，比一张图还省内存。
 *
 *  为什么放右上角
 *  --------------
 *  时间在 y=140、日期在 y=259，上面整片是空的；底部留给「未校时」提示。
 *  另外屏幕最上面 28px（=450>>4）是上划面板的透明感应条（app_clock_status_bar.c），
 *  图标从 34 开始放，正好避开它，不会出现"点电池弹面板"的怪事。
 */
#define BAT_SHOW          1     /* 0 = 整块不建，回到没有电池指示的样子 */
#define BAT_SHOW_STATE    1     /* 1 = 插着线时在图标下面多写一行「充电中 / 已充满」 */

#define BAT_MARGIN_R      20    /* 离屏幕右边缘 */
#define BAT_MARGIN_T      34    /* 离屏幕上边缘（要大于 28，见上面的说明） */

#define BAT_BODY_W        40    /* 电池外壳宽 */
#define BAT_BODY_H        20    /* 电池外壳高 */
#define BAT_BODY_RADIUS   6     /* 外壳圆角 */
#define BAT_BODY_BORDER   2     /* 外壳描边粗细 */

#define BAT_TIP_W         4     /* 正极小凸起 宽 */
#define BAT_TIP_H         9     /* 正极小凸起 高 */
#define BAT_TIP_GAP       2     /* 凸起和外壳之间的缝 */

#define BAT_FILL_INSET    5     /* 电量条离外壳内侧的距离（含描边）*/

#define BAT_ICON_W        (BAT_BODY_W + BAT_TIP_GAP + BAT_TIP_W)
#define BAT_ICON_H        BAT_BODY_H
#define BAT_FILL_MAX_W    (BAT_BODY_W - 2 * BAT_FILL_INSET)
#define BAT_FILL_H        (BAT_BODY_H - 2 * BAT_FILL_INSET)
#define BAT_FILL_MIN_W    3     /* 1% 也要看得见，不然像是空格子 */
#define BAT_TEXT_GAP      8     /* 百分比和图标之间的距离 */

#define BAT_BOLT_W        12    /* 闪电折线占的格子（等宽，这样隐藏/显示时百分比不跳） */
#define BAT_BOLT_H        16

#define BAT_PERIOD_S      10    /* 多少秒读一次。一次读 ~22ms，别太勤 */
#define BAT_LOW_PCT       20    /* 低于这个百分比：数字和电量条变红 */

/* 颜色。表盘是深底白字，所以这三个都要够亮才看得清。 */
#define BAT_C_NORMAL      lv_color_make(0xFF, 0xFF, 0xFF)   /* 放电：白 */
#define BAT_C_CHARGE      lv_color_make(0x3D, 0xE0, 0x8A)   /* 充电/充满：绿 */
#define BAT_C_LOW         lv_color_make(0xFF, 0x5A, 0x52)   /* 低电：红 */
#define BAT_C_NONE        lv_color_make(0x9A, 0xA3, 0xB2)   /* 读不到：灰 */

/* 闪电单独放在「百分比左边」而不是压在电池上。
 * 压上去不行：电量条是从左边开始长的，充到一半时闪电的右半截会落在透明背景上，
 * 用深色看不见、用白色又和右边的白字糊在一起。放在行首就永远在背景上，
 * 什么电量都读得清。 */
#define BAT_C_BOLT        lv_color_make(0x3D, 0xE0, 0x8A)

/* RTC「还没被校过」的判定线。
 *
 * 这块板子没有 RTC 备份电池：拔电再上电，RTC 域整块掉电，值归零变成
 * 2000-01-01 00:00:00。watch_demo.c 的 app_watch_entry() 会给它兜一个
 * 2024-01-01 12:00，好让表盘有数字可画。
 *
 * 问题就出在这：那个兜底值看着像个正常时间。屏幕上一句"12:00"，
 * 既不像坏、也不像没设，谁也看不出问题 —— 反馈里那句"时间一直是 12"
 * 就是这么来的。所以这里按年份划一条线，年份 < 2025 就认定没校过，
 * 直接在屏幕上挂提示，把"为什么是 12"讲明白。
 *
 * 注意：这条线必须和 watch_demo.c 里写的兜底年份一致（那边是 2024）。 */
#define RTC_SET_MIN_YEAR  (2025)

/* 校时面板要调的 6 个字段。顺序 = 界面上先左右、再上下的排布顺序。 */
enum
{
    CAL_YEAR = 0,
    CAL_MON,
    CAL_DAY,
    CAL_HOUR,
    CAL_MIN,
    CAL_SEC,
    CAL_FIELD_N
};

typedef struct
{
    lv_obj_t *bg;
#if WF_USE_SCRIM
    lv_obj_t *scrim;
#endif
    lv_obj_t *hour[2];      /* 时 十位 / 个位 */
    lv_obj_t *colon;
    lv_obj_t *minute[2];    /* 分 十位 / 个位 */
    lv_obj_t *date[6];      /* 月十位 月 月个位 日十位 日个位 日（用不到的收起来）*/
    lv_obj_t *week;         /* 周一..周日 */
    lv_obj_t *unset_hint;   /* 「未校时 · 请重新对时」，只在 RTC 没校过时出现 */
    lv_timer_t *tick;
    uint8_t last_hour, last_min, last_day;

    /* ---- 右上角电池指示（见文件开头 BAT_* 那一大段）----
     * bat_row  ：一行横向排列 —— [闪电][百分比][电池图标]，整行贴右上角
     * bat_state：「充电中 / 已充满」，在 bat_row 下方，不插线时收起 */
    lv_obj_t *bat_col;
    lv_obj_t *bat_row;
    lv_obj_t *bat_bolt;
    lv_obj_t *bat_pct;
    lv_obj_t *bat_fill;
    lv_obj_t *bat_tip;
    lv_obj_t *bat_state;
    uint8_t   bat_sec;          /* 秒计数器：攒够 BAT_PERIOD_S 秒才读一次电压 */
    int8_t    bat_pct_last;     /* 上次画出来的百分比，没变就不动界面 */
    int8_t    bat_st_last;      /* 上次画出来的充电状态 */

    /* ---- 校时面板：见下面「校时面板」那一大段 ----
     * 遮罩挂在 lv_scr_act() 上（和状态栏同一层），不在这里随 root 一起被清掉，
     * 所以 deinit 里要自己删。 */
    lv_obj_t   *cal_mask;                 /* 全屏遮罩：压暗 + 吃掉误触 */
    lv_obj_t   *cal_panel;                /* 面板本体 */
    lv_obj_t   *cal_btn[CAL_FIELD_N];     /* 年 月 日 时 分 秒 六个字段按钮 */
    lv_obj_t   *cal_lbl[CAL_FIELD_N];     /* 每个按钮里的文字 */
    lv_timer_t *cal_rep;                  /* 按住不放时的连加/连减 */
    int         cal_val[CAL_FIELD_N];     /* 正在编辑的值，按"确定"之前不碰 RTC */
    int8_t      cal_sel;                  /* 当前选中的字段 */
    int8_t      cal_dir;                  /* 连加的方向：+1 / -1 */
    uint8_t     cal_open;                 /* 面板当前是否显示 */
    uint8_t last_unset;     /* 上一次的"没校时"状态，用来只在变化时打日志 */
} app_clock_band_t;

LV_IMG_DECLARE(wf_bg);
LV_IMG_DECLARE(wf_scrim);
LV_IMG_DECLARE(wf_d0);
LV_IMG_DECLARE(wf_d1);
LV_IMG_DECLARE(wf_d2);
LV_IMG_DECLARE(wf_d3);
LV_IMG_DECLARE(wf_d4);
LV_IMG_DECLARE(wf_d5);
LV_IMG_DECLARE(wf_d6);
LV_IMG_DECLARE(wf_d7);
LV_IMG_DECLARE(wf_d8);
LV_IMG_DECLARE(wf_d9);
LV_IMG_DECLARE(wf_colon);
LV_IMG_DECLARE(wf_s0);
LV_IMG_DECLARE(wf_s1);
LV_IMG_DECLARE(wf_s2);
LV_IMG_DECLARE(wf_s3);
LV_IMG_DECLARE(wf_s4);
LV_IMG_DECLARE(wf_s5);
LV_IMG_DECLARE(wf_s6);
LV_IMG_DECLARE(wf_s7);
LV_IMG_DECLARE(wf_s8);
LV_IMG_DECLARE(wf_s9);
LV_IMG_DECLARE(wf_ym);
LV_IMG_DECLARE(wf_yd);
LV_IMG_DECLARE(wf_w1);
LV_IMG_DECLARE(wf_w2);
LV_IMG_DECLARE(wf_w3);
LV_IMG_DECLARE(wf_w4);
LV_IMG_DECLARE(wf_w5);
LV_IMG_DECLARE(wf_w6);
LV_IMG_DECLARE(wf_w7);

static const void *const big_digit[10] =
{
    LV_EXT_IMG_GET(wf_d0), LV_EXT_IMG_GET(wf_d1), LV_EXT_IMG_GET(wf_d2),
    LV_EXT_IMG_GET(wf_d3), LV_EXT_IMG_GET(wf_d4), LV_EXT_IMG_GET(wf_d5),
    LV_EXT_IMG_GET(wf_d6), LV_EXT_IMG_GET(wf_d7), LV_EXT_IMG_GET(wf_d8),
    LV_EXT_IMG_GET(wf_d9),
};

static const void *const small_digit[10] =
{
    LV_EXT_IMG_GET(wf_s0), LV_EXT_IMG_GET(wf_s1), LV_EXT_IMG_GET(wf_s2),
    LV_EXT_IMG_GET(wf_s3), LV_EXT_IMG_GET(wf_s4), LV_EXT_IMG_GET(wf_s5),
    LV_EXT_IMG_GET(wf_s6), LV_EXT_IMG_GET(wf_s7), LV_EXT_IMG_GET(wf_s8),
    LV_EXT_IMG_GET(wf_s9),
};

/* 索引 0..6 对应 wf_w1..wf_w7 = 周一..周日 */
static const void *const week_name[7] =
{
    LV_EXT_IMG_GET(wf_w1), LV_EXT_IMG_GET(wf_w2), LV_EXT_IMG_GET(wf_w3),
    LV_EXT_IMG_GET(wf_w4), LV_EXT_IMG_GET(wf_w5), LV_EXT_IMG_GET(wf_w6),
    LV_EXT_IMG_GET(wf_w7),
};

static app_clock_band_t *p_clk_band = NULL;

/* 校时面板的前置声明。
 * 面板的实现在文件后半段（band_refresh 之后），但 resume_callback 比它更早，
 * 又要在「RTC 没校过」时把面板叫出来，所以入口先在这里声明。
 *
 * cal_auto_shown 也放这儿：它刻意是文件级 static 而不是 p_clk_band 的成员 ——
 * 切到主菜单再切回来会重新 init（见 app_clock_main.c 的 STATE_DEINIT），
 * 放在结构体里就会每次回到表盘都弹一遍面板，像推销。 */
static uint8_t   cal_auto_shown = 0;
static void      cal_open(void);
static void      cal_close(void);
static rt_bool_t rtc_is_unset(void);

/* 时间行的 5 个格子，从左到右的 x。等宽单元，所以是一串加法出来的。 */
#define WF_X_DIGIT0   (WF_TIME_X0)
#define WF_X_DIGIT1   (WF_TIME_X0 + WF_DIGIT_W)
#define WF_X_COLON    (WF_TIME_X0 + 2 * WF_DIGIT_W)
#define WF_X_DIGIT2   (WF_TIME_X0 + 2 * WF_DIGIT_W + WF_COLON_W)
#define WF_X_DIGIT3   (WF_TIME_X0 + 3 * WF_DIGIT_W + WF_COLON_W)


static void band_place(lv_obj_t *obj, const void *src, lv_coord_t x, lv_coord_t y)
{
    lv_img_set_src(obj, src);
    lv_obj_set_pos(obj, x, y);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
}


static void band_set_time(const struct tm *t)
{
    band_place(p_clk_band->hour[0],   big_digit[t->tm_hour / 10], WF_X_DIGIT0, WF_TIME_Y);
    band_place(p_clk_band->hour[1],   big_digit[t->tm_hour % 10], WF_X_DIGIT1, WF_TIME_Y);
    band_place(p_clk_band->minute[0], big_digit[t->tm_min / 10],  WF_X_DIGIT2, WF_TIME_Y);
    band_place(p_clk_band->minute[1], big_digit[t->tm_min % 10],  WF_X_DIGIT3, WF_TIME_Y);
}


static void band_set_date(const struct tm *t)
{
    /* 「9月20日 周日」—— 顺序是「数字在前、单位在后」。
     * 月份和日期都可能是一位也可能两位，元素个数会变，所以每次整个重排一遍
     * 再整体居中，比按最大宽度预留更整齐。
     *
     * 这一段的顺序必须和 watchface-tools/make_assets.py 的 mon_day_tokens() 一致，
     * 预览图就是照那个顺序拼的。踩过的坑：月份个位漏排（屏幕上只剩一个孤零零
     * 的「月」），以及把「月」写到了月份数字前面。 */
    int mon = t->tm_mon + 1;
    int day = t->tm_mday;
    const void *seq[6];
    lv_coord_t   wid[6];
    int n = 0;
    int i;
    int wi;
    lv_coord_t total;
    lv_coord_t x;

    /* ---- 月份：十位(可选) + 个位 + 「月」---- */
    if (mon >= 10)
    {
        seq[n] = small_digit[mon / 10];
        wid[n] = WF_SDIGIT_W;
        n++;
    }

    seq[n] = small_digit[mon % 10];
    wid[n] = WF_SDIGIT_W;
    n++;

    seq[n] = LV_EXT_IMG_GET(wf_ym);
    wid[n] = WF_YM_W;
    n++;

    /* ---- 日期：十位(可选) + 个位 + 「日」---- */
    if (day >= 10)
    {
        seq[n] = small_digit[day / 10];
        wid[n] = WF_SDIGIT_W;
        n++;
    }

    seq[n] = small_digit[day % 10];
    wid[n] = WF_SDIGIT_W;
    n++;

    seq[n] = LV_EXT_IMG_GET(wf_yd);
    wid[n] = WF_YD_W;
    n++;

    total = WF_WEEK_SEP + WF_WEEK_W;

    for (i = 0; i < n; i++)
    {
        total += wid[i];
    }

    if (n > 1)
    {
        total += WF_DAY_GAP * (n - 1);
    }

    x = (WF_SCR_W - total) / 2;

    for (i = 0; i < n; i++)
    {
        band_place(p_clk_band->date[i], seq[i], x, WF_DATE_Y);
        x += wid[i] + WF_DAY_GAP;
    }

    for (; i < 6; i++)                                  /* 用不到的槽位收起来 */
    {
        lv_obj_add_flag(p_clk_band->date[i], LV_OBJ_FLAG_HIDDEN);
    }

    x += WF_WEEK_SEP - WF_DAY_GAP;

    /* tm_wday: 0=周日 .. 6=周六  ->  week_name: 0=周一 .. 6=周日 */
    wi = (t->tm_wday == 0) ? 7 : t->tm_wday;
    band_place(p_clk_band->week, week_name[wi - 1], x, WF_DATE_Y);
}


/* ══════════════════════════════════════════════════════════════════════════
 *  电池指示：建界面
 * ══════════════════════════════════════════════════════════════════════════
 *  层次（只有这一处嵌套，是为了让「充电中」自动挂在整行下面）：
 *
 *      bat_col      竖排，整体贴右上角，子项一律靠右
 *        ├─ bat_row          横排：[闪电格][百分比][电池图标]
 *        └─ bat_state        一行小字「充电中 / 已充满」，不插线时藏起来
 *                              （藏起来就不占位置，整块自己缩回去）
 *
 *  电池图标内部 3 块（外壳 / 电量条 / 正极凸起）走绝对坐标 ——
 *  它们是一个固定尺寸的格子，不存在"文字变长就错位"的问题，算死坐标反而最稳。
 */
static const lv_point_t bat_bolt_pts[] =
{
    /* 闪电。x∈[2,10]、y∈[0,14]，等下靠 set_pos 摆到 12x16 格子的正中间。
     *
     * 必须是文件级的 static：lv_line_set_points() **只存指针不拷数据**，
     * 写成函数里的局部数组，函数一返回这块内存就在栈上被踩烂了，
     * 屏幕会画出一堆随机折线。 */
    {7, 0}, {2, 7}, {5, 7}, {4, 14}, {10, 5}, {7, 5}, {7, 0}
};

static void bat_create(lv_obj_t *root)
{
    lv_obj_t *o;
    lv_obj_t *icon;

    /* 整块：竖排 + 贴右上角。用 flex 而不是 align_to 算坐标 ——
     * 百分比从 "9%" 变到 "100%" 宽度会变，算死坐标就会偏。
     * （上一轮上划面板的蓝牙状态字就是栽在算死坐标上，越调越歪。） */
    p_clk_band->bat_col = lv_obj_create(root);
    lv_obj_remove_style_all(p_clk_band->bat_col);
    lv_obj_set_size(p_clk_band->bat_col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(p_clk_band->bat_col, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(p_clk_band->bat_col, LV_FLEX_FLOW_COLUMN);
    /* 主轴（竖）从上往下排；交叉轴（横）靠右 —— 这样两行都是右对齐的 */
    lv_obj_set_flex_align(p_clk_band->bat_col,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_row(p_clk_band->bat_col, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(p_clk_band->bat_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(p_clk_band->bat_col, LV_ALIGN_TOP_RIGHT, -BAT_MARGIN_R, BAT_MARGIN_T);

    /* ---- 第一行 ---- */
    p_clk_band->bat_row = lv_obj_create(p_clk_band->bat_col);
    lv_obj_remove_style_all(p_clk_band->bat_row);
    lv_obj_set_size(p_clk_band->bat_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(p_clk_band->bat_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(p_clk_band->bat_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(p_clk_band->bat_row,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(p_clk_band->bat_row, BAT_TEXT_GAP,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(p_clk_band->bat_row, LV_OBJ_FLAG_SCROLLABLE);

    /* ① 充电闪电。
     *
     * 占一个固定大小的格子，只把**折线本身**藏起来、格子留着 ——
     * 这样充不充电，百分比和电池的位置都一样，不会左右跳。 */
    o = lv_obj_create(p_clk_band->bat_row);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, BAT_BOLT_W, BAT_BOLT_H);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);

    p_clk_band->bat_bolt = lv_line_create(o);
    lv_line_set_points(p_clk_band->bat_bolt, bat_bolt_pts,
                       sizeof(bat_bolt_pts) / sizeof(bat_bolt_pts[0]));
    lv_obj_set_style_line_width(p_clk_band->bat_bolt, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_line_color(p_clk_band->bat_bolt, BAT_C_BOLT,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_line_rounded(p_clk_band->bat_bolt, true,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    /* 墨迹范围 x∈[2,10]、y∈[0,14]，加 2px 线宽后各向外胀 1px
     *  → 原点放 (0,1) 就正好居中 */
    lv_obj_set_pos(p_clk_band->bat_bolt, 0, 1);

    /* ② 百分比。字体用主题的 FONT_NORMAL（这块屏 390 宽 > 350，所以是 20px），
     * 和上划面板、校时面板同一套字，不会大小不一。 */
    p_clk_band->bat_pct = lv_label_create(p_clk_band->bat_row);
    lv_label_set_text(p_clk_band->bat_pct, "--%");
    lv_ext_set_local_font(p_clk_band->bat_pct, FONT_NORMAL, BAT_C_NONE);

    /* ③ 电池图标：一个固定尺寸的透明格子，里面三块绝对定位 */
    icon = lv_obj_create(p_clk_band->bat_row);
    lv_obj_remove_style_all(icon);
    lv_obj_set_size(icon, BAT_ICON_W, BAT_ICON_H);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);

    /* 外壳：只有描边、里面透明 —— 背景图能从电池里透出来，才不像贴了块白纸 */
    o = lv_obj_create(icon);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, BAT_BODY_W, BAT_BODY_H);
    lv_obj_set_pos(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(o, BAT_BODY_RADIUS, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(o, BAT_BODY_BORDER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(o, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(o, BAT_C_NORMAL, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* 电量条。和外壳是**兄弟**关系（都挂在 icon 上），不是外壳的子对象 ——
     * 免得去纠结"子对象坐标从描边内侧算还是外侧算"。它是后创建的，
     * 所以盖在外壳上面；但四周各让开 5px（描边只有 2px），不会压到边框。 */
    p_clk_band->bat_fill = lv_obj_create(icon);
    lv_obj_remove_style_all(p_clk_band->bat_fill);
    lv_obj_set_size(p_clk_band->bat_fill, BAT_FILL_MAX_W, BAT_FILL_H);
    lv_obj_set_pos(p_clk_band->bat_fill, BAT_FILL_INSET, BAT_FILL_INSET);
    lv_obj_clear_flag(p_clk_band->bat_fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(p_clk_band->bat_fill, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(p_clk_band->bat_fill, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(p_clk_band->bat_fill, BAT_C_NORMAL, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* 正极凸起：贴在外壳右边、竖向居中 */
    p_clk_band->bat_tip = lv_obj_create(icon);
    lv_obj_remove_style_all(p_clk_band->bat_tip);
    lv_obj_set_size(p_clk_band->bat_tip, BAT_TIP_W, BAT_TIP_H);
    lv_obj_set_pos(p_clk_band->bat_tip, BAT_BODY_W + BAT_TIP_GAP, (BAT_BODY_H - BAT_TIP_H) / 2);
    lv_obj_clear_flag(p_clk_band->bat_tip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(p_clk_band->bat_tip, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(p_clk_band->bat_tip, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(p_clk_band->bat_tip, BAT_C_NORMAL, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* ---- 第二行：「充电中 / 已充满」----
     * 为什么还要写一行字，光靠颜色和闪电不够吗：
     * 「插着线但充电器没启动」和「正在充」「已充满」在颜色上是分不出来的，
     * 而这三件事对第一次上手的意义完全不同。多这一行，一眼就有答案。
     * 不想要就 BAT_SHOW_STATE 改 0。 */
    p_clk_band->bat_state = lv_label_create(p_clk_band->bat_col);
    lv_label_set_text(p_clk_band->bat_state, "");
    lv_ext_set_local_font(p_clk_band->bat_state, FONT_NORMAL, BAT_C_CHARGE);
    lv_obj_add_flag(p_clk_band->bat_state, LV_OBJ_FLAG_HIDDEN);

    /* 一次性把摆放位置报到串口：屏幕上的东西我（改代码的）看不见，
     * 摆歪了、压到时间了，只能靠这几个数字判断。
     * 期望：右上角，y 从 BAT_MARGIN_T 开始，底边（含「充电中」那行）远在
     * WF_TIME_Y 之上 —— 也就是不会跟大号时间打架。 */
    lv_obj_update_layout(root);
    {
        lv_coord_t x = lv_obj_get_x(p_clk_band->bat_col);
        lv_coord_t y = lv_obj_get_y(p_clk_band->bat_col);
        lv_coord_t w = lv_obj_get_width(p_clk_band->bat_col);
        lv_coord_t h = lv_obj_get_height(p_clk_band->bat_col);

        rt_kprintf("battery: block x=%d..%d y=%d..%d (%dx%d) "
                   "screen %dx%d  time row starts y=%d -> %s\n",
                   x, x + w, y, y + h, w, h,
                   WF_SCR_W, WF_SCR_H, WF_TIME_Y,
                   (y + h < WF_TIME_Y) ? "no overlap" : "OVERLAPS time row!");
    }
}


/* 把最新一次读数画到界面上。读的动作在 bat_mon_get() 里，这里只管画。 */
static void bat_apply(void)
{
    const bat_info_t *b;
    int          pct;
    int          w;
    lv_color_t   c;
    char         buf[12];

    b   = bat_mon_get();
    pct = b->percent;

    /* 读到的原始信息全打一行出来 —— 屏幕上的东西我看不见，
     * 「表盘为什么显示 --%」「到底在不在充电」全得靠串口这几行。 */
    bat_mon_dump();

    if (pct == p_clk_band->bat_pct_last && (int8_t)b->state == p_clk_band->bat_st_last)
    {
        return;                          /* 没变化就不动界面，省一次重绘 */
    }

    p_clk_band->bat_pct_last = (int8_t)pct;
    p_clk_band->bat_st_last  = (int8_t)b->state;

    if (pct < 0)
    {
        lv_label_set_text(p_clk_band->bat_pct, "--%");
        c = BAT_C_NONE;
        w = 0;                           /* 空格子：表示"不知道"，不是"没电了" */
    }
    else
    {
        lv_snprintf(buf, sizeof(buf), "%d%%", pct);
        lv_label_set_text(p_clk_band->bat_pct, buf);

        if (pct <= BAT_LOW_PCT)
        {
            c = BAT_C_LOW;               /* 低电优先，红色盖过充电的绿 */
        }
        else if (b->state != BAT_ST_DISCHARGE)
        {
            c = BAT_C_CHARGE;
        }
        else
        {
            c = BAT_C_NORMAL;
        }

        w = BAT_FILL_MAX_W * pct / 100;
        if (pct > 0 && w < BAT_FILL_MIN_W)
        {
            w = BAT_FILL_MIN_W;          /* 1% 也得看得见，不然像是空格子 */
        }
    }

    lv_obj_set_style_text_color(p_clk_band->bat_pct, c, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(p_clk_band->bat_fill, c, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(p_clk_band->bat_tip, c, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_size(p_clk_band->bat_fill, w, BAT_FILL_H);

    /* 闪电只在"正在充"的时候亮；充满了就不闪了。 */
    if (b->state == BAT_ST_CHARGING)
    {
        lv_obj_clear_flag(p_clk_band->bat_bolt, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(p_clk_band->bat_bolt, LV_OBJ_FLAG_HIDDEN);
    }

#if BAT_SHOW_STATE
    if (b->state == BAT_ST_CHARGING || b->state == BAT_ST_FULL)
    {
        lv_label_set_text(p_clk_band->bat_state,
                          (b->state == BAT_ST_FULL) ? "已充满" : "充电中");
        lv_obj_clear_flag(p_clk_band->bat_state, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(p_clk_band->bat_state, LV_OBJ_FLAG_HIDDEN);
    }
#endif
}


static void band_refresh(void)
{
    time_t now;
    struct tm *t;

    if (p_clk_band == NULL)
    {
        return;
    }

    time(&now);
    t = localtime(&now);

    if (t == RT_NULL)
    {
        return;
    }

    /* 换日了，日期和时间一起重画 */
    if (t->tm_mday != p_clk_band->last_day)
    {
        band_set_date(t);
        band_set_time(t);
        p_clk_band->last_day  = t->tm_mday;
        p_clk_band->last_hour = t->tm_hour;
        p_clk_band->last_min  = t->tm_min;
    }
    else if (t->tm_hour != p_clk_band->last_hour || t->tm_min != p_clk_band->last_min)
    {
        /* 只在分钟真的变了才动图像源。每秒无脑重设会让 LVGL 一直重绘，
         * 白白耗电，而且这个表盘本来也不需要每秒刷。 */
        band_set_time(t);
        p_clk_band->last_hour = t->tm_hour;
        p_clk_band->last_min  = t->tm_min;
    }

#if WF_COLON_BLINK
    if (t->tm_sec & 1)
    {
        lv_obj_add_flag(p_clk_band->colon, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_clear_flag(p_clk_band->colon, LV_OBJ_FLAG_HIDDEN);
    }
#endif

    /* RTC 有没有被校过？见 RTC_SET_MIN_YEAR 的说明。
     *
     * 只在状态真的翻转时打一行日志 —— 屏幕上的东西我看不见，这条日志是
     * 唯一能从串口确认「提示挂上去了 / 撤掉了」的手段。每次刷新都打会把
     * 控制台刷爆，所以用 last_unset 卡一道。 */
    if (p_clk_band->unset_hint != NULL)
    {
        uint8_t is_unset = (t->tm_year + 1900 < RTC_SET_MIN_YEAR) ? 1 : 0;

        if (is_unset)
        {
            lv_obj_clear_flag(p_clk_band->unset_hint, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(p_clk_band->unset_hint, LV_OBJ_FLAG_HIDDEN);
        }

        if (is_unset != p_clk_band->last_unset)
        {
            rt_kprintf("watchface: rtc %s -> \"未校时\" hint %s\n",
                       is_unset ? "UNSET" : "set",
                       is_unset ? "shown" : "hidden");
            p_clk_band->last_unset = is_unset;
        }
    }

#if BAT_SHOW
    /* 电池不和秒针一起刷。
     *
     * 一是没必要：电压是慢变量，10 秒看一次和 1 秒看一次没区别。
     * 二是读一次要 ~22ms（drv_adc.c 里是 22 次采样去掉最大最小再取平均），
     * 写在每秒都跑的地方会白白占着 LVGL 线程。
     *
     * bat_sec 是"秒表"而不是看时间戳：这个函数本来就是 1 秒叫一次
     * （见 resume_callback 里 lv_timer_create(band_tick, 1000, ...)），
     * 数满 BAT_PERIOD_S 次就是 BAT_PERIOD_S 秒，和系统时基无关，更不容易错。 */
    if (++p_clk_band->bat_sec >= BAT_PERIOD_S)
    {
        p_clk_band->bat_sec = 0;
        bat_apply();
    }
#endif
}


static void band_tick(lv_timer_t *task)
{
    (void)task;
    band_refresh();
}


static rt_int32_t resume_callback(void)
{
    if (p_clk_band == NULL)
    {
        return -RT_ERROR;
    }

    if (p_clk_band->tick == NULL)
    {
        band_refresh();                                  /* 先画一次，别让表盘空一秒 */
        p_clk_band->tick = lv_timer_create(band_tick, 1000, RT_NULL);

        /* RTC 是空的（拔电之后必然如此）就直接把校时面板推到眼前。
         * 不这么做的话，用户看到「未校时」四个字加一个 12:00，却不知道
         * 该去哪儿改 —— 这正是上一轮反馈的原话。
         * 只在本次开机的第一次进入表盘时弹；按了「取消」就不会再烦他。 */
        if (cal_auto_shown == 0 && rtc_is_unset() == RT_TRUE)
        {
            cal_auto_shown = 1;
            rt_kprintf("calib: rtc unset at first resume -> auto opening panel\n");
            cal_open();
        }
    }

    return RT_EOK;
}


static rt_int32_t pause_callback(void)
{
    if (p_clk_band != NULL && p_clk_band->tick != NULL)
    {
        lv_timer_del(p_clk_band->tick);
        p_clk_band->tick = NULL;
    }

    return RT_EOK;
}


/* ══════════════════════════════════════════════════════════════════════
 *  校时面板 —— 回答「到底在哪里校准」
 *
 *  这块板子没有 RTC 备份电池：拔电再上电，RTC 域整块掉电、值归零。所以
 *  「校准」不是一次性的初始化，而是每次断电后都得重做一遍的日常动作。
 *  既然要反复用，就不能只留一条串口命令 —— 那要求用户开电脑、跑脚本。
 *
 *  于是把它做在手表自己身上：
 *    - RTC 没校过 -> 开机进表盘时自动弹出（见 resume_callback）
 *    - 任何时候   -> 点屏幕下方那条提示，或者串口敲 calib
 *
 *  几个刻意的取舍：
 *  1. 先点字段选中、再用 - / + 调，而不是每个字段配一对按钮。
 *     390x450 的屏上摆 12 个小按钮，手指按不准。
 *  2. 「-」「+」支持按住连加（自己起 lv_timer，不依赖 LVGL 的长按配置），
 *     否则把「日」从 1 调到 20 得点 19 次。
 *  3. 按「确定」之前只改内存里的副本，不碰 RTC —— 这样「取消」能干净退出。
 *  4. 遮罩挂在 lv_scr_act() 上。状态栏也挂在那儿（app_clock_main.c 里那句
 *     app_clock_main_status_bar_init(lv_scr_act(), tileview)），而表盘自己
 *     挂在 tileview 的 page 里，挂在 page 里会被状态栏盖住。
 *     代价：libobj_clean(page) 清不到它，deinit 里必须自己删。
 * ══════════════════════════════════════════════════════════════════════ */

/* 面板几何。都在 360x406 的面板里量，面板本身居中放在 390x450 的屏上。 */
#define CAL_W        360
#define CAL_H        406
#define CAL_F_BW     160      /* 字段按钮 */
#define CAL_F_BH     56
#define CAL_F_X0     12
#define CAL_F_X1     188
#define CAL_F_Y0     62
#define CAL_F_DY     64
#define CAL_AB_W     160      /* - / + */
#define CAL_AB_H     72
#define CAL_AB_Y     256
#define CAL_AB_X0    12
#define CAL_AB_X1    188
#define CAL_OK_W     160      /* 取消 / 确定 */
#define CAL_OK_H     52
#define CAL_OK_Y     340

#define CAL_REP_FIRST_MS   420    /* 按住多久开始连加 */
#define CAL_REP_MS          90    /* 开始连加之后的步进间隔 */

/* 这块表是深色背景的表盘，所以面板也用深色；只在"选中"和"提醒"上用琥珀色。 */
#define CAL_C_MASK    lv_color_make(0x00, 0x00, 0x00)
#define CAL_C_BG      lv_color_make(0x1B, 0x1F, 0x27)
#define CAL_C_BTN     lv_color_make(0x2E, 0x33, 0x3D)
#define CAL_C_BTN_SEL lv_color_make(0x4A, 0x3B, 0x14)
#define CAL_C_LINE    lv_color_make(0x44, 0x4A, 0x57)
#define CAL_C_AMBER   lv_color_make(0xFF, 0xC1, 0x07)
#define CAL_C_TEXT    lv_color_make(0xFF, 0xFF, 0xFF)
#define CAL_C_DIM     lv_color_make(0x9A, 0xA3, 0xB2)
#define CAL_C_OK      lv_color_make(0x2E, 0x7D, 0x32)

/* 传给事件回调的参数。不用把 int 强转成 void*：那样在 32/64 位和符号位上
 * 都容易出幺蛾子，一张静态表最省事。 */
static int cal_idx_tab[CAL_FIELD_N] = { CAL_YEAR, CAL_MON, CAL_DAY, CAL_HOUR, CAL_MIN, CAL_SEC };
static int cal_dir_tab[2]          = { -1, +1 };   /* [0] = 减，[1] = 加 */

static const char *const cal_field_name[CAL_FIELD_N] =
{
    "年", "月", "日", "时", "分", "秒"
};


static int cal_days_in_month(int y, int m)
{
    static const int d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    if (m < 1 || m > 12)
    {
        return 31;
    }

    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
    {
        return 29;
    }

    return d[m - 1];
}


/* 把某个字段加减一步。边界上是"回绕"而不是卡住：31 号再加就到 1 号，
 * 23 点再加就到 0 点 —— 手表上这比"按了没反应"好懂得多。
 * 只有「年」例外，2099 再加不绕回 2024（太容易手滑穿过去）。 */
static void cal_step(int field, int dir)
{
    app_clock_band_t *b = p_clk_band;
    int v;

    if (b == NULL || field < 0 || field >= CAL_FIELD_N)
    {
        return;
    }

    v = b->cal_val[field] + dir;

    switch (field)
    {
    case CAL_YEAR:
        if (v < 2024) { v = 2024; }
        if (v > 2099) { v = 2099; }
        break;

    case CAL_MON:
        if (v < 1)  { v = 12; }
        if (v > 12) { v = 1; }
        break;

    case CAL_DAY:
    {
        int dim = cal_days_in_month(b->cal_val[CAL_YEAR], b->cal_val[CAL_MON]);
        if (v < 1)   { v = dim; }
        if (v > dim) { v = 1; }
    }
    break;

    case CAL_HOUR:
        if (v < 0)  { v = 23; }
        if (v > 23) { v = 0; }
        break;

    default:                    /* 分、秒 */
        if (v < 0)  { v = 59; }
        if (v > 59) { v = 0; }
        break;
    }

    b->cal_val[field] = v;

    /* 年或月一动，原来选的「日」可能已经不存在了（1月31日 减一个月 -> 2月）。
     * 不夹住的话，set_date() 会收到一个非法日期，行为取决于驱动实现。 */
    if (field == CAL_YEAR || field == CAL_MON)
    {
        int dim = cal_days_in_month(b->cal_val[CAL_YEAR], b->cal_val[CAL_MON]);
        if (b->cal_val[CAL_DAY] > dim)
        {
            b->cal_val[CAL_DAY] = dim;
        }
    }
}


static void cal_style_field(int i, rt_bool_t selected)
{
    app_clock_band_t *b = p_clk_band;

    if (b == NULL || b->cal_btn[i] == NULL)
    {
        return;
    }

    lv_obj_set_style_bg_color(b->cal_btn[i], selected ? CAL_C_BTN_SEL : CAL_C_BTN,
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(b->cal_btn[i], selected ? CAL_C_AMBER : CAL_C_LINE,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(b->cal_btn[i], selected ? 2 : 1,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_ext_set_local_font(b->cal_lbl[i], FONT_NORMAL, selected ? CAL_C_AMBER : CAL_C_TEXT);
}


static void cal_render(void)
{
    app_clock_band_t *b = p_clk_band;
    char buf[20];
    int i;

    if (b == NULL || b->cal_panel == NULL)
    {
        return;
    }

    for (i = 0; i < CAL_FIELD_N; i++)
    {
        /* 数字在前、单位在后 ——「2024 年」而不是「年 2024」，
         * 跟表盘日期行的「9月20日」保持同一个语序。
         * 2026-09-20 用户验收时指出来的（这个错我在表盘日期上已经踩过一次）。 */
        if (i == CAL_YEAR)
        {
            rt_snprintf(buf, sizeof(buf), "%d %s", b->cal_val[i], cal_field_name[i]);
        }
        else
        {
            rt_snprintf(buf, sizeof(buf), "%02d %s", b->cal_val[i], cal_field_name[i]);
        }

        lv_label_set_text(b->cal_lbl[i], buf);
        /* 文字长度变了要重居中（年从 4 位变不变都是 4 位，但月/日会变），
         * 并且换了文字之后字体要重新挂一次 —— lv_ext_set_local_font 挂的是
         * 局部样式，这里重设最稳。 */
        lv_obj_align(b->cal_lbl[i], LV_ALIGN_CENTER, 0, 0);
        cal_style_field(i, (i == b->cal_sel) ? RT_TRUE : RT_FALSE);
    }
}


/* 按住不放：先立刻走一步，之后交给 cal_rep 定时器续步。 */
static void cal_adjust_event_cb(lv_event_t *e)
{
    app_clock_band_t *b = p_clk_band;
    lv_event_code_t code = lv_event_get_code(e);
    int dir = *(const int *)lv_event_get_user_data(e);

    if (b == NULL || b->cal_panel == NULL)
    {
        return;
    }

    if (code == LV_EVENT_PRESSED)
    {
        b->cal_dir = (int8_t)dir;
        cal_step(b->cal_sel, dir);
        cal_render();

        if (b->cal_rep != NULL)
        {
            lv_timer_set_period(b->cal_rep, CAL_REP_FIRST_MS);
            lv_timer_reset(b->cal_rep);
            lv_timer_resume(b->cal_rep);
        }
    }
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
    {
        if (b->cal_rep != NULL)
        {
            lv_timer_pause(b->cal_rep);
        }
    }
}


static void cal_repeat_cb(lv_timer_t *task)
{
    app_clock_band_t *b = p_clk_band;

    if (b == NULL || b->cal_open == 0)
    {
        lv_timer_pause(task);
        return;
    }

    cal_step(b->cal_sel, b->cal_dir);
    cal_render();

    lv_timer_set_period(task, CAL_REP_MS);      /* 第一下之后提速 */
}


static void cal_field_event_cb(lv_event_t *e)
{
    app_clock_band_t *b = p_clk_band;

    if (b == NULL || lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }

    b->cal_sel = (int8_t)(*(const int *)lv_event_get_user_data(e));
    cal_render();
}


/* 真正写 RTC 的地方。「确定」按钮和串口 calib ok 走的是同一条路。 */
static void cal_apply(void)
{
    app_clock_band_t *b = p_clk_band;
    time_t now;
    struct tm *t;

    /* cal_val[] 只在 cal_open() 里被填过（起点是 RTC 当前值）。面板没开就
     * 应用，写进去的是 memset 留下的全 0 —— 也就是 set_date(0, 0, 0)。
     * 串口那条 calib ok 完全可以绕过按钮走到这里，所以必须挡住。 */
    if (b == NULL || b->cal_open == 0)
    {
        rt_kprintf("calib: panel not open, nothing applied\n");
        return;
    }

    set_date(b->cal_val[CAL_YEAR], b->cal_val[CAL_MON], b->cal_val[CAL_DAY]);
    set_time(b->cal_val[CAL_HOUR], b->cal_val[CAL_MIN], b->cal_val[CAL_SEC]);

    /* 立刻回读一遍再报出来。屏幕我看不见，串口这一行是唯一能确认
     * "真的写进去了"的证据 —— 上一轮就是在"看起来像坏了"上栽过一次。 */
    time(&now);
    t = localtime(&now);

    if (t != RT_NULL)
    {
        rt_kprintf("calib: applied -> rtc now %04d-%02d-%02d %02d:%02d:%02d\n",
                   t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                   t->tm_hour, t->tm_min, t->tm_sec);
    }
    else
    {
        rt_kprintf("calib: applied, but readback failed\n");
    }

    cal_close();
    band_refresh();          /* 让表盘立刻换到新时间，不必等下一秒 */
}


static void cal_ok_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        cal_apply();
    }
}


static void cal_cancel_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        rt_kprintf("calib: cancelled, rtc untouched\n");
        cal_close();
    }
}


/* 小工具：造一个样式完全由我们指定的圆角方块，里面居中放一行字。
 * 不用主题（proj.conf 里 LV_USE_THEME_DEFAULT/BASIC 都没开），
 * 所以每个属性都自己写死，不依赖任何默认值。 */
static lv_obj_t *cal_mkbtn(lv_obj_t *par, const char *text, lv_coord_t x, lv_coord_t y,
                           lv_coord_t w, lv_coord_t h, uint16_t font,
                           lv_color_t bg, lv_color_t fg, lv_color_t border)
{
    lv_obj_t *btn = lv_btn_create(par);
    lv_obj_t *lbl;

    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_radius(btn, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(btn, border, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, bg, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_ext_set_local_font(lbl, font, fg);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

    return btn;
}


static void cal_create(void)
{
    app_clock_band_t *b = p_clk_band;
    lv_obj_t *o;
    int i;

    if (b == NULL || b->cal_mask != NULL)
    {
        return;
    }

    /* 遮罩：全屏、压暗、可点。可点这一点是必须的 —— 它要吃掉所有落在面板
     * 外面的点按，否则调时间的时候会顺手把表盘的 tileview 划走。 */
    b->cal_mask = lv_obj_create(lv_scr_act());
    lv_obj_set_size(b->cal_mask, WF_SCR_W, WF_SCR_H);
    lv_obj_set_pos(b->cal_mask, 0, 0);
    lv_obj_set_style_pad_all(b->cal_mask, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(b->cal_mask, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(b->cal_mask, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(b->cal_mask, CAL_C_MASK, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(b->cal_mask, 190, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(b->cal_mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b->cal_mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(b->cal_mask, LV_OBJ_FLAG_HIDDEN);

    b->cal_panel = lv_obj_create(b->cal_mask);
    lv_obj_set_size(b->cal_panel, CAL_W, CAL_H);
    lv_obj_align(b->cal_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_pad_all(b->cal_panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(b->cal_panel, 18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(b->cal_panel, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(b->cal_panel, CAL_C_LINE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(b->cal_panel, CAL_C_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(b->cal_panel, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(b->cal_panel, LV_OBJ_FLAG_SCROLLABLE);

    o = lv_label_create(b->cal_panel);
    lv_label_set_text(o, "设置时间");
    lv_ext_set_local_font(o, FONT_SUBTITLE, CAL_C_TEXT);
    lv_obj_align(o, LV_ALIGN_TOP_MID, 0, 8);

    o = lv_label_create(b->cal_panel);
    lv_label_set_text(o, "点一项选中，再用下面 - + 调整");
    lv_ext_set_local_font(o, FONT_SMALL, CAL_C_DIM);
    lv_obj_align(o, LV_ALIGN_TOP_MID, 0, 38);

    /* 6 个字段：两列三行 */
    for (i = 0; i < CAL_FIELD_N; i++)
    {
        int col = i & 1;
        int row = i >> 1;

        b->cal_btn[i] = cal_mkbtn(b->cal_panel, cal_field_name[i],
                                  col ? CAL_F_X1 : CAL_F_X0,
                                  CAL_F_Y0 + row * CAL_F_DY,
                                  CAL_F_BW, CAL_F_BH, FONT_NORMAL,
                                  CAL_C_BTN, CAL_C_TEXT, CAL_C_LINE);
        b->cal_lbl[i] = lv_obj_get_child(b->cal_btn[i], 0);
        lv_obj_add_event_cb(b->cal_btn[i], cal_field_event_cb, LV_EVENT_CLICKED,
                            (void *)&cal_idx_tab[i]);
    }

    /* - / + ：用 LV_EVENT_ALL，因为要分别接住按下(开始连加)和松开(停止) */
    o = cal_mkbtn(b->cal_panel, "-", CAL_AB_X0, CAL_AB_Y, CAL_AB_W, CAL_AB_H,
                  FONT_TITLE, CAL_C_BTN, CAL_C_TEXT, CAL_C_LINE);
    lv_obj_add_event_cb(o, cal_adjust_event_cb, LV_EVENT_ALL, (void *)&cal_dir_tab[0]);

    o = cal_mkbtn(b->cal_panel, "+", CAL_AB_X1, CAL_AB_Y, CAL_AB_W, CAL_AB_H,
                  FONT_TITLE, CAL_C_BTN, CAL_C_TEXT, CAL_C_LINE);
    lv_obj_add_event_cb(o, cal_adjust_event_cb, LV_EVENT_ALL, (void *)&cal_dir_tab[1]);

    /* 取消 / 确定 */
    o = cal_mkbtn(b->cal_panel, "取消", CAL_AB_X0, CAL_OK_Y, CAL_OK_W, CAL_OK_H,
                  FONT_NORMAL, CAL_C_BTN, CAL_C_TEXT, CAL_C_LINE);
    lv_obj_add_event_cb(o, cal_cancel_event_cb, LV_EVENT_CLICKED, NULL);

    o = cal_mkbtn(b->cal_panel, "确定", CAL_AB_X1, CAL_OK_Y, CAL_OK_W, CAL_OK_H,
                  FONT_NORMAL, CAL_C_OK, CAL_C_TEXT, CAL_C_OK);
    lv_obj_add_event_cb(o, cal_ok_event_cb, LV_EVENT_CLICKED, NULL);

    /* 连加定时器：先建好、先停住，按下去时再跑 */
    b->cal_rep = lv_timer_create(cal_repeat_cb, CAL_REP_FIRST_MS, RT_NULL);

    if (b->cal_rep != NULL)
    {
        lv_timer_pause(b->cal_rep);
    }
}


static void cal_open(void)
{
    app_clock_band_t *b = p_clk_band;
    time_t now;
    struct tm *t;

    if (b == NULL || b->cal_mask == NULL)
    {
        rt_kprintf("calib: panel not created (还在表盘之外?)\n");
        return;
    }

    /* 起点永远是 RTC 的当前值 —— 包括那个兜底的 2024-01-01 12:00。
     * 用户从哪儿开始调都行，至少不会跳到一个凭空的默认值上。 */
    time(&now);
    t = localtime(&now);

    if (t != RT_NULL)
    {
        b->cal_val[CAL_YEAR] = t->tm_year + 1900;
        b->cal_val[CAL_MON]  = t->tm_mon + 1;
        b->cal_val[CAL_DAY]  = t->tm_mday;
        b->cal_val[CAL_HOUR] = t->tm_hour;
        b->cal_val[CAL_MIN]  = t->tm_min;
        b->cal_val[CAL_SEC]  = t->tm_sec;
    }
    else
    {
        b->cal_val[CAL_YEAR] = 2026;
        b->cal_val[CAL_MON]  = 1;
        b->cal_val[CAL_DAY]  = 1;
        b->cal_val[CAL_HOUR] = 0;
        b->cal_val[CAL_MIN]  = 0;
        b->cal_val[CAL_SEC]  = 0;
    }

    b->cal_sel = CAL_YEAR;      /* 没校过时，最先要改的就是「年」 */

    cal_render();

    lv_obj_clear_flag(b->cal_mask, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(b->cal_mask);    /* 状态栏是同一层里更早建的，得抬上来 */
    b->cal_open = 1;

    rt_kprintf("calib: panel open, start %04d-%02d-%02d %02d:%02d:%02d\n",
               b->cal_val[CAL_YEAR], b->cal_val[CAL_MON], b->cal_val[CAL_DAY],
               b->cal_val[CAL_HOUR], b->cal_val[CAL_MIN], b->cal_val[CAL_SEC]);
}


static void cal_close(void)
{
    app_clock_band_t *b = p_clk_band;

    if (b == NULL || b->cal_mask == NULL)
    {
        return;
    }

    if (b->cal_rep != NULL)
    {
        lv_timer_pause(b->cal_rep);
    }

    lv_obj_add_flag(b->cal_mask, LV_OBJ_FLAG_HIDDEN);
    b->cal_open = 0;
    rt_kprintf("calib: panel closed\n");
}


/* ── 对外的入口：给上划面板的「手动校时」按钮用 ──
 *
 * 为什么需要它：cal_open() 是 static，而"手动校准"这件事以前只有一个入口 ——
 * 表盘上那行「未校时 · 点这里校准」的提示（cal_hint_event_cb）。
 * 一旦时间已经校准过，那行提示就消失了，用户就再也找不到校准入口
 * （用户原话：「我不知道在哪里校准」）。所以把它固定到面板按钮上。
 *
 * 安全性：cal_mask 是建在 lv_scr_act() 上的全屏遮罩，和状态栏同一个屏幕；
 * 而本 app（手环风表盘）是第一个注册的、开机默认表盘，所以只要表盘在，
 * cal_mask 就在。万一不在，cal_open() 自己会判断 NULL 并打印
 * "panel not created" 后返回，不会崩。 */
void app_clock_band_open_calib(void)
{
    cal_open();
}


static void cal_hint_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        rt_kprintf("calib: hint tapped -> opening panel\n");
        cal_open();
    }
}


/* RTC 里是不是还没被校过。判断口径必须和 band_refresh() 里挂提示的那一套
 * 完全一致（年份 < RTC_SET_MIN_YEAR），否则会出现"提示没了但还自动弹面板"。 */
static rt_bool_t rtc_is_unset(void)
{
    time_t now = time(RT_NULL);
    struct tm *t = localtime(&now);

    if (t == RT_NULL)
    {
        return RT_TRUE;
    }

    return ((t->tm_year + 1900) < RTC_SET_MIN_YEAR) ? RT_TRUE : RT_FALSE;
}


#ifdef RT_USING_FINSH
/* 串口侧的手脚。
 *
 * 屏幕我看不见，所以按钮之外再留一套等价入口：这几条命令调用的就是按钮
 * 回调用的那几个函数（cal_step / cal_render / cal_apply），不是另写一套逻辑，
 * 这样"串口验证通过"才真的等于"按钮也是好的"。
 *
 * 另外留了一条 check 用来直接调 rtc_is_unset()，方便在"该不该弹面板"上对账。
 *
 * 函数名就叫 calib：MSH_CMD_EXPORT(fn, desc) 注册的**是函数名本身**，不叫
 * cmd_calib 就会变成要用 cmd_calib 才能敲出来。上一轮在 rtc_now 上刚踩过一次，
 * 这里不再踩第二次。 */
static int calib(int argc, char **argv)
{
    app_clock_band_t *b = p_clk_band;
    const char *sub = (argc > 1) ? argv[1] : "open";

    if (b == NULL)
    {
        rt_kprintf("calib: watchface not running\n");
        return -1;
    }

    if (0 == rt_strcmp(sub, "open"))
    {
        cal_open();
    }
    else if (0 == rt_strcmp(sub, "close"))
    {
        cal_close();
    }
    else if (0 == rt_strcmp(sub, "inc"))
    {
        cal_step(b->cal_sel, +1);
        cal_render();
    }
    else if (0 == rt_strcmp(sub, "dec"))
    {
        cal_step(b->cal_sel, -1);
        cal_render();
    }
    else if (0 == rt_strcmp(sub, "ok"))
    {
        cal_apply();
        return 0;
    }
    else if (0 == rt_strcmp(sub, "check"))
    {
        rt_kprintf("calib: rtc_is_unset=%d  auto_shown=%d  open=%d\n",
                   (int)rtc_is_unset(), (int)cal_auto_shown, (int)b->cal_open);
        return 0;
    }
    else if (0 == rt_strcmp(sub, "sel") && argc > 2 && argv[2][0] >= '0' && argv[2][0] <= '5')
    {
        b->cal_sel = (int8_t)(argv[2][0] - '0');
        cal_render();
    }
    else
    {
        rt_kprintf("calib: open | close | sel <0..5> | inc | dec | ok | check\n");
        return -1;
    }

    rt_kprintf("calib: sel=%d  [%d %d %d %d %d %d]  open=%d\n",
               b->cal_sel,
               b->cal_val[CAL_YEAR], b->cal_val[CAL_MON], b->cal_val[CAL_DAY],
               b->cal_val[CAL_HOUR], b->cal_val[CAL_MIN], b->cal_val[CAL_SEC],
               (int)b->cal_open);

    /* 把 LVGL 里那个 label 对象的文字读回来。字模有没有、画不画得出来我看不见，
     * 但"label 到底收没收到这串字"是能从这儿确认的 —— 屏幕上那几个数字是
     * 否真的被赋值，就靠这一行。 */
    if (b->cal_lbl[b->cal_sel] != NULL)
    {
        rt_kprintf("calib: label[%d] = \"%s\"\n", b->cal_sel,
                   lv_label_get_text(b->cal_lbl[b->cal_sel]));
    }

    return 0;
}
MSH_CMD_EXPORT(calib, time setting panel: open|close|sel|inc|dec|ok|check);
#endif /* RT_USING_FINSH */


static rt_int32_t init(lv_obj_t *parent)
{
    int i;
    lv_obj_t *root;

    p_clk_band = (app_clock_band_t *) rt_malloc(sizeof(app_clock_band_t));
    RT_ASSERT(p_clk_band != RT_NULL);
    memset(p_clk_band, 0, sizeof(app_clock_band_t));

    /* 先强制刷一次初值（0xFF 不等于任何真实时分秒/日期） */
    p_clk_band->last_hour = 0xFF;
    p_clk_band->last_min  = 0xFF;
    p_clk_band->last_day  = 0xFF;
    p_clk_band->last_unset = 0xFF;

    /* 所有坐标都放在自己这个 390x450 的容器里量，不受 tileview 的样式影响 */
    root = lv_obj_create(parent);
    lv_obj_set_size(root, WF_SCR_W, WF_SCR_H);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(root, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(root, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    /* 1. 背景图 —— 想换背景，就替换工程里的 wf_bg.png 再编译 */
    p_clk_band->bg = lv_img_create(root);
    lv_img_set_src(p_clk_band->bg, LV_EXT_IMG_GET(wf_bg));
    lv_obj_set_pos(p_clk_band->bg, 0, 0);

#if WF_USE_SCRIM
    /* 2. 压暗层：垫在背景之上、文字之下 */
    p_clk_band->scrim = lv_img_create(root);
    lv_img_set_src(p_clk_band->scrim, LV_EXT_IMG_GET(wf_scrim));
    lv_obj_set_pos(p_clk_band->scrim, 0, 0);
#endif

    /* 3. 大号时间 */
    p_clk_band->hour[0]   = lv_img_create(root);
    p_clk_band->hour[1]   = lv_img_create(root);
    p_clk_band->colon     = lv_img_create(root);
    p_clk_band->minute[0] = lv_img_create(root);
    p_clk_band->minute[1] = lv_img_create(root);

    /* 冒号必须显式设图源：lv_img_create() 出来的是个空对象，不给 src 就什么都不画。
     * （上一版漏了这一句，屏幕上的表现是「没有冒号」—— 对象在、位置也对，就是没内容。）*/
    lv_img_set_src(p_clk_band->colon, LV_EXT_IMG_GET(wf_colon));

    lv_obj_set_pos(p_clk_band->hour[0],   WF_X_DIGIT0, WF_TIME_Y);
    lv_obj_set_pos(p_clk_band->hour[1],   WF_X_DIGIT1, WF_TIME_Y);
    lv_obj_set_pos(p_clk_band->colon,     WF_X_COLON,  WF_TIME_Y);
    lv_obj_set_pos(p_clk_band->minute[0], WF_X_DIGIT2, WF_TIME_Y);
    lv_obj_set_pos(p_clk_band->minute[1], WF_X_DIGIT3, WF_TIME_Y);

    /* 4. 日期行：先造齐槽位并全部藏起来，等 band_refresh() 按真实日期决定谁上场 */
    for (i = 0; i < 6; i++)
    {
        p_clk_band->date[i] = lv_img_create(root);
        lv_obj_add_flag(p_clk_band->date[i], LV_OBJ_FLAG_HIDDEN);
    }

    p_clk_band->week = lv_img_create(root);
    lv_obj_add_flag(p_clk_band->week, LV_OBJ_FLAG_HIDDEN);

    /* 5. 「未校时」提示（RTC 没校过时才现身，见 RTC_SET_MIN_YEAR 的说明）
     *
     * 这一条必须用字体渲染，不能用那套 PNG 数字贴图：贴图只有 0-9 和年月日，
     * 汉字是排不出来的。屏幕上的中文走的是 FreType 那条路，
     * 字模覆盖情况已用 watchface-tools/check_font_cjk.py 验过，字齐。
     *
     * 位置靠 align 贴屏幕底边、用负偏移往上抬一点，故意不写死 y ——
     * 表盘几何常量都在 wf_layout.h 里由脚本生成，写死 y 的话以后调排版就会错位。 */
    p_clk_band->unset_hint = lv_label_create(root);
    /* 文案是"点这里校准"而不是"请重新对时"：光说"该校准了"不告诉人去哪儿，
     * 就是上一轮的原话"有未校准字样，但是我不知道在哪里校准"。
     * 一句话里把动作说清楚。 */
    lv_label_set_text(p_clk_band->unset_hint, "未校时 · 点这里校准");
    lv_ext_set_local_font(p_clk_band->unset_hint, FONT_NORMAL,
                          lv_color_make(0xFF, 0xC1, 0x07));   /* 琥珀色：提醒，不是报错 */
    lv_obj_align(p_clk_band->unset_hint, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_add_flag(p_clk_band->unset_hint, LV_OBJ_FLAG_HIDDEN);   /* band_refresh 决定它出不出现 */

    /* 这条提示同时也是「校时面板」的入口：点它就把面板叫出来。
     * 只提示不给路，等于把人晾在那儿 —— 上一轮反馈的原话就是
     * "有未校准字样，但是我不知道在哪里校准"。 */
    lv_obj_add_flag(p_clk_band->unset_hint, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(p_clk_band->unset_hint, cal_hint_event_cb, LV_EVENT_CLICKED, NULL);
    /* 文字原来只有一行那么点大，手指很难点中；用 padding 把可点区域撑大，
     * 然后再居中一次（padding 会改变对象尺寸）。 */
    lv_obj_set_style_pad_hor(p_clk_band->unset_hint, 26, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_ver(p_clk_band->unset_hint, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(p_clk_band->unset_hint, LV_ALIGN_BOTTOM_MID, 0, -24);

    /* 6. 右上角电池指示（见文件里 BAT_* 那一段的说明） */
#if BAT_SHOW
    bat_create(root);

    /* 让第一次 band_refresh 马上就读一次电压，而不是开机先空 10 秒。
     * last 记成 -2：和任何真实值都不一样（真实值是 0..100 / 0..3），
     * 保证首帧一定重画一次，不用靠"初值恰好不同"这种运气。 */
    p_clk_band->bat_sec      = BAT_PERIOD_S;
    p_clk_band->bat_pct_last = -2;
    p_clk_band->bat_st_last  = -2;
#endif

    /* 7. 校时面板：先建好藏起来，cal_open() 才显示 */
    cal_create();

    p_clk_band->tick = NULL;

    return RT_EOK;
}


static rt_int32_t deinit(void)
{
    if (p_clk_band != NULL)
    {
        if (p_clk_band->tick != NULL)
        {
            lv_timer_del(p_clk_band->tick);
            p_clk_band->tick = NULL;
        }

        if (p_clk_band->cal_rep != NULL)
        {
            lv_timer_del(p_clk_band->cal_rep);
            p_clk_band->cal_rep = NULL;
        }

        /* 遮罩挂在 lv_scr_act() 上，不在表盘自己的 page 里，所以
         * app_clock_main.c 里那句 lv_obj_clean(page) 清不到它 —— 必须自己删。
         * 不删的话，切到别的 app 之后屏幕上会永远压着一层点不动的灰。 */
        if (p_clk_band->cal_mask != NULL)
        {
            lv_obj_del(p_clk_band->cal_mask);
            p_clk_band->cal_mask = NULL;
        }

        p_clk_band->cal_panel = NULL;
        p_clk_band->cal_open = 0;

        rt_free(p_clk_band);
        p_clk_band = NULL;
    }

    return RT_EOK;
}


static const app_clock_ops_t ops =
{
    .init   = init,
    .pause  = pause_callback,
    .resume = resume_callback,
    .deinit = deinit,
};


void app_clock_band_register(void)
{
    app_clock_register("band", &ops);
}
