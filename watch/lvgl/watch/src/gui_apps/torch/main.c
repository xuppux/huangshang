/*
 * 手电筒 App（torch）
 * ==================
 *
 * 一个"把能发光的东西全打开"的 app。这块板子上有两个光源：
 *
 *   1. 屏幕本身 —— 白底 + 背光拉到 100%。这是**主光源**，面积大、真的亮。
 *   2. WS2812 RGB 灯（PA32）—— 板子背面那颗小灯，点光源。**辅助光源**。
 *
 * 所以这个 app 的核心不是"画一个好看的界面"，而是四件容易被忽略的事：
 *
 *   ⚠ 一、必须挡住自动息屏。
 *      watch_demo.c 里有个 30ms 一次的 lv_timer，只要
 *      lv_disp_get_inactive_time() > 10s 就 gui_pm_fsm(SLEEP) 息屏。
 *      手电筒是"开着不动"的用法，不挡的话 10 秒后自己黑掉。
 *      做法：起一个 1s 的 lv_timer 反复 lv_disp_trig_activity()（重置空闲计时）。
 *      1s 对 10s 阈值有 10 倍余量，且不用改 watch_demo.c 那套 PM 逻辑。
 *
 *   ⚠ 二、退出必须把背光恢复回去。
 *      进来时把 100% 设下去，走的时候不还，用户就会莫名其妙发现"屏幕变亮了"。
 *      所以进来先存旧值（app_watch_brightness_get），on_pause/on_stop 还回去。
 *
 *   ⚠ 三、WS2812 的三个前置条件必须**每次进入都重做**。
 *      灯珠是**片上 LDO3_3V3 供电**的，不先
 *      HAL_PMU_ConfigPeriLdo(PMU_PERI_LDO3_3V3, true, true) 就什么都不亮；
 *      PA32 在板级 bsp_pinmux.c 里默认被初始化成 GPIO_A32（下拉），
 *      要重新映射成 GPTIM2_CH1 才有脉动出来。
 *
 *      ⚠ 注意这里有个上一版埋的雷：最初用 s_led_ready 把整个 init 做成
 *      "只跑一次"。但息屏休眠（gui_pm_fsm(SLEEP)）会关掉外设 LDO，
 *      PA32 的复用状态也可能被 PM 复位 —— 于是**第二次进入 app 时
 *      设备还在（find 得到）、灯却不亮**。所以现在拆成两半：
 *        · find 设备 → 只做一次（设备是常驻注册的，不会消失）
 *        · LDO + pinmux → 每次点亮都重做（幂等、便宜，但漏了就全黑）
 *
 *   ⚠ 四、驱动的返回值不能丢。
 *      drv_rgbled_control() 的 PWM_CMD_SET_COLOR 内部会再走
 *      PWM_CMD_SET → PWM_CMD_ENABLE 两步，任一步失败都直接返回错误码。
 *      不看返回值就只能看到"串口全绿、灯不亮"。
 *      现在 led_hw_write() 把返回值记在 s_last_ret 里并打进日志。
 *
 * 界面刻意压到最少：整屏纯白，只有顶部一行标题、底部一行提示、中间一个
 * 模式胶囊。手电筒的界面越少白占的面积越小。
 *
 * 交互：
 *   轻触屏幕        = 在 常亮 → 频闪 → 关 之间循环
 *   右边缘往左滑    = 退出（滑动手势由 app_utils/swipe_back.c 提供）
 *
 * ── 为什么轻触不用 LV_EVENT_CLICKED，而是自己看 PRESSED/RELEASED ──
 *
 * CLICKED 是 LVGL 内部在一串条件都满足时才合成的（按下命中、没有被滚动接管、
 * 抬起与按下同对象……）。这条链上任何一环被别的对象插一脚就静默不触发，
 * 而且完全看不出来 —— 排查时只知道"点了没反应"。
 * 现在自己收 PRESSED / RELEASED：位移 < 24px 且时长 < 1.2s 就算一次轻触。
 * 好处是每次按下抬起都会打一行日志，"事件到没到"一眼可判，
 * 顺带把"手指划一下也算点击"这类误触一起挡掉。
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>
#include <stdlib.h>
#include "littlevgl2rtt.h"
#include "lvgl.h"
#include "lvsf_comp.h"
#include "gui_app_fwk.h"
#include "lv_ext_resource_manager.h"
#include "lv_ex_data.h"

/* ================================================================
 *  一、WS2812 是可选的
 *  proj.conf 没打开 RGB_SK6812MINI_HS_ENABLE 时，整套 LED 代码编译成空操作。
 *  这样"配置没生效"不会连累"屏幕手电筒"—— 手电筒至少还是亮的。
 * ================================================================ */
#if defined(RGB_SK6812MINI_HS_ENABLE)
    #define TORCH_HAS_LED   1
    #include "bf0_hal.h"
    #include "drivers/rt_drv_pwm.h"
    #include "drv_io.h"
#else
    #define TORCH_HAS_LED   0
#endif

/* 满白。
 *
 * 驱动 create_color_array() 是按 **8bit/通道** 解析的：
 *     red = (color & 0x00ff0000) >> 16
 * 然后把每个 bit 从高到低编码成 PWM 占空比（1→reg_high=18，0→reg_low=7）。
 * 所以 SDK 例程里那个 0x0f0f0f 其实只有 15/255 ≈ 6% 亮度
 * （当氛围灯够、当手电筒不够）。满白是 0xFFFFFF。
 *
 * 电流估算：单片 SK6812 满白约 60mA，LDO3_3V3 带得动。
 * 嫌刺眼 / 想省电就往下调这个宏，例如 0x404040 约 25%。
 * 现场想试别的颜色：串口 torch_raw <hex>。 */
#define TORCH_LED_COLOR     0xFFFFFF

/* 频闪的半周期（ms）。100ms → 全周期 200ms = 5Hz。
 * 太慢像故障灯，太快每半周期都要过一次 rt_device_control，没必要。 */
#define TORCH_STROBE_MS     100

/* 轻触判定：按下→抬起的位移小于 24px、且时长短于 1.2s，才算一次轻触。 */
#define TORCH_TAP_DX        24
#define TORCH_TAP_MS        1200

/* ================================================================
 *  二、配色（白底上的字，不用纯黑，反差太硬）
 * ================================================================ */
#define C_BG        lv_color_make(0xFF, 0xFF, 0xFF)
#define C_INK       lv_color_make(0x2B, 0x30, 0x3A)   /* 标题 */
#define C_MUTE      lv_color_make(0x8E, 0x96, 0xA3)   /* 提示 */
#define C_LED_ON    lv_color_make(0xE0, 0x9A, 0x00)   /* 琥珀：常亮 */
#define C_LED_FLASH lv_color_make(0x2F, 0x6F, 0xED)   /* 蓝：频闪 */
#define C_LED_OFF   lv_color_make(0xC2, 0xC8, 0xD2)   /* 灰：关 */
/* 按下瞬间的整屏底色：给用户一个"摸到了"的可见反馈，同时也是排查时
 * "事件到没到 root"最直观的判据（见 s_evt_* 上面的注释）。 */
#define C_TAP_FLASH lv_color_make(0xDD, 0xE3, 0xEC)

/* 保持屏幕活跃的周期。必须远小于 watch_demo.c 的 IDLE_TIME_LIMIT(10s)。 */
#define TORCH_KEEP_MS       1000

/* LED 三种模式 */
#define TORCH_MODE_OFF      0
#define TORCH_MODE_ON       1
#define TORCH_MODE_STROBE   2


/* ================================================================
 *  三、硬件层 —— 背光 + WS2812
 *  这一层**不碰 LVGL**，所以串口线程也能安全调。
 * ================================================================ */

/* watch_demo.c 暴露的两个入口（见那里的注释：lcd_device 是它的私有变量，
 * 所以能力留在它自己文件里，这边只调函数）。 */
extern uint8_t app_watch_brightness_get(void);
extern void    app_watch_brightness_set(uint8_t b);

static rt_uint8_t s_on           = 0;      /* app 是否处于"点亮"状态 */
static rt_uint8_t s_led_mode     = TORCH_MODE_OFF;  /* 见 TORCH_MODE_* */
static rt_uint8_t s_saved_bright = 100;    /* 进来之前的背光，退出时还回去 */
static rt_uint8_t s_led_found    = 0;      /* rgbled 设备找到了没（只找一次） */
static int        s_last_ret     = 999;    /* 最近一次驱动返回值（诊断用） */
static rt_uint8_t s_comp         = 0;      /* 传给驱动结构的 is_comp（诊断用） */

/* app 被启动过几次。用来区分两种"点了没反应"：
 *   starts 一直是 0 → 用户压根没进到这个 app（点空了 / 注册有问题）
 *   starts 在涨但屏幕没反应 → 进得来，是 app 内部的问题
 * 单看 s_on 分不出来（它只说明"当前是否点亮"）。 */
static rt_uint32_t s_start_cnt = 0;

/* ⚠ 这两样必须声明在 led_strobe_*() 之前，而那两个函数属于硬件层，
 *   所以定时器句柄放在这里，不放"界面"那一节。 */
static lv_timer_t *g_strobe       = NULL;  /* 频闪定时器 */
static rt_uint8_t  s_strobe_phase = 0;     /* 频闪当前相位 */

/* ⚠ s_rgb 不放进 #if TORCH_HAS_LED 里：torch_info 无条件打印它，
 *   否则关掉 LED 代码时这里会编译不过。 */
static rt_device_t s_rgb = RT_NULL;

#if TORCH_HAS_LED
/* 每次点亮都要重做的那半：外设 LDO + PA32 复用。
 * 幂等 —— 重复设同一套配置没有副作用。 */
static void led_hw_power_on(void)
{
    /* ① 给灯珠供电。少了这句，下面怎么设颜色都不亮。
     *    PM 休眠时这个 LDO 会被关掉，所以必须每次重开。 */
    HAL_PMU_ConfigPeriLdo(PMU_PERI_LDO3_3V3, true, true);

    /* ② PA32 重新映射。板级 bsp_pinmux.c 把它初始化成了 GPIO_A32，
     *    这里改回定时器通道，WS2812 的时序才有地方出来。 */
    HAL_PIN_Set(PAD_PA32, GPTIM2_CH1, PIN_NOPULL, 1);
}

/* 只做一次的那半：找设备。 */
static void led_hw_init(void)
{
    if (s_led_found)
    {
        return;
    }

    led_hw_power_on();

    /* ③ 找设备。名字是 drv_rgbled.c 里写死的 "rgbled"。 */
    s_rgb = rt_device_find("rgbled");
    if (s_rgb)
    {
        s_led_found = 1;
        rt_kprintf("torch: rgbled found (pwm=%s ch=%d)\n",
                   RGB_USING_SK6812MINI_HS_PWM_DEV_NAME, BSP_USING_RGBLED_CH);
    }
    else
    {
        rt_kprintf("torch: rgbled NOT found -> screen-only torch\n");
    }
}
#endif /* TORCH_HAS_LED */

/* 写一次颜色。返回驱动的错误码，并记进 s_last_ret。
 *
 * ⚠ 这个返回值以前被丢掉了，于是"灯不亮"没法区分是代码没调到、
 *   还是驱动当场返回了错误。现在一律记下来。 */
static int led_hw_write(rt_uint32_t color)
{
#if TORCH_HAS_LED
    struct rt_rgbled_configuration cfg;

    if (!s_rgb)
    {
        s_last_ret = -1000;     /* 自定义：设备都没找到 */
        return s_last_ret;
    }

    cfg.is_comp   = s_comp;
    cfg.reserved  = 0;
    cfg.color_rgb = color;

    s_last_ret = rt_device_control(s_rgb, PWM_CMD_SET_COLOR, &cfg);
    return s_last_ret;
#else
    (void)color;
    s_last_ret = -1001;         /* 自定义：编译期就没打开 LED 代码 */
    return s_last_ret;
#endif
}

static void led_strobe_stop(void)
{
    if (g_strobe)
    {
        lv_timer_del(g_strobe);
        g_strobe = NULL;
    }
    s_strobe_phase = 0;
}

/* 频闪心跳：每 TORCH_STROBE_MS 翻一次灯。
 * 走 LVGL 定时器（GUI 线程），所以和触摸事件天然串行，不用加锁。
 * 这里刻意不打日志 —— 5Hz 打日志会把串口和 GUI 线程一起拖死。 */
static void led_strobe_cb(lv_timer_t *t)
{
    (void)t;
    s_strobe_phase = (rt_uint8_t)(!s_strobe_phase);
    led_hw_write(s_strobe_phase ? TORCH_LED_COLOR : 0x000000u);
}

/* 切模式：唯一改灯的地方。停频闪 → 写一次颜色 → （频闪则起定时器）。 */
static void led_set_mode(rt_uint8_t mode)
{
    led_strobe_stop();

    switch (mode)
    {
    case TORCH_MODE_ON:
        s_led_mode = TORCH_MODE_ON;
        led_hw_write(TORCH_LED_COLOR);
        break;

    case TORCH_MODE_STROBE:
        s_led_mode = TORCH_MODE_STROBE;
        led_hw_write(TORCH_LED_COLOR);
        g_strobe = lv_timer_create(led_strobe_cb, TORCH_STROBE_MS, NULL);
        break;

    default:
        s_led_mode = TORCH_MODE_OFF;
        led_hw_write(0x000000u);
        break;
    }

    rt_kprintf("torch: led mode=%d (0=off 1=on 2=strobe) ret=%d found=%d\n",
               (int)s_led_mode, s_last_ret, (int)s_led_found);
}

/* 轻触的语义：常亮 → 频闪 → 关 → 常亮。
 * 进 app 时是"常亮"，所以第一次轻触就是切到频闪。 */
static void torch_next_mode(void)
{
    rt_uint8_t next;

    switch (s_led_mode)
    {
    case TORCH_MODE_ON:     next = TORCH_MODE_STROBE; break;
    case TORCH_MODE_STROBE: next = TORCH_MODE_OFF;    break;
    default:                next = TORCH_MODE_ON;     break;
    }

    rt_kprintf("torch: tap -> mode %d -> %d\n", (int)s_led_mode, (int)next);
    led_set_mode(next);
}

/* 点亮 / 熄灭。两边都幂等 —— on_pause 和 on_stop 会连着来，不能重复减。 */
static void torch_light_on(void)
{
    if (s_on)
    {
        return;
    }

    s_on = 1;
    s_saved_bright = app_watch_brightness_get();

#if TORCH_HAS_LED
    led_hw_init();
    led_hw_power_on();      /* ⚠ 每次进来都重做，见文件头 ⚠ 三 */
#endif

    app_watch_brightness_set(100);
    led_set_mode(TORCH_MODE_ON);

    rt_kprintf("torch: ON  backlight=100 (saved %d) ret=%d found=%d\n",
               (int)s_saved_bright, s_last_ret, (int)s_led_found);
}

static void torch_light_off(void)
{
    if (!s_on)
    {
        return;
    }

    s_on = 0;
    led_set_mode(TORCH_MODE_OFF);
    led_strobe_stop();
    app_watch_brightness_set(s_saved_bright);

    rt_kprintf("torch: OFF backlight restored to %d, led mode=0\n",
               (int)s_saved_bright);
}


/* ================================================================
 *  四、界面
 * ================================================================ */
static lv_obj_t *g_root    = NULL;
static lv_obj_t *g_badge   = NULL;   /* 模式胶囊 */
static lv_obj_t *g_hint    = NULL;
static lv_timer_t *g_keep  = NULL;

/* 触摸事件日志开关。默认开 —— 手电筒最要紧的排查就是"点下去事件到没到"，
 * 而这件事只有真手指按一次才知道。稳定后可以 torch_evt 0 关掉。 */
static rt_uint8_t s_evt_log = 1;

/* 轻触判定用的按下快照 */
static lv_point_t s_press_pt;
static rt_tick_t  s_press_tick = 0;
static rt_uint8_t s_press_down = 0;

/* ── 触摸链路取证 ────────────────────────────────────────────────────
 *
 * 「点了没反应」有三种完全不同的原因，光读代码分不出来，只能让**真实的一次
 * 按压**各自留下痕迹。三个判据互斥，一次按压就能把范围锁死：
 *
 *   ① root_down 涨了            → 事件到了 root，问题在"切换逻辑/灯"那一侧
 *   ② root_down 不涨，但
 *      indev 的 raw 点变了       → 触摸进了 LVGL，但命中了**别的对象**
 *                                  （用 torch_act 看 last_pressed 是谁）
 *   ③ indev 的 raw 点根本不动   → 触摸数据没进 LVGL（驱动/上报层）
 *
 * 另外为了让"事件到没到"这件事**不接串口也能看**，按下时会把整屏背景
 * 闪一下（C_TAP_FLASH）。用户点一下就能肉眼判断，不必读日志。
 */
static rt_uint32_t    s_evt_total = 0;              /* root 收到的全部事件数 */
static rt_uint32_t    s_evt_down  = 0;              /* PRESSED 次数 */
static rt_uint32_t    s_evt_up    = 0;              /* RELEASED 次数 */
static rt_uint32_t    s_evt_lost  = 0;              /* PRESS_LOST 次数 */
static rt_uint32_t    s_evt_click = 0;              /* CLICKED 次数 */
static lv_event_code_t s_last_code = LV_EVENT_ALL;  /* 最近一次事件码 */
static lv_point_t      s_last_pt   = { 0, 0 };      /* 最近一次事件的坐标 */

/* 让屏幕一直"有活动"，从而跳过 10s 自动息屏 —— 见文件头 ⚠ 一。 */
static void keep_awake_cb(lv_timer_t *t)
{
    (void)t;
    lv_disp_trig_activity(NULL);
}

/* 按下时把整屏底色压暗一档，抬起恢复纯白。
 *
 * 一举两得：
 *   对用户 —— 手指摸上去屏幕有反应，不是"点了没反应"的黑箱；
 *   对排查 —— 这是"事件到底到没到 root"唯一**不用接串口**的判据。
 *             点一下屏幕会闪 = 事件到了；完全不闪 = 没到（那就是
 *             indev/命中层的问题，去看 torch_act）。
 */
static void tap_flash(int on)
{
    if (!g_root)
    {
        return;
    }
    lv_obj_set_style_bg_color(g_root,
                              on ? C_TAP_FLASH : C_BG,
                              LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void refresh_badge(void)
{
    const char *txt;
    lv_color_t  col;

    if (!g_badge)
    {
        return;
    }

    switch (s_led_mode)
    {
    case TORCH_MODE_ON:
        txt = "LED 常亮";
        col = C_LED_ON;
        break;

    case TORCH_MODE_STROBE:
        txt = "LED 频闪";
        col = C_LED_FLASH;
        break;

    default:
        txt = "LED 关";
        col = C_LED_OFF;
        break;
    }

    lv_label_set_text(g_badge, txt);
    lv_obj_set_style_text_color(g_badge, col, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(g_badge, col, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static const char *evt_name(lv_event_code_t c)
{
    switch (c)
    {
    case LV_EVENT_PRESSED:      return "PRESSED";
    case LV_EVENT_PRESSING:     return "PRESSING";
    case LV_EVENT_PRESS_LOST:   return "PRESS_LOST";
    case LV_EVENT_RELEASED:     return "RELEASED";
    case LV_EVENT_CLICKED:      return "CLICKED";
    case LV_EVENT_LONG_PRESSED: return "LONG_PRESSED";
    default:                    return "other";
    }
}

/* 只打关心的事件。⚠ PRESSING 是每帧都来的，绝不能无脑全打。 */
static void log_evt(lv_event_code_t code, lv_point_t p)
{
    if (!s_evt_log)
    {
        return;
    }

    switch (code)
    {
    case LV_EVENT_PRESSED:
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
    case LV_EVENT_CLICKED:
    case LV_EVENT_LONG_PRESSED:
        rt_kprintf("torch: evt %s(%d) x=%d y=%d\n",
                   evt_name(code), (int)code, (int)p.x, (int)p.y);
        break;

    default:
        break;
    }
}

static void root_event_cb(lv_event_t *e)
{
    lv_event_code_t code  = lv_event_get_code(e);
    lv_indev_t     *indev = lv_indev_get_act();
    lv_point_t      p     = { 0, 0 };

    if (indev)
    {
        lv_indev_get_point(indev, &p);
    }

    /* 无条件记账：不管后面走哪个分支、日志开关是开是关，这三个数都涨。
     * 排查时只要"点一下看数字变不变"，就能判断事件有没有到这个对象。 */
    s_evt_total++;
    s_last_code = code;
    s_last_pt   = p;
    if      (code == LV_EVENT_PRESSED)    s_evt_down++;
    else if (code == LV_EVENT_RELEASED)   s_evt_up++;
    else if (code == LV_EVENT_PRESS_LOST) s_evt_lost++;
    else if (code == LV_EVENT_CLICKED)    s_evt_click++;

    log_evt(code, p);

    switch (code)
    {
    case LV_EVENT_PRESSED:
        s_press_pt   = p;
        s_press_tick = rt_tick_get();
        s_press_down = 1;
        tap_flash(1);           /* 屏幕上可见的触摸反馈 */
        break;

    case LV_EVENT_RELEASED:
    {
        lv_coord_t  dx, dy;
        rt_uint32_t dt_ms;

        /* ⚠ 先恢复底色再判断 —— 否则命中下面那个 `!s_press_down` 的
         *   early-out 时，屏幕会一直停在按下色上。 */
        tap_flash(0);

        if (!s_press_down)
        {
            break;
        }
        s_press_down = 0;

        dx    = (p.x > s_press_pt.x) ? (p.x - s_press_pt.x) : (s_press_pt.x - p.x);
        dy    = (p.y > s_press_pt.y) ? (p.y - s_press_pt.y) : (s_press_pt.y - p.y);
        dt_ms = (rt_uint32_t)(rt_tick_get() - s_press_tick) * 1000u / RT_TICK_PER_SECOND;

        if (dx < TORCH_TAP_DX && dy < TORCH_TAP_DX && dt_ms < TORCH_TAP_MS)
        {
            rt_kprintf("torch: release dx=%d dy=%d dt=%ums -> TAP\n",
                       (int)dx, (int)dy, (unsigned)dt_ms);
            torch_next_mode();
            refresh_badge();
        }
        else
        {
            rt_kprintf("torch: release dx=%d dy=%d dt=%ums -> drag, ignore\n",
                       (int)dx, (int)dy, (unsigned)dt_ms);
        }
        break;
    }

    case LV_EVENT_PRESS_LOST:
        s_press_down = 0;
        tap_flash(0);
        break;

    default:
        break;
    }
}

static void mk_label(lv_obj_t *par, const char *txt, int font, lv_color_t col,
                     lv_align_t align, lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *l = lv_label_create(par);

    lv_label_set_text(l, txt);
    lv_ext_set_local_font(l, font, col);
    lv_obj_align(l, align, x, y);
    /* 标签默认就不可点击，这里显式写一遍：它们是压在 root 上的，
     * 万一哪天 SDK 主题改了默认值，点击就会被这些字吃掉。 */
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
}

static void torch_create(void)
{
    lv_obj_t *scr = lv_scr_act();

    /* 屏幕底色也刷白，避免转场那一帧露出别的颜色 */
    lv_obj_set_style_bg_color(scr, C_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    g_root = lv_obj_create(scr);
    lv_obj_set_size(g_root, LV_HOR_RES_MAX, LV_VER_RES_MAX);
    lv_obj_set_pos(g_root, 0, 0);
    lv_obj_set_style_radius(g_root, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(g_root, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(g_root, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(g_root, C_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    /* ⚠ 必须清 SCROLLABLE：否则 LVGL 会把它选成 scroll_obj，
     *   滚动处理会插在 PRESSED/PRESSING 之间抢走事件（和 swipe_back.c 同一个坑）。 */
    lv_obj_clear_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_root, root_event_cb, LV_EVENT_ALL, NULL);

    /* 标题压在上边但避开顶部 34px（上划手势条的位置，和计算器一致） */
    mk_label(g_root, "手电筒", FONT_TITLE, C_INK, LV_ALIGN_TOP_MID, 0, 46);
    mk_label(g_root, "常亮 · 屏幕不会自动熄灭", FONT_SMALL, C_MUTE,
             LV_ALIGN_TOP_MID, 0, 84);

    /* 模式胶囊（靠下，别挡着中间那块最亮的白） */
    g_badge = lv_label_create(g_root);
    lv_label_set_text(g_badge, "LED 关");
    lv_ext_set_local_font(g_badge, FONT_NORMAL, C_LED_OFF);
    lv_obj_clear_flag(g_badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_hor(g_badge, 18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_ver(g_badge, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(g_badge, 999, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(g_badge, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(g_badge, C_LED_OFF, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(g_badge, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(g_badge, LV_ALIGN_BOTTOM_MID, 0, -66);

    g_hint = lv_label_create(g_root);
    lv_label_set_text(g_hint, "轻触屏幕 = 常亮 / 频闪 / 关\n右边缘往左滑 = 退出");
    lv_ext_set_local_font(g_hint, FONT_SMALL, C_MUTE);
    lv_obj_set_style_text_align(g_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(g_hint, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(g_hint, LV_ALIGN_BOTTOM_MID, 0, -10);

    /* 几何自检：屏幕到手电筒这种"整屏铺满"的界面，最容易出的是
     * "其实没铺满，边上露了一条底色"，所以把 root 的实际几何打出来。
     *
     * ⚠ 两个坑都在这一行里：
     *   1) 读 lv_obj_get_width() 而不是 LV_HOR_RES_MAX —— 打印期望值等于没验；
     *   2) 先 lv_obj_update_layout() —— 刚 create 完的对象 coords 还是空的，
     *      不加这句打出来是 x=0..-1（宽 0），会让人以为界面尺寸错了。 */
    lv_obj_update_layout(g_root);
    rt_kprintf("torch: root geom x=%d..%d y=%d..%d (%dx%d) screen=%dx%d clickable=%d\n",
               g_root->coords.x1, g_root->coords.x2,
               g_root->coords.y1, g_root->coords.y2,
               lv_obj_get_width(g_root), lv_obj_get_height(g_root),
               LV_HOR_RES_MAX, LV_VER_RES_MAX,
               (int)lv_obj_has_flag(g_root, LV_OBJ_FLAG_CLICKABLE));
    rt_kprintf("torch: badge bottom=-66 hint bottom=-10 (both above the 16px swipe bar)\n");

    if (!g_keep)
    {
        g_keep = lv_timer_create(keep_awake_cb, TORCH_KEEP_MS, NULL);
        rt_kprintf("torch: keep-awake timer %dms (idle screen-off is 10s)\n",
                   TORCH_KEEP_MS);
    }
}


/* ================================================================
 *  五、app 生命周期
 * ================================================================ */
static void on_start(void)
{
    s_start_cnt++;
    rt_kprintf("torch: start (#%u)\n", (unsigned)s_start_cnt);
    torch_create();
    torch_light_on();
    refresh_badge();
}

static void on_resume(void)
{
    rt_kprintf("torch: resume\n");

    /* ⚠ on_pause 会把灯关掉、背光还回去。而框架里存在 pause -> resume
     *   这种"不经过 start"的恢复路径（比如被别的 app 抢过前台又回来）。
     *   真走那条路的话，用户会看到"回到手电筒，但屏幕变暗、灯也不亮"。
     *   这里补一次"确保点亮"—— torch_light_on() 内部有 s_on 守卫，
     *   重复调完全无害（进 app 时 start 和 resume 是连着来的，第二次是空操作）。 */
    torch_light_on();
    refresh_badge();
}

/* pause 和 stop 会连着来（返回时框架先把 app 挂起再销毁），
 * torch_light_off() 内部有 s_on 守卫，重复调不会把背光减两次。 */
static void on_pause(void)
{
    rt_kprintf("torch: pause\n");
    torch_light_off();
}

static void on_stop(void)
{
    rt_kprintf("torch: stop\n");
    torch_light_off();

    led_strobe_stop();

    if (g_keep)
    {
        lv_timer_del(g_keep);       /* 定时器不删会在 app 销毁后继续跑 */
        g_keep = NULL;
    }

    g_root  = NULL;
    g_badge = NULL;
    g_hint  = NULL;
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

LV_IMG_DECLARE(img_torch);

#define APP_ID "torch"

static int app_main(intent_t i)
{
    gui_app_regist_msg_handler(APP_ID, msg_handler);

    /* 设备在这里找一次就够了（drv_rgbled 是 INIT_COMPONENT_EXPORT 常驻注册的），
     * 这样即使还没进过 app，串口命令也能直接把灯点亮来排查。 */
#if TORCH_HAS_LED
    led_hw_init();
#endif

    return 0;
}

/* 图标不是 NULL 才会出现在主菜单的蜂窝里，这条务必别漏 */
BUILTIN_APP_EXPORT(LV_EXT_STR_ID(torch), LV_EXT_IMG_GET(img_torch),
                   APP_ID, app_main);


/* ================================================================
 *  六、串口自检
 *
 *  「灯亮没亮」这件事屏幕截不到、串口看不见，只能靠人眼 —— 但这不等于
 *  整条链路都只能靠眼睛。可以自动验证的是：
 *    · 背光有没有真的被设成 100、退出有没有还回去（数值可读）
 *    · rgbled 设备找没找到、**写颜色时驱动返回了什么**（返回值可读）
 *    · 某个坐标点下去会不会命中 root（命中测试可查）
 *    · 出没出现在蜂窝里（遍历 builtin app 列表）
 *  剩下唯一要人看的只有"物理上那颗灯亮不亮"。
 *
 *  ⚠ 会碰 LVGL 的动作走 lv_async_call 派回 GUI 线程 —— 直接在 shell
 *    线程里改控件会撞断言（和 swipe_back.c 的 swipe_sim 同一个理由）。
 *    只读的诊断（torch_info / torch_hit）直接调，和 swipe_info 一致。
 * ================================================================ */
#ifdef RT_USING_FINSH

#include <finsh.h>

/* 统一异步动作码 */
#define TC_NONE     0
#define TC_OFF      1
#define TC_ON       2
#define TC_TAP      3
#define TC_STROBE   4
#define TC_RAW      5
#define TC_COMP     6

static int         s_cmd_action  = TC_NONE;
static rt_uint32_t s_raw_color   = 0;

static void torch_cmd_async(void *arg)
{
    (void)arg;

    switch (s_cmd_action)
    {
    case TC_OFF:
        torch_light_off();
        break;

    case TC_ON:
        /* ⚠ 不能无脑 torch_light_on()：app 已经点亮时它会被开头的 s_on 守卫
         *   直接 return，于是 torch_on / torch_strobe 0 变成空操作
         *   （实测就是：串口说"回到常亮"，torch_info 里 mode 还是 2）。
         *   已经在点亮态，就只把 LED 切回常亮。 */
        if (s_on)
        {
            led_set_mode(TORCH_MODE_ON);
        }
        else
        {
            torch_light_on();
        }
        break;

    case TC_TAP:
        /* 走的就是真手指轻触的那条路 —— 用来区分
         * "轻触逻辑坏了" 和 "触摸事件根本没到"。 */
        torch_next_mode();
        break;

    case TC_STROBE:
        led_set_mode(TORCH_MODE_STROBE);
        break;

    case TC_RAW:
        /* 手动试颜色：退出模式循环，免得被频闪定时器立刻覆盖掉 */
        led_strobe_stop();
        s_led_mode = TORCH_MODE_OFF;
        rt_kprintf("torch: raw 0x%06X -> ret=%d\n",
                   (unsigned)s_raw_color, led_hw_write(s_raw_color));
        break;

    case TC_COMP:
        rt_kprintf("torch: comp=%d -> ret=%d\n", (int)s_comp, led_hw_write(TORCH_LED_COLOR));
        break;

    default:
        break;
    }

    refresh_badge();
    s_cmd_action = TC_NONE;
}

static void torch_post(int action)
{
    s_cmd_action = action;
    lv_async_call(torch_cmd_async, NULL);
}

/* ── 状态总览 ──────────────────────────────────────────────────── */
static int torch_info(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    rt_kprintf("torch: app on=%d starts=%u mode=%d saved_bright=%d backlight_now=%d\n",
               (int)s_on, (unsigned)s_start_cnt, (int)s_led_mode,
               (int)s_saved_bright, (int)app_watch_brightness_get());
    rt_kprintf("torch: keep_timer=%p strobe_timer=%p phase=%d evt_log=%d\n",
               g_keep, g_strobe, (int)s_strobe_phase, (int)s_evt_log);
    /* 触摸链路：点一下屏幕，这行数字必须涨。不涨就是事件没到 root。 */
    rt_kprintf("torch: evt_total=%u down=%u up=%u lost=%u click=%u last=%d@(%d,%d)\n",
               (unsigned)s_evt_total, (unsigned)s_evt_down, (unsigned)s_evt_up,
               (unsigned)s_evt_lost, (unsigned)s_evt_click,
               (int)s_last_code, (int)s_last_pt.x, (int)s_last_pt.y);
    rt_kprintf("torch: led_found=%d dev=%p last_ret=%d color=0x%06X comp=%d has_led_code=%d\n",
               (int)s_led_found, s_rgb, s_last_ret,
               (unsigned)TORCH_LED_COLOR, (int)s_comp, TORCH_HAS_LED);
#if TORCH_HAS_LED
    rt_kprintf("torch: rgbled=%s dev=\"rgbled\" pwm=\"%s\" ch=%d\n",
               s_led_found ? "OK" : "MISSING",
               RGB_USING_SK6812MINI_HS_PWM_DEV_NAME, BSP_USING_RGBLED_CH);
#endif
    if (g_root)
    {
        rt_kprintf("torch: root x=%d..%d y=%d..%d clickable=%d scrollable=%d\n",
                   g_root->coords.x1, g_root->coords.x2,
                   g_root->coords.y1, g_root->coords.y2,
                   (int)lv_obj_has_flag(g_root, LV_OBJ_FLAG_CLICKABLE),
                   (int)lv_obj_has_flag(g_root, LV_OBJ_FLAG_SCROLLABLE));
    }
    else
    {
        rt_kprintf("torch: root NOT created (app not started)\n");
    }
    return 0;
}
/* 注意：MSH_CMD_EXPORT 的描述里不能出现逗号（会被预处理器当成参数分隔符） */
MSH_CMD_EXPORT(torch_info, torch state: backlight/led mode/driver retval);

/* ── 命中测试：问"这个坐标点下去会打给谁" ──────────────────────
 *
 * 复刻 LVGL indev 的搜索顺序：top 层 → sys 层 → 当前屏幕，
 * 每层里先子后父、从最后一个子对象往前（即最上面的优先）。
 * 命中的对象必须同时满足"坐标在内"和"CLICKABLE"。
 *
 * 典型对照：
 *   torch_hit 195 300   → 期望 ROOT(good)
 *   torch_hit 386 300   → 期望 OTHER（右边缘 16px 是滑动返回条的地盘）
 */
static lv_obj_t *torch_hit_search(lv_obj_t *obj, const lv_point_t *p)
{
    int32_t cnt = 0;
    int32_t i;

    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN))
    {
        return NULL;
    }

    /* 自己数子对象个数：只依赖 lv_obj_get_child()，避开不同 LVGL 小版本
     * 里 lv_obj_get_child_cnt 的命名差异。 */
    while (lv_obj_get_child(obj, cnt) != NULL)
    {
        cnt++;
    }

    for (i = cnt - 1; i >= 0; i--)
    {
        lv_obj_t *r = torch_hit_search(lv_obj_get_child(obj, i), p);

        if (r)
        {
            return r;
        }
    }

    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE) && lv_obj_hit_test(obj, p))
    {
        return obj;
    }

    return NULL;
}

static int torch_hit(int argc, char **argv)
{
    lv_point_t p;
    lv_obj_t *hit;

    if (argc < 3)
    {
        rt_kprintf("usage: torch_hit <x> <y>\n");
        return -1;
    }

    p.x = (lv_coord_t)atoi(argv[1]);
    p.y = (lv_coord_t)atoi(argv[2]);

    hit = torch_hit_search(lv_layer_top(), &p);
    if (!hit) hit = torch_hit_search(lv_layer_sys(), &p);
    if (!hit) hit = torch_hit_search(lv_scr_act(), &p);

    rt_kprintf("torch: hit(%d,%d) -> %s   [hit=%p root=%p badge=%p]\n",
               (int)p.x, (int)p.y,
               hit ? ((hit == g_root) ? "ROOT(good)" : "OTHER") : "NOTHING",
               hit, g_root, g_badge);

    if (g_root)
    {
        rt_kprintf("torch: root x=%d..%d y=%d..%d (%dx%d) clickable=%d hidden=%d\n",
                   g_root->coords.x1, g_root->coords.x2,
                   g_root->coords.y1, g_root->coords.y2,
                   lv_obj_get_width(g_root), lv_obj_get_height(g_root),
                   (int)lv_obj_has_flag(g_root, LV_OBJ_FLAG_CLICKABLE),
                   (int)lv_obj_has_flag(g_root, LV_OBJ_FLAG_HIDDEN));
    }
    return 0;
}
MSH_CMD_EXPORT(torch_hit, torch_hit <x> <y> : hit-test a point against the torch screen);

/* ── 触摸链路取证 ────────────────────────────────────────────────
 *
 * 「点了没反应」有三种完全不同的原因，读代码分不出来，但一次真实按压
 * 就能把它们分开。torch_act 读的是 LVGL **内部**的真实状态
 * （indev->proc），不是我复刻的近似值：
 *
 *   raw  = 驱动报上来的原始点（还没做屏幕旋转 / 偏移换算）
 *   act  = 换算之后、真正拿去命中测试的点
 *   last_pressed = 最近一次"按下"所命中的对象
 *
 * 判读表（点一下屏幕，然后敲 torch_act）：
 *   raw 变了 + last_pressed 不是 root  → 触摸是好的，被别的层挡住了
 *   raw 变了 + last_pressed 就是 root  → 该到 root 却没到，看 evt_down
 *   raw 一直不动                       → 触摸数据没进 LVGL（驱动/上报层）
 *
 * 顺带：这里用 LVGL 自己的 lv_indev_search_obj() 做命中测试，
 * 不再用我复刻的那套 —— 免得"我的复刻和真实现不一致"把结论带偏。
 */
static const char *obj_tag(lv_obj_t *o)
{
    if (!o)                  return "NULL";
    if (o == g_root)         return "g_root";
    if (o == lv_scr_act())   return "scr";
    if (o == lv_layer_top()) return "layer_top";
    if (o == lv_layer_sys()) return "layer_sys";
    if (o == g_badge)        return "g_badge";
    if (o == g_hint)         return "g_hint";
    return "other";
}

static int torch_act(int argc, char **argv)
{
    lv_indev_t *indev = lv_indev_get_next(NULL);
    int         n     = 0;
    lv_point_t  p     = { 195, 300 };   /* 屏幕中下部：手电筒界面里最"干净"的点 */
    lv_obj_t   *hit   = NULL;

    (void)argc;
    (void)argv;

    rt_kprintf("torch: ==== indev (LVGL internal) ====\n");

    while (indev)
    {
        lv_disp_t *disp = (indev->driver) ? indev->driver->disp : NULL;

        rt_kprintf("torch: indev[%d] @%p type=%d state=%d disabled=%d wait_rel=%d\n",
                   n, indev,
                   indev->driver ? (int)indev->driver->type : -1,
                   (int)indev->proc.state,
                   (int)indev->proc.disabled,
                   (int)indev->proc.wait_until_release);

        rt_kprintf("torch:   raw=(%d,%d) act=(%d,%d) last=(%d,%d)\n",
                   (int)indev->proc.types.pointer.last_raw_point.x,
                   (int)indev->proc.types.pointer.last_raw_point.y,
                   (int)indev->proc.types.pointer.act_point.x,
                   (int)indev->proc.types.pointer.act_point.y,
                   (int)indev->proc.types.pointer.last_point.x,
                   (int)indev->proc.types.pointer.last_point.y);

        rt_kprintf("torch:   rot=%d off=(%d,%d) hor=%d ver=%d\n",
                   (disp && disp->driver) ? (int)disp->driver->rotated : -1,
                   (disp && disp->driver) ? (int)disp->driver->offset_x : -1,
                   (disp && disp->driver) ? (int)disp->driver->offset_y : -1,
                   disp ? (int)lv_disp_get_hor_res(disp) : -1,
                   disp ? (int)lv_disp_get_ver_res(disp) : -1);

        rt_kprintf("torch:   act_obj=%s(%p) last_obj=%s last_pressed=%s(%p)\n",
                   obj_tag(indev->proc.types.pointer.act_obj),
                   indev->proc.types.pointer.act_obj,
                   obj_tag(indev->proc.types.pointer.last_obj),
                   obj_tag(indev->proc.types.pointer.last_pressed),
                   indev->proc.types.pointer.last_pressed);

        indev = lv_indev_get_next(indev);
        n++;
    }

    if (n == 0)
    {
        rt_kprintf("torch: !! no indev registered at all\n");
    }

    hit = lv_indev_search_obj(lv_layer_top(), &p);
    rt_kprintf("torch: real-hit(%d,%d) top->%s(%p)\n", (int)p.x, (int)p.y, obj_tag(hit), hit);
    if (!hit)
    {
        hit = lv_indev_search_obj(lv_layer_sys(), &p);
        if (hit)
        {
            rt_kprintf("torch: real-hit sys->%s(%p)\n", obj_tag(hit), hit);
        }
    }
    if (!hit)
    {
        hit = lv_indev_search_obj(lv_scr_act(), &p);
        rt_kprintf("torch: real-hit scr->%s(%p)\n", obj_tag(hit), hit);
    }
    rt_kprintf("torch: real-hit FINAL=%s(%p) is_root=%d\n",
               obj_tag(hit), hit, (hit == g_root));

    rt_kprintf("torch: ==== root-side counters ====\n");
    rt_kprintf("torch: evt_total=%u down=%u up=%u lost=%u click=%u last=%d@(%d,%d)\n",
               (unsigned)s_evt_total, (unsigned)s_evt_down, (unsigned)s_evt_up,
               (unsigned)s_evt_lost, (unsigned)s_evt_click,
               (int)s_last_code, (int)s_last_pt.x, (int)s_last_pt.y);
    return 0;
}
MSH_CMD_EXPORT(torch_act, torch_act : dump LVGL indev state and a real hit-test);

/* ── 对象树：看"屏幕上到底压着几层东西" ────────────────────────
 * 手电筒的 root 建在 lv_scr_act() 上，但命中测试是 top → sys → scr
 * 一路往下找的。只要上面两层里有任何一层是可点的，root 就永远轮不到。
 */
static void tree_dump(lv_obj_t *o, int depth)
{
    char     ind[24];
    int      i, n;
    uint32_t cnt;

    if (!o || depth > 6)
    {
        return;
    }

    n = depth * 2;
    if (n > 20)
    {
        n = 20;
    }
    for (i = 0; i < n; i++)
    {
        ind[i] = ' ';
    }
    ind[n] = 0;

    rt_kprintf("torch: %s%s(%p) %dx%d at(%d,%d) clk=%d hid=%d opa=%d\n",
               ind, obj_tag(o), o,
               (int)lv_obj_get_width(o), (int)lv_obj_get_height(o),
               (int)o->coords.x1, (int)o->coords.y1,
               (int)lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE),
               (int)lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN),
               (int)lv_obj_get_style_bg_opa(o, LV_PART_MAIN));

    cnt = lv_obj_get_child_cnt(o);
    for (i = 0; i < (int)cnt; i++)
    {
        tree_dump(lv_obj_get_child(o, i), depth + 1);
    }
}

static int torch_tree(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    rt_kprintf("torch: ==== layer_top ====\n");
    tree_dump(lv_layer_top(), 0);
    rt_kprintf("torch: ==== layer_sys ====\n");
    tree_dump(lv_layer_sys(), 0);
    rt_kprintf("torch: ==== scr ====\n");
    tree_dump(lv_scr_act(), 0);
    rt_kprintf("torch: root=%p badge=%p hint=%p\n", g_root, g_badge, g_hint);
    return 0;
}
MSH_CMD_EXPORT(torch_tree, torch_tree : dump the obj tree above the torch screen);

/* ── 直接写颜色（现场试灯用）───────────────────────────────────
 *   torch_raw FF0000   → 纯红：满白下最容易看出灯到底亮不亮
 *   torch_raw 0F0F0F   → SDK 例程那个"白"，只有 6% 亮度
 *   torch_raw 000000   → 灭
 */
static int torch_raw(int argc, char **argv)
{
    if (argc < 2)
    {
        rt_kprintf("usage: torch_raw <hex color>   e.g. torch_raw FF0000\n");
        return -1;
    }

    s_raw_color = (rt_uint32_t)strtoul(argv[1], NULL, 16);
    rt_kprintf("torch: raw queued 0x%06X\n", (unsigned)s_raw_color);
    torch_post(TC_RAW);
    return 0;
}
MSH_CMD_EXPORT(torch_raw, torch_raw <hex> : write a raw color to the WS2812);

/* 切换驱动结构体里的 is_comp 字段（有些板子要开着才是满亮度），
 * 顺手重写一次当前颜色，方便直接对比。 */
static int torch_comp(int argc, char **argv)
{
    if (argc >= 2)
    {
        s_comp = (rt_uint8_t)(atoi(argv[1]) != 0);
    }
    rt_kprintf("torch: comp queued %d\n", (int)s_comp);
    torch_post(TC_COMP);
    return 0;
}
MSH_CMD_EXPORT(torch_comp, torch_comp <0|1> : set the rgbled is_comp field);

/* ── 模式 / 动作 ────────────────────────────────────────────── */
static int torch_on(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    rt_kprintf("torch: cmd on -> async\n");
    torch_post(TC_ON);
    return 0;
}
MSH_CMD_EXPORT(torch_on, turn the torch on (backlight 100 + white screen + led));

static int torch_off(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    rt_kprintf("torch: cmd off -> async\n");
    torch_post(TC_OFF);
    return 0;
}
MSH_CMD_EXPORT(torch_off, turn the torch off and restore the previous backlight);

static int torch_strobe(int argc, char **argv)
{
    if (argc >= 2 && atoi(argv[1]) == 0)
    {
        rt_kprintf("torch: cmd strobe 0 -> back to steady on\n");
        torch_post(TC_ON);
    }
    else
    {
        rt_kprintf("torch: cmd strobe -> async\n");
        torch_post(TC_STROBE);
    }
    return 0;
}
MSH_CMD_EXPORT(torch_strobe, torch_strobe <0|1> : start/stop the 5Hz strobe);

/* 模拟一次轻触：走的是和真手指同一条逻辑，
 * 用来把"逻辑问题"和"触摸没送达"分开。 */
static int torch_tap(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    rt_kprintf("torch: cmd tap -> async (same path as a real tap)\n");
    torch_post(TC_TAP);
    return 0;
}
MSH_CMD_EXPORT(torch_tap, torch_tap : simulate one tap on the torch screen);

static int torch_led(int argc, char **argv)
{
    if (argc > 1)
    {
        rt_kprintf("torch: cmd led %s\n", argv[1]);
        torch_post((argv[1][0] == '1') ? TC_ON : TC_OFF);
    }
    else
    {
        rt_kprintf("torch: cmd led (toggle)\n");
        torch_post(TC_TAP);     /* 不带参数 = 按轻触的循环走一格 */
    }
    return 0;
}
MSH_CMD_EXPORT(torch_led, torch_led <0|1> : force the WS2812 stead on/off);

/* 触摸事件日志开关（默认开） */
static int torch_evt(int argc, char **argv)
{
    if (argc >= 2)
    {
        s_evt_log = (rt_uint8_t)(atoi(argv[1]) != 0);
    }
    rt_kprintf("torch: evt_log=%d\n", (int)s_evt_log);
    return 0;
}
MSH_CMD_EXPORT(torch_evt, torch_evt <0|1> : toggle touch event logging);

/* 从串口切到手电筒 app —— 和 watch_demo.c 的 to_clock 一个路子
 * （gui_app_run() 只是往 GUI 邮箱发消息，在 shell 线程里调是安全的）。
 *
 * 为什么要专门留这条：torch_on / torch_off 只测到「硬件那一层」，
 * 而「进 app 后界面建没建、防息屏定时器起没起、退出会不会把背光还回去」
 * 这三件事，只有 app 真的被启动才走得到。这是手电筒最容易坏的地方
 * —— 一个 10 秒后自己黑的手电筒，光看代码是看不出来的。 */
static int torch_ui(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    rt_kprintf("torch: gui_app_run(\"%s\") -> start app\n", APP_ID);
    gui_app_run(APP_ID);
    return 0;
}
MSH_CMD_EXPORT(torch_ui, switch to the torch app (same as tapping its icon));

#endif /* RT_USING_FINSH */
