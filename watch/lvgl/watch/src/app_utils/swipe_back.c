/*
 * swipe_back.c —— 右边缘「从右往左滑」→ 触发返回上一页
 *
 * ── 这一版的核心变化：只做手势识别，不做动画 ──
 *
 * 手指在右边缘从右往左划 → 松手时判定（位移够远 / 甩得够快）→ gui_app_goback()。
 * 画面不再跟手；返回那一动画由框架自己播（详见下面"返回动画从哪来"）。
 *
 * ── 为什么放弃「跟手」：上一版真机卡死的根因 ──
 *
 * 上一版用框架的手动转场接口让画面跟着手指走：
 *     gui_app_manual_animation_start(0) / update(process) / stop(process)
 * 这是个"框架搭台、应用驱动"的半成品状态机。它的收尾语义有两条
 * （见 middleware/app_fwk/app_trans_anim_port.c:56）：
 *
 *     end_value != 0  →  TRANS_RES_FINISHED   真的切页
 *     end_value == 0  →  TRANS_RES_ABORTED    "取消"，要回滚
 *
 * 而 ABORTED 的结束回调会把页面状态**回滚**（app_schedule.c:1336）：
 *     page_set_target_state(trans_anim_pg_exit,  page_st_resumed);  // 回出发页
 *     page_set_target_state(trans_anim_pg_enter, page_st_paused);   // 暂停目标页
 *     app_resume(&trans_anim_pg_exit->parent->node);               // 恢复出发 app
 *
 * 麻烦在于：手势「没划够 → 弹回」天然就是 end_value==0，也就是 ABORTED，
 * 而此时被取消的那次返回**已经执行过了**（返回消息在 handler 里就把 app 退了）。
 * 于是框架先恢复了蜂窝，又被回调改成"暂停蜂窝、恢复计算器" —— 状态错位：
 * 蜂窝画面还在，但框架认为它是 paused，点哪都没反应。
 * 这正是「通过这种方式返回后蜂窝界面没反应」的来历。
 *
 * 说白了：手动跟手这条路的 abort 语义，和"手势可以中途放弃"天生冲突。
 * 绕开它最干净的办法就是不用它 —— 手势只决定"要不要返回"，
 * 返回动作交给 gui_app_goback()，走框架自己那套 free-run 转场：
 * 有确定的结束值、必定走完、必定 clean（app_trans_anim_clean）。
 *
 * ── 顺手修掉的另一个隐患 ──
 *
 * 上一版还在滑动开始时用 gui_app_set_enter_trans_anim() /
 * gui_app_set_exit_trans_anim() / gui_app_set_trans_anim_prio() 去"临时"改
 * 当前页的转场配置。这几个接口写的是**当前页**的配置：站在蜂窝上滑就会改到
 * 蜂窝自己的配置，而且不会自动还原。现在完全不碰这些接口。
 *
 * ── 返回动画从哪来 ──
 *
 * 蜂窝 on_pause 里 mm_trans_anim_init() 配的 CUST_ANIM_TYPE_PUSH
 * （src/gui_apps/utils/custom_trans_anim.c）：水平平移 + ease-out ——
 * 进入 app 时页面向左推、返回时向右退。进/出共用一套配置，所以两个方向
 * 都一定是平移，不会再出现原厂那套旋转/缩放的"随机演示动画"。
 *
 * ── 三条判定线 ──
 *   dx <  SB_MIN_DX(8px)             当误碰，什么都不做
 *   dx >= 屏宽/3(≈130px)             松手即返回
 *   dx >= 60px 且 >= 0.8px/ms        快甩，松手即返回（不必划过半屏）
 *
 * ── 为什么松手才触发，而不是划到阈值就触发 ──
 *
 * 框架在转场开始时会复位整个触摸设备
 * （gui_app_trans_anim.c:479 → port_app_sche_reset_indev → lv_indev_reset(NULL,NULL)）。
 * 若手指还按在屏幕上，复位之后 LVGL 会把"当前还按着"当成一次全新的按下、
 * 重新做命中测试 —— 那一刻手指多半已经在屏幕中间，正好落在蜂窝某个图标上，
 * 松手就会把那个 app 点开。改成松手触发就完全没有这个问题；
 * 手感上的即时反馈由右边缘那条指示条负责。
 *
 * ── 右边缘指示条 ──
 *
 * 一条 5×56 的小竖条，默认全透明。手指在右边缘划起来时出现，
 * 越过判定线时变蓝、变实、并跟着手指往左挪一点（最多 18px）。
 * 它只是挂在 lv_layer_top() 上的一个非点击装饰对象：
 * 不参与命中测试，不持有触摸，也不会碰任何页面。
 *
 * ── 蜂窝页要禁用 ──
 * 蜂窝是根页面，没有上一级；滑它会让框架去 app_run_by_id(main) /
 * app_restart_last_suspend()，行为不可预期。所以 app_mainmenu.c 在 on_resume
 * 里关掉、on_pause 里打开。关掉时连 CLICKABLE 一起清 —— 否则右边缘这 16px
 * 会变成触摸死区（检测条压在蜂窝图标上面把点击吃掉）。
 *
 * ── 改动范围 ──
 *   本文件 + src/app_utils/SConscript 一行 + watch_demo.c 调一次 init
 *   + app_mainmenu.c 两行开关。**SDK 一行未动**（上一轮改的已还原）。
 */

#include <rtthread.h>
#include <string.h>
#include <stdlib.h>     /* atoi：串口自检命令要用 */
#include <stdbool.h>
#include "lvgl.h"
#include "gui_app_fwk.h"
#include "swipe_back.h"

/* ── 可调参数 ─────────────────────────────────────────────────────────── */

/* 右边缘检测条宽度(px)。
 * 取 16 而不是 SDK 左侧的 20，是因为计算器键盘最右一列按键的右边缘在 x=374，
 * 屏幕宽 390 —— 16px 的检测条正好落在 374~390 这段空白里，一个按键都挡不到。 */
#define SB_EDGE_W        16

/* 超过它才算"一次拖动"（指示条出现）。只用来滤噪声，不代表会返回。 */
#define SB_MIN_DX        8

/* 判定返回的位移阈值 = 屏宽 / SB_TRIGGER_DIV。
 * 取 3 是跟着手势类交互的惯例（Android 边缘返回也是 1/3 屏左右）：
 * 手指从右边缘往里划，划过约 130px 就认定"这个人是想返回"。 */
#define SB_TRIGGER_DIV   3

/* 快甩判定：位移够长 + 速度够快，没划过 1/3 屏也认。 */
#define SB_FLING_MIN_DX  60
#define SB_FLING_V_X10   8       /* 0.8 px/ms */

/* 指示条：宽 × 高、跟随手指的最大左移量、两种状态的透明度。 */
#define SB_IND_W         5
#define SB_IND_H         56
#define SB_IND_FOLLOW_MAX 18
#define SB_IND_OPA_IDLE  90
#define SB_IND_OPA_ARMED 230

/* ── 内部状态 ─────────────────────────────────────────────────────────── */

typedef enum
{
    SB_EVT_DOWN,        /* 手指按下（右边缘） */
    SB_EVT_MOVE,        /* 拖动中 */
    SB_EVT_UP,          /* 抬起 / 丢失焦点 */
} sb_evt_t;

static lv_obj_t   *sb_bar       = NULL;
static lv_obj_t   *sb_ind       = NULL;   /* 右边缘指示条 */
static bool        sb_enabled   = true;
static bool        sb_ind_on     = true;  /* 指示条开关（串口可关，对比手感） */
static bool        sb_debug     = false;  /* 打开后每次触摸事件打一行 */
static bool        sb_tracking  = false;  /* 本次按下序列是否在跟踪中 */
static lv_coord_t  sb_x0        = 0;      /* 按下点（屏幕绝对 x） */
static lv_coord_t  sb_dx        = 0;      /* 最近一次往左的位移 */
static rt_tick_t   sb_t_down    = 0;      /* 按下时刻，算甩动速度用 */
static rt_tick_t   sb_t_move    = 0;      /* 最近一次移动的时刻 */
static int         sb_triggers  = 0;      /* 触发次数（诊断：确认判定链路真的走过） */

/* ── 阈值 ─────────────────────────────────────────────────────────────── */

static lv_coord_t sb_trigger_dx(void)
{
    lv_coord_t hor = lv_disp_get_hor_res(NULL);
    return (hor > 0) ? (hor / SB_TRIGGER_DIV) : 130;
}

/* 快速轻甩：位移够长 + 速度够快，没过 1/3 屏也返回。 */
static bool sb_is_fling(lv_coord_t dx)
{
    uint32_t   dt_ms;
    rt_tick_t  dt;

    if (dx < SB_FLING_MIN_DX) return false;

    dt = sb_t_move - sb_t_down;
    if (dt == 0) return true;

    dt_ms = (uint32_t)dt * 1000 / RT_TICK_PER_SECOND;
    if (dt_ms == 0) return true;

    /* 速度(px/ms) ≥ SB_FLING_V_X10/10 → 改写成分式，避免浮点 */
    return ((uint32_t)dx * 10 >= (uint32_t)SB_FLING_V_X10 * dt_ms);
}

/* ── 指示条 ───────────────────────────────────────────────────────────── */

/* 显示 / 隐藏。用整体透明度而不是 HIDDEN，这样能有个"半亮"的中间态。 */
static void sb_ind_show(bool on)
{
    if (!sb_ind || !sb_ind_on) return;
    lv_obj_set_style_opa(sb_ind, on ? SB_IND_OPA_IDLE : LV_OPA_TRANSP, 0);
}

/* 拖动中刷新：跟随位移 + 越过判定线后变色。
 * 注意这条竖条只是给手指的反馈，和页面无关 —— 页面的位移是零。 */
static void sb_ind_update(lv_coord_t dx, bool armed)
{
    lv_coord_t hor, follow;

    if (!sb_ind || !sb_ind_on) return;
    if (dx < SB_MIN_DX) { sb_ind_show(false); return; }

    hor    = lv_disp_get_hor_res(NULL);
    follow = dx / 6;
    if (follow > SB_IND_FOLLOW_MAX) follow = SB_IND_FOLLOW_MAX;

    lv_obj_set_x(sb_ind, hor - SB_IND_W - follow);
    lv_obj_set_style_opa(sb_ind, armed ? SB_IND_OPA_ARMED : SB_IND_OPA_IDLE, 0);
    lv_obj_set_style_bg_color(sb_ind,
                              armed ? lv_color_hex(0x3B82F6) : lv_color_hex(0xFFFFFF),
                              0);
}

/* ── 触发返回 ─────────────────────────────────────────────────────────── */

/* 这是本模块唯一对外产生的动作。
 *
 * gui_app_goback() 发的是框架的标准 GUI_APP_MSG_GOBACK（不是 MANUAL_ 那条），
 * 于是走的是框架原厂、非手动的转场：结束值固定、必定走完、必定清理。
 * 我们不再需要关心动画的任何中间状态。 */
static void sb_trigger(lv_coord_t dx, bool fling)
{
    sb_triggers++;

    rt_kprintf("swipe: TRIGGER #%d dx=%d fling=%d -> gui_app_goback()\n",
               sb_triggers, (int)dx, (int)fling);

    /* 先把指示条收掉：转场一开始就会把整屏(含 top 层之上)截进快照，
     * 别让这条竖条留在返回动画的起手上。 */
    sb_ind_show(false);

    gui_app_goback();
}

/* ── 状态机（与触摸解耦，所以能用串口命令直接喂序列做自检）───────────── */

static void sb_reset_gesture(void)
{
    sb_tracking = false;
    sb_dx       = 0;
    sb_ind_show(false);
}

static void sb_feed(sb_evt_t evt, lv_coord_t x)
{
    lv_coord_t dx;

    if (!sb_enabled)
    {
        /* 禁用时只清状态，不产生任何动作 */
        if (evt == SB_EVT_DOWN) sb_reset_gesture();
        return;
    }

    switch (evt)
    {
    case SB_EVT_DOWN:
        sb_x0       = x;
        sb_dx       = 0;
        sb_t_down   = rt_tick_get();
        sb_t_move   = sb_t_down;
        sb_tracking = true;
        break;

    case SB_EVT_MOVE:
        if (!sb_tracking) return;

        sb_t_move = rt_tick_get();

        dx = sb_x0 - x;                 /* 往左为正 */
        if (dx <= 0) return;            /* 只认「从右往左」；往右滑不理会 */
        sb_dx = dx;

        /* 只做视觉反馈，不动页面 */
        sb_ind_update(dx, dx >= sb_trigger_dx());
        break;

    case SB_EVT_UP:
        if (sb_tracking)
        {
            bool go    = (sb_dx >= sb_trigger_dx()) || sb_is_fling(sb_dx);
            bool fling = sb_is_fling(sb_dx);

            if (sb_debug)
                rt_kprintf("swipe: RELEASE dx=%d need=%d fling=%d -> %s\n",
                           (int)sb_dx, (int)sb_trigger_dx(), (int)fling,
                           go ? "GO-BACK" : "cancel");

            if (go) sb_trigger(sb_dx, fling);
        }
        sb_reset_gesture();
        break;

    default:
        break;
    }
}

/* ── LVGL 事件入口（真实触摸走这里） ──────────────────────────────────── */

static void sb_event_cb(lv_event_t *e)
{
    lv_event_code_t code  = lv_event_get_code(e);
    lv_indev_t     *indev = lv_indev_get_act();
    lv_point_t      p;

    if (!sb_enabled) return;

    if (code == LV_EVENT_PRESSED)
    {
        if (!indev) return;
        lv_indev_get_point(indev, &p);
        if (sb_debug) rt_kprintf("swipe: touch DOWN  x=%d y=%d\n", p.x, p.y);
        sb_feed(SB_EVT_DOWN, p.x);
    }
    else if (code == LV_EVENT_PRESSING)
    {
        if (!indev) return;
        lv_indev_get_point(indev, &p);
        if (sb_debug) rt_kprintf("swipe: touch MOVE  x=%d dx=%d\n", p.x, sb_x0 - p.x);
        sb_feed(SB_EVT_MOVE, p.x);
    }
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
    {
        if (indev)
        {
            lv_indev_get_point(indev, &p);
            if (sb_debug)
                rt_kprintf("swipe: touch UP    x=%d dx=%d code=%d\n",
                           p.x, sb_x0 - p.x, (int)code);
            sb_feed(SB_EVT_UP, p.x);
        }
        else
        {
            sb_feed(SB_EVT_UP, sb_x0);
        }
    }
}

/* ── 对外接口 ─────────────────────────────────────────────────────────── */

void swipe_back_init(void)
{
    lv_obj_t *top;

    if (sb_bar) return;                 /* 幂等 */

    top = lv_layer_top();
    if (!top) return;

    /* ── 检测条 ── */
    sb_bar = lv_obj_create(top);
    lv_obj_set_size(sb_bar, SB_EDGE_W, lv_disp_get_ver_res(NULL));
    lv_obj_align(sb_bar, LV_ALIGN_RIGHT_MID, 0, 0);

    /* 完全隐形：不要背景、不要边框、不要内边距、不要圆角 */
    lv_obj_set_style_bg_opa(sb_bar, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(sb_bar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(sb_bar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(sb_bar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* 关键：明确不让它成为滚动对象。
     * 一旦 LVGL 把这条 bar 选为 scroll_obj，滚动处理会插进 PRESSING 之间，
     * 并且让 lv_indev.c 里的手势通道整体失效。我们只要原始 PRESSING 流。 */
    lv_obj_clear_flag(sb_bar, LV_OBJ_FLAG_SCROLLABLE);

    /* 按下后把焦点锁在这条 bar 上：手指往左划出 16px 检测带之后，
     * PRESSING 仍然会送给我们，位移才测得到完整行程。 */
    lv_obj_add_flag(sb_bar, LV_OBJ_FLAG_PRESS_LOCK);

    lv_obj_add_event_cb(sb_bar, sb_event_cb, LV_EVENT_ALL, NULL);

    /* ── 指示条 ── */
    sb_ind = lv_obj_create(top);
    lv_obj_set_size(sb_ind, SB_IND_W, SB_IND_H);
    lv_obj_set_x(sb_ind, lv_disp_get_hor_res(NULL) - SB_IND_W);
    lv_obj_set_y(sb_ind, (lv_disp_get_ver_res(NULL) - SB_IND_H) / 2);

    lv_obj_set_style_radius(sb_ind, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(sb_ind, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(sb_ind, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(sb_ind, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(sb_ind, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(sb_ind, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* 纯装饰：不能吃触摸、不能滚、不能参与布局 */
    lv_obj_clear_flag(sb_ind, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(sb_ind, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(sb_ind, LV_OBJ_FLAG_SCROLL_CHAIN);

    sb_ind_show(false);                 /* 平时全透明 */
}

void swipe_back_set_enabled(bool enable)
{
    if (sb_enabled == enable) return;
    sb_enabled = enable;

    sb_reset_gesture();

    if (!sb_bar) return;

    /* 禁用时把 CLICKABLE 一起清掉 —— 否则这条 16px 的条会继续压在页面上
     * 把右边缘的触摸吃掉（蜂窝最右一列的图标会点不动）。 */
    if (enable)
    {
        lv_obj_add_flag(sb_bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(sb_bar, LV_OBJ_FLAG_PRESS_LOCK);
    }
    else
    {
        lv_obj_clear_flag(sb_bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(sb_bar, LV_OBJ_FLAG_PRESS_LOCK);
    }
}

/* ── 串口自检 ─────────────────────────────────────────────────────────── */

/* 状态报告：确认检测条建出来了、几何和参数对不对 */
static int swipe_info(int argc, char **argv)
{
    lv_coord_t hor = lv_disp_get_hor_res(NULL);
    lv_coord_t ver = lv_disp_get_ver_res(NULL);

    if (!sb_bar)
    {
        rt_kprintf("swipe: bar NOT created (call swipe_back_init first)\n");
        return -1;
    }

    rt_kprintf("swipe: bar x=%d..%d y=%d..%d (%dx%d)  screen %dx%d  enabled=%d edge=%dpx min_dx=%dpx\n",
               sb_bar->coords.x1, sb_bar->coords.x2,
               sb_bar->coords.y1, sb_bar->coords.y2,
               lv_obj_get_width(sb_bar), lv_obj_get_height(sb_bar),
               hor, ver, (int)sb_enabled, SB_EDGE_W, SB_MIN_DX);
    rt_kprintf("swipe: trigger_dx=%dpx (hor/%d)  fling>=%dpx@%d.%dpx/ms  ind=%d  triggers=%d\n",
               (int)sb_trigger_dx(), SB_TRIGGER_DIV,
               SB_FLING_MIN_DX, SB_FLING_V_X10 / 10, SB_FLING_V_X10 % 10,
               (int)sb_ind_on, sb_triggers);
    return 0;
}
MSH_CMD_EXPORT(swipe_info, swipe_info : dump the right-edge swipe bar geometry);

/* 命中测试：确认右边缘某个点真的会被这条 bar 接住，而不是被下面的控件抢走。
 * 例：swipe_hit 380 200  → 期望 BAR
 *     swipe_hit 200 200  → 期望 not-bar（屏幕中间不该被拦） */
static int swipe_hit(int argc, char **argv)
{
    lv_point_t p;

    if (argc < 3)
    {
        rt_kprintf("usage: swipe_hit <x> <y>\n");
        return -1;
    }
    if (!sb_bar)
    {
        rt_kprintf("swipe: bar NOT created\n");
        return -1;
    }

    p.x = (lv_coord_t)atoi(argv[1]);
    p.y = (lv_coord_t)atoi(argv[2]);

    rt_kprintf("swipe: hit(%d,%d) -> %s   [bar owns x %d..%d, clickable=%d, hidden=%d, enabled=%d]\n",
               p.x, p.y,
               lv_obj_hit_test(sb_bar, &p) ? "BAR" : "not-bar",
               sb_bar->coords.x1, sb_bar->coords.x2,
               (int)lv_obj_has_flag(sb_bar, LV_OBJ_FLAG_CLICKABLE),
               (int)lv_obj_has_flag(sb_bar, LV_OBJ_FLAG_HIDDEN),
               (int)sb_enabled);
    return 0;
}
MSH_CMD_EXPORT(swipe_hit, swipe_hit <x> <y> : hit-test a point against the swipe bar);

/* 打开后，手指碰右边缘会逐帧打印坐标 —— 想确认「手势到底有没有收到触摸」
 * 时先开这个，再滑一次，串口上应该出现 DOWN / MOVE / UP / TRIGGER 几行。 */
static int swipe_dbg(int argc, char **argv)
{
    if (argc >= 2)
    {
        sb_debug = (atoi(argv[1]) != 0);
    }
    rt_kprintf("swipe: debug=%d\n", (int)sb_debug);
    return 0;
}
MSH_CMD_EXPORT(swipe_dbg, swipe_dbg <0|1> : toggle per-event logging on the swipe bar);

/* 指示条开关：用来对比"有反馈"和"纯静默"两种手感。 */
static int swipe_ind(int argc, char **argv)
{
    if (argc >= 2)
    {
        sb_ind_on = (atoi(argv[1]) != 0);
        if (!sb_ind_on) sb_ind_show(false);
    }
    rt_kprintf("swipe: indicator=%d\n", (int)sb_ind_on);
    return 0;
}
MSH_CMD_EXPORT(swipe_ind, swipe_ind <0|1> : toggle the right-edge drag indicator);

/* 手动开关手势本身（蜂窝页的生命周期里会自动调用） */
static int swipe_on(int argc, char **argv)
{
    if (argc >= 2)
    {
        swipe_back_set_enabled(atoi(argv[1]) != 0);
    }
    rt_kprintf("swipe: enabled=%d triggers=%d\n", (int)sb_enabled, sb_triggers);
    return 0;
}
MSH_CMD_EXPORT(swipe_on, swipe_on <0|1> : enable/disable the right-edge back gesture);

/* 端到端：按真实节奏注入一次合成滑动，走完整状态机。
 *
 * swipe_sim <x_down> <x_up>
 *     swipe_sim 385 100  → 左滑 285px → 超过 1/3 屏 → 应触发返回
 *     swipe_sim 385 340  → 左滑  45px → 不够       → 应什么都不做
 *
 * 步间隔取 100ms：太密会让甩动判定误命中（合成滑动天然"快"），
 * 100ms/步 × 4 步 = 400ms，接近人手一次正常滑动的节奏。
 *
 * ⚠ 为什么绕一圈用 lv_async_call + lv_timer，而不是在命令里直接喂：
 *   app 框架的每个 API 开头都有 CHECK_CUR_RTOS_TASK()，展开是
 *   RT_ASSERT(test_mode || rt_thread_self() == host_rtos_task)
 *   （middleware/app_fwk/gui_app_fwk.c:40-42）。host_rtos_task 就是跑
 *   lv_timer_handler() 的那个 main 线程。MSH 命令跑在 shell 线程里，直接调用
 *   gui_app_goback() 会当场撞断言。所以先 lv_async_call 把活派回 GUI 线程，
 *   再用 lv_timer 分帧驱动。 */
static lv_timer_t *sb_sim_timer = NULL;
static int         sb_sim_step  = 0;
static lv_coord_t  sb_sim_x0    = 0;
static lv_coord_t  sb_sim_x1    = 0;

static void sb_sim_step_cb(lv_timer_t *t)
{
    if (sb_sim_step == 0)
    {
        rt_kprintf("swipe: sim DOWN x=%d\n", sb_sim_x0);
        sb_feed(SB_EVT_DOWN, sb_sim_x0);
    }
    else if (sb_sim_step <= 4)
    {
        lv_coord_t x = sb_sim_x0 + (sb_sim_x1 - sb_sim_x0) * sb_sim_step / 4;
        sb_feed(SB_EVT_MOVE, x);
        rt_kprintf("swipe: sim MOVE x=%d  dx=%d\n", x, sb_x0 - x);
    }
    else
    {
        lv_coord_t dx = sb_x0 - sb_sim_x1;

        rt_kprintf("swipe: sim UP x=%d  dx=%d  need=%d\n",
                   sb_sim_x1, dx, (int)sb_trigger_dx());
        sb_feed(SB_EVT_UP, sb_sim_x1);

        lv_timer_del(t);
        sb_sim_timer = NULL;
        return;
    }
    sb_sim_step++;
}

static void sb_sim_start_async(void *arg)
{
    if (sb_sim_timer)
    {
        lv_timer_del(sb_sim_timer);
        sb_sim_timer = NULL;
    }
    sb_sim_step  = 0;
    sb_sim_timer = lv_timer_create(sb_sim_step_cb, 100, NULL);
}

static int swipe_sim(int argc, char **argv)
{
    lv_coord_t dx;

    if (argc < 3)
    {
        rt_kprintf("usage: swipe_sim <x_down> <x_up>\n");
        return -1;
    }

    sb_sim_x0 = (lv_coord_t)atoi(argv[1]);
    sb_sim_x1 = (lv_coord_t)atoi(argv[2]);
    dx        = sb_sim_x0 - sb_sim_x1;

    rt_kprintf("swipe: queued x %d -> %d (dx_left=%d, need=%d, expect=%s)\n",
               sb_sim_x0, sb_sim_x1, dx, (int)sb_trigger_dx(),
               (dx >= sb_trigger_dx()) ? "GO-BACK" : "nothing");

    lv_async_call(sb_sim_start_async, NULL);
    return 0;
}
MSH_CMD_EXPORT(swipe_sim, swipe_sim <x_down> <x_up> : inject a synthetic right-to-left swipe);
