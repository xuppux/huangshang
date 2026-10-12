/*********************
 *      INCLUDES
 *********************/
#include "littlevgl2rtt.h"
#include "lv_ext_resource_manager.h"
#include <rtdevice.h>
#include <time.h>          /* time() / localtime()：启动时判断 RTC 里有没有真实时间 */
#ifndef _WIN32
    #include "drv_lcd.h"
#endif
#include "gui_app_fwk.h"
#include "lv_ex_data.h"
#include "app_mem.h"
#include "log.h"
#include "lv_freetype.h"
#include "ble_app.h"         /* 蓝牙校时：对外只有那几个函数，细节全在 src/app_utils/ble_app.c */
#include "swipe_back.h"      /* 右边缘「从右往左滑」返回：细节在 src/app_utils/swipe_back.c */

#ifdef BSP_USING_PM
    #include "bf0_pm.h"
    #include "gui_app_pm.h"
    #include "drv_gpio.h"
    #include "bf0_hal_aon.h"    /* AON_PIN_MODE_* / HAL_HPAON_QueryWakeupPin */
    #include "bf0_hal_pmu.h"    /* HAL_PMU_SelectWakeupPin / EnablePinWakeup / EnableRtcWakeup */
#endif /* BSP_USING_PM */

#define APP_WATCH_GUI_TASK_STACK_SIZE 16*1024

/* KEY1 一键三用：平时当 Home/翻页键、息屏后当唤醒键、长按当关机键 */
#define SLEEP_CTRL_PIN   (BSP_KEY1_PIN)
#define LCD_DEVICE_NAME  "lcd"

/* 无操作多久自动息屏（毫秒）。10 秒。
 *
 * 这个判断写在 button_event_task_entry() 里（30ms 一次的 lv_timer），
 * 而那个函数连同整个 PM 状态机都在 BSP_USING_PM 开关后面 ——
 * 开关不开，息屏逻辑等于不存在。开关在 project/proj.conf 里。 */
#define IDLE_TIME_LIMIT  (10000)

/* ══════════════════════════════════════════════════════════════════════════
 *  开机时钟自检 —— 临时诊断，定位「表盘时间冻住」
 * ══════════════════════════════════════════════════════════════════════════
 *  为什么必须让固件自己开口说话：
 *  这块板子没有 RTC 备份电池，拔电即丢时，冷启动 RTC 读到 0。要回答
 *  「时间到底走不走」，光靠串口问是问不出来的 —— 板子 10 秒无操作就息屏，
 *  uart1 跟着断电，控制台彻底哑掉，命令发进去连一个字符的回显都没有。
 *  所以开机后前 60 秒，每 2 秒主动打一行，同时报出三个时钟，一次复位就能
 *  看清到底是谁不动：
 *
 *      rtc  : time() + localtime()          —— 表盘真正用的就是这条路径
 *      tick : lv_tick_get()                 —— LVGL 所有 lv_timer 的时基，
 *                                              它不动 => band_tick 永远不触发，
 *                                              表盘就永久停在第一帧
 *      idle : lv_disp_get_inactive_time()   —— 息屏判断用的那个值
 *
 *  判定方法：连着看两行，rtc 的秒数每 2 秒应 +2，tick 每 2 秒应 +2000 左右。
 *  哪个不动，问题就在哪一层。
 *
 *  正常使用时设 0。设 1 时还会顺手把 pm_keepawake 的初值改成 1
 *  （见下面 pm_keepawake 的定义），否则板子 10 秒就睡，自检只打得出 5 行。
 *
 *  ⚠ 现在设 0。自检在 2026-09-20 已经跑完，结论是：
 *      RTC 走时正常（每 2 秒精确 +2 秒，跑满 60 秒无跳变）
 *      lv_tick 正常（每 2 秒 +2000）
 *      RTC 软复位后保持，只有**拔电**才归零 —— 硬件无备份电池
 *    留成 1 会让板子永远不睡，白耗电，所以回到 0。 */
#define WATCH_DIAG_BOOTCLOCK    0
#define DIAG_PERIOD_MS          (2000)
#define DIAG_SAMPLES            (30)      /* 30 × 2s = 60s */

typedef enum
{
    BTN_EVT_SHUTDOWN = 0x01,
    BTN_EVT_UI_CLOSE = 0x02,
    BTN_EVT_UI_OPEN  = 0x04,
    BTN_EVT_ALL = BTN_EVT_SHUTDOWN | BTN_EVT_UI_CLOSE | BTN_EVT_UI_OPEN,
} btn_evt_type_t;


static struct rt_thread watch_thread;

ALIGN(RT_ALIGN_SIZE)
static uint8_t watch_thread_stack[APP_WATCH_GUI_TASK_STACK_SIZE];
static rt_device_t lcd_device;

static lv_timer_t *button_event_task;
static struct rt_event btn_event;
static lv_obj_t *mbox;

#ifdef BSP_USING_PM
/* 置 1 就不做无操作息屏了。调试开关，正常使用时保持 0。
 * 见 button_event_task_entry() 里的注释和文件末尾的 keepawake 命令。
 *
 * 时钟自检期间默认置 1：板子一睡 uart1 就断电，自检日志会断在半路。 */
#if WATCH_DIAG_BOOTCLOCK
static rt_bool_t pm_keepawake = RT_TRUE;
#else
static rt_bool_t pm_keepawake = RT_FALSE;
#endif
#endif

/*Compatible with private lib*/
uint32_t g_mainmenu[2];

extern void ui_datac_init(void);

/**
 * return to MAIN_APP or CLOCK_APP
 * \n
 *
 * @param event
 * @return
 * @param key
 * \n
 * @see
 */
static int32_t default_keypad_handler(lv_key_t key, lv_indev_state_t event)
{
    static lv_indev_state_t last_event = LV_INDEV_STATE_REL;

    if (last_event != event) //Not execute repeatly.
    {
        last_event = event;

        if ((LV_INDEV_STATE_PR == event) && (LV_KEY_HOME == key))
        {
            LOG_I("default_keypad_handler %d,%d", key, event);
            if (gui_app_is_actived("Main"))
                gui_app_run("clock");
            else
            {
                gui_app_run("Main");
#if defined(GUI_APP_FRAMEWORK)&&(!defined (APP_TRANS_ANIMATION_NONE))
                lvsf_gesture_bars_realign();
#endif
            }
        }
        else if ((LV_INDEV_STATE_PR == event) && (LV_KEY_ESC == key))
        {
            gui_app_goback();
        }
    }

    return LV_BLOCK_EVENT;
}


#ifdef USING_BUTTON_LIB

#include "button.h"

#ifdef BSP_KEY1_ACTIVE_HIGH
    #define BUTTON_ACTIVE_POL BUTTON_ACTIVE_HIGH
#else
    #define BUTTON_ACTIVE_POL BUTTON_ACTIVE_LOW
#endif


typedef enum
{
    KEYPAD_KEY_HOME = 2,
} keypad_key_code_t;

typedef enum
{
    KEYPAD_KEY_STATE_REL,
    KEYPAD_KEY_STATE_PRESSED,
} keypad_key_state_t;

typedef struct
{
    keypad_key_code_t last_key;
    keypad_key_state_t last_key_state;
} keypad_status_t;

static int32_t key1_button_handle = -1;
static keypad_status_t keypad_status;

void button_key_read(uint32_t *last_key, lv_indev_state_t *state)
{
    if (last_key && state)
    {
        *last_key = keypad_status.last_key;
        if (KEYPAD_KEY_STATE_REL == keypad_status.last_key_state)
        {
            *state = LV_INDEV_STATE_REL;
        }
        else
        {
            *state = LV_INDEV_STATE_PR;
            keypad_status.last_key_state = KEYPAD_KEY_STATE_REL;
        }
    }
}

/* button event handler in UI inactive state */
static void button_event_handler(int32_t pin, button_action_t action)
{
#ifdef BSP_USING_PM
    gui_pm_action_t pm_action;

    LOG_I("button:%d,%d", pin, action);

    if ((SLEEP_CTRL_PIN == pin) && (!gui_is_active() || (action == BUTTON_LONG_PRESSED)))
    {
        pm_action = GUI_PM_ACTION_INVALID;
        switch (action)
        {
        case BUTTON_PRESSED:
        {
            pm_action = GUI_PM_ACTION_BUTTON_PRESSED;
            break;
        }
        case BUTTON_RELEASED:
        {
            pm_action = GUI_PM_ACTION_BUTTON_RELEASED;
            break;
        }
        case BUTTON_CLICKED:
        {
            pm_action = GUI_PM_ACTION_BUTTON_CLICKED;
            break;
        }
        case BUTTON_LONG_PRESSED:
        {
            pm_action = GUI_PM_ACTION_BUTTON_LONG_PRESSED;
            break;
        }
        default:
        {
            pm_action = GUI_PM_ACTION_INVALID;
        }
        }
        if (GUI_PM_ACTION_INVALID != pm_action)
        {
            gui_pm_fsm(pm_action);
        }
    }
    else
#endif  /* BSP_USING_PM */
    {
        lv_disp_trig_activity(NULL);
        switch (action)
        {
        case BUTTON_CLICKED:
        {
            keypad_status.last_key = KEYPAD_KEY_HOME;
            keypad_status.last_key_state = KEYPAD_KEY_STATE_PRESSED;
            break;
        }
        default:
            break;
        }
    }
}

static void init_pin(void)
{
    button_cfg_t cfg;

    cfg.pin = SLEEP_CTRL_PIN;
    cfg.active_state = BUTTON_ACTIVE_POL;
    cfg.mode = PIN_MODE_INPUT;
    cfg.button_handler = button_event_handler;
    int32_t id = button_init(&cfg);
    RT_ASSERT(id >= 0);
    RT_ASSERT(SF_EOK == button_enable(id));
    key1_button_handle = id;
}

#else
#define init_pin()
#endif /* USING_BUTTON_LIB */

#ifdef BSP_USING_PM
static void opa_anim(void *bg, int32_t v)
{
    lv_obj_set_style_bg_opa(bg, (lv_opa_t)v, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void mbox_event_cb(lv_event_t *event)
{
    lv_obj_t *mbox = lv_event_get_user_data(event);
    uint16_t btn_idx = lv_msgbox_get_active_btn(mbox);

    LOG_I("mbox_event VALUE_CHANGED: %d", btn_idx);
    if (0 == btn_idx)
    {
        rt_device_control(lcd_device, RTGRAPHIC_CTRL_POWEROFF, NULL);
        pm_shutdown();
    }
    else
    {
        /* Delete the parent modal background */
        lv_obj_del_async(lv_obj_get_parent(mbox));
        //Restore HOME key
        keypad_default_handler_register(default_keypad_handler);
    }
}

static void show_shutdown_msgbox(void)
{
    /* Create a base object for the modal background */
    lv_obj_t *obj = lv_obj_create(lv_scr_act());
    lv_obj_set_style_bg_color(obj, LV_COLOR_BLACK, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, LV_HOR_RES, LV_VER_RES);

    /* 中文界面。字符串直接写 UTF-8 字面量 —— 本工程里 app_clock_status_bar.c 的
     * notify_msgs[] 就是这么干的（"思澈科技欢迎您"），编译器和 LVGL 都按 UTF-8
     * 处理，走这条路最少意外。
     *
     * 字体必须显式指定：字模在 tiny55_full.ttf 里（实测 20902 个汉字，
     * "关机/确定/取消"都在），但 LVGL 的字库是"按字体对象"查的，
     * 默认主题字体不保证覆盖 CJK。缺字时 LVGL 不报错、直接画不出来 ——
     * 屏幕上就是一片空白，日志里一个字都没有，属于最难查的那类。
     * 所以这里把标题、正文、按钮三处都显式挂上字体。 */
    static const char *btns2[] = {"确定", "取消", ""};

    /* Create the message box as a child of the modal background */
    lv_obj_t *mbox = lv_msgbox_create(obj, "关机",
                                      "确定要关机吗？", btns2, false);
    lv_obj_align(mbox, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(mbox, mbox_event_cb, LV_EVENT_VALUE_CHANGED, mbox);

    lv_ext_set_local_font(mbox, FONT_NORMAL, LV_COLOR_WHITE);
#ifdef LV_USE_MSGBOX
    lv_ext_set_local_font(lv_msgbox_get_title(mbox), FONT_TITLE,   LV_COLOR_WHITE);
    lv_ext_set_local_font(lv_msgbox_get_text(mbox),  FONT_NORMAL,  LV_COLOR_WHITE);
    lv_ext_set_local_font(lv_msgbox_get_btns(mbox),  FONT_NORMAL,  LV_COLOR_WHITE);
#endif

    /* Fade the message box in with an animation */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_time(&a, 500);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_50);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)opa_anim);
    lv_anim_start(&a);

    //Disable HOME key
    keypad_default_handler_register(NULL);
    //lv_label_set_text(info, in_msg_info);
    //lv_obj_align(info, NULL, LV_ALIGN_IN_BOTTOM_LEFT, 5, -5);
}

static void button_event_task_entry(struct _lv_timer_t *task)
{
    rt_uint32_t evt;
    rt_err_t err;

#ifdef BSP_USING_PM
    /* 调试用开关，见文件末尾的 keepawake 命令。
     *
     * 为什么非要有它：息屏之后 gui_suspend() 会把 app_watch 线程整个抱住，
     * 系统随 PM 框架进入低功耗态，uart1 的时钟被关掉 —— 串口控制台彻底没反应。
     * 也就是说"板子一睡着，我就再也无法从串口查任何东西"。
     * 把息屏关掉，串口才活着，才谈得上查 RTC / 查表盘。 */
    if (pm_keepawake)
    {
        return;
    }
#endif /* BSP_USING_PM */

    /* ⚠ 蓝牙在广播中 / 已连上时不让息屏。
     * 息屏 = gui_suspend + PM 深睡 = 射频域断电，表现就是"手机刚连上就断"。
     * 判断条件在 ble_app_wants_awake() 里，这里只是尊重它的结论。 */
    if ((RT_FALSE == ble_app_wants_awake()) &&
            (lv_disp_get_inactive_time(NULL) > IDLE_TIME_LIMIT))
    {
        gui_pm_fsm(GUI_PM_ACTION_SLEEP);
    }

    err = rt_event_recv(&btn_event, BTN_EVT_ALL, RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, RT_WAITING_NO, &evt);

    if (RT_EOK != err)
    {
        return;
    }

    if (evt & BTN_EVT_SHUTDOWN)
    {
        lv_disp_trig_activity(NULL);
        show_shutdown_msgbox();
    }
}

static void pm_event_handler(gui_pm_event_type_t event)
{
    switch (event)
    {
    case GUI_PM_EVT_SUSPEND:
    {
        lv_timer_enable(false);
        break;
    }
    case GUI_PM_EVT_RESUME:
    {
        lv_timer_enable(true);
        break;
    }
    case GUI_PM_EVT_SHUTDOWN:
    {
        //TODO: start power down procedure
        RT_ASSERT(RT_EOK == rt_event_send(&btn_event, BTN_EVT_SHUTDOWN));
        break;
    }
    default:
    {
        RT_ASSERT(0);
    }
    }
}
#else
#define rt_pm_request(mode)
#define rt_pm_release(mode)
#endif /* BSP_USING_PM */

#ifdef BSP_USING_DFU
#include "bf0_ble_dfu.h"

static void dfu_btn_event_cb(lv_obj_t *obj, lv_event_t evt)
{
    if ((evt == LV_EVENT_DELETE) && (obj == mbox))
    {
        /* Delete the parent modal background */
        lv_obj_del_async(lv_obj_get_parent(mbox));
        mbox = NULL; /* happens before object is actually deleted! */
        //lv_label_set_text(info, welcome_info);
    }
    else if (evt == LV_EVENT_VALUE_CHANGED)
    {
        uint16_t btn_idx = lv_msgbox_get_active_btn(obj);
        if (0 == btn_idx)
        {
            rt_device_control(lcd_device, RTGRAPHIC_CTRL_POWEROFF, NULL);
            rt_hw_cpu_reset();
        }
        else
        {
            /* A button was clicked */
            lv_msgbox_start_auto_close(mbox, 0);
        }
    }
}

static void opa_anim(void *bg, lv_anim_value_t v)
{
    lv_obj_set_style_local_bg_opa(bg, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, v);
}


static void show_dfu_reboot_msgbox(void)
{
    /* Create a base object for the modal background */
    lv_obj_t *obj = lv_obj_create(lv_scr_act(), NULL);
    _lv_obj_set_style_local_color(obj, LV_OBJ_PART_MAIN, LV_STYLE_BG_COLOR, LV_COLOR_BLACK);
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, LV_HOR_RES, LV_VER_RES);

    static const char *btns2[] = {"Ok", "Cancel", ""};

    /* Create the message box as a child of the modal background */
    mbox = lv_msgbox_create(obj, NULL);
    lv_msgbox_add_btns(mbox, btns2);
    lv_msgbox_set_text(mbox, "Upgrade firmware is ready! Do you want to update?");
    lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_event_cb(mbox, dfu_btn_event_cb);

    /* Fade the message box in with an animation */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_time(&a, 500);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_50);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)opa_anim);
    lv_anim_start(&a);

    //lv_label_set_text(info, in_msg_info);
    //lv_obj_align(info, NULL, LV_ALIGN_IN_BOTTOM_LEFT, 5, -5);
}



uint8_t app_dfu_callback(uint16_t event, void *param)
{
    uint8_t ret = BLE_DFU_EVENT_SUCCESSED;
    switch (event)
    {
    case BLE_DFU_END:
    {
        ble_dfu_end_t *ret = (ble_dfu_end_t *)param;
        LOG_I("app dfu reset start %d", ret->result);
        if (ret->result == 0)
            show_dfu_reboot_msgbox();
        break;
    }
    default:
        break;
    }


    return ret;
}
#else
#define ble_dfu_register(cbk)
#endif

#if WATCH_DIAG_BOOTCLOCK
/* 见文件顶部 WATCH_DIAG_BOOTCLOCK 的说明。
 *
 * 用 RT-Thread 软件定时器而不是 lv_timer，是故意的：如果 lv_tick 真的不走了，
 * 挂在 LVGL 上的定时器压根不会被调用，那等于拿一只坏钟去核对时间。
 * 软件定时器由系统 tick 驱动，独立于 LVGL，能给出干净的对照。 */
static rt_timer_t diag_clk_timer = RT_NULL;
static int        diag_clk_n     = 0;

static void diag_clock_cb(void *parameter)
{
    (void)parameter;

    time_t     now = time(RT_NULL);
    struct tm *t   = localtime(&now);
    char       tbuf[24];

    if (t == RT_NULL)
    {
        rt_strncpy(tbuf, "(time()==NULL)", sizeof(tbuf));
    }
    else
    {
        rt_snprintf(tbuf, sizeof(tbuf), "%04d-%02d-%02d %02d:%02d:%02d",
                    t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                    t->tm_hour, t->tm_min, t->tm_sec);
    }

    rt_kprintf("clkdiag %02d/%02d  rtc=%s (%ld)  tick=%u  idle=%u\n",
               diag_clk_n + 1, DIAG_SAMPLES, tbuf, (long)now,
               (unsigned)lv_tick_get(),
               (unsigned)lv_disp_get_inactive_time(NULL));

    diag_clk_n++;
    if (diag_clk_n >= DIAG_SAMPLES && diag_clk_timer != RT_NULL)
    {
        rt_timer_stop(diag_clk_timer);
        rt_kprintf("clkdiag: done, %d samples\n", diag_clk_n);
    }
}
#endif /* WATCH_DIAG_BOOTCLOCK */

void app_watch_entry(void *parameter)
{
    uint8_t first_loop = 1;
#ifdef _MSC_VER
    {
        extern int wait_platform_init_done(void);
        wait_platform_init_done();
    }
#else
    {
        /* 这里以前是每次开机都硬写 set_date(2022,7,1); set_time(9,0,0);
         * 等于把 RTC 里的真实时间冲掉 —— 表盘永远显示 2022-07-01 09:00，
         * 看着就像"时间没实现"。现在改成：RTC 里已经是合理时间就原样保留，
         * 只有明显没被设过（年份 < 2024）才兜一个默认值，免得显示 1970。
         *
         * ⚠ 关于这个兜底值，有一件事必须说清楚（2026-09-20 实测确认）：
         *   这块板子**没有 RTC 备份电池**。软复位（reboot）RTC 值保持，
         *   但拔电再上电 RTC 域整块掉电，值归零 → 每次冷启动都会走到这个兜底。
         *   而兜底值 2024-01-01 12:00 长得像个正常时间，屏幕上一句"12:00"
         *   既不像坏也不像没设，反而把人绕进去了（反馈原话"时间一直是 12"）。
         *   所以表盘那边（app_clock_band.c）按 RTC_SET_MIN_YEAR 划了条线，
         *   年份 < 2025 就挂「未校时 · 请重新对时」提示。**两处的年份必须一致。**
         *
         *   想彻底解决冷启动丢时，只能靠外部时间源 —— 计划走蓝牙（BLE）校时。
         *
         * 校时办法：在 finsh/shell 里敲
         *     show_date 2026 9 20 19 30 0        (年 月 日 时 分 秒)
         * 想确认 RTC 现在是什么时间，只敲 show_date 不加参数即可；
         * 也可以敲 rtc_now —— 它走的是和表盘完全相同的取时间路径。 */
        time_t now = time(RT_NULL);
        struct tm *tm_now = localtime(&now);

        if (tm_now == RT_NULL)
        {
            /* time() 都拿不到值，说明 RTC 设备没就绪。先记一笔再去兜底。 */
            LOG_W("rtc at boot: time() failed (rtc device not ready?)");
        }
        else
        {
            LOG_I("rtc at boot: %04d-%02d-%02d %02d:%02d:%02d",
                  tm_now->tm_year + 1900, tm_now->tm_mon + 1, tm_now->tm_mday,
                  tm_now->tm_hour, tm_now->tm_min, tm_now->tm_sec);
        }

        if (tm_now == RT_NULL || tm_now->tm_year < (2024 - 1900))
        {
            LOG_W("rtc at boot: value unusable -> writing fallback 2024-01-01 12:00");
            set_date(2024, 1, 1);
            set_time(12, 0, 0);

            /* 立刻回读一次：这样日志里能直接看出 set_date/set_time 到底有没有生效，
             * 不用再靠"屏幕上的数字对不对"去反推。 */
            now = time(RT_NULL);
            tm_now = localtime(&now);
            if (tm_now != RT_NULL)
            {
                LOG_I("rtc after fallback: %04d-%02d-%02d %02d:%02d:%02d",
                      tm_now->tm_year + 1900, tm_now->tm_mon + 1, tm_now->tm_mday,
                      tm_now->tm_hour, tm_now->tm_min, tm_now->tm_sec);
            }
        }
    }
#endif /* _MSC_VER */

#if WATCH_DIAG_BOOTCLOCK
    /* 开机时钟自检，见文件顶部说明。这里起一只软件定时器，60 秒后自己停。 */
    diag_clk_timer = rt_timer_create("clkdiag", diag_clock_cb, RT_NULL,
                                     rt_tick_from_millisecond(DIAG_PERIOD_MS),
                                     RT_TIMER_FLAG_PERIODIC | RT_TIMER_FLAG_SOFT_TIMER);
    if (diag_clk_timer != RT_NULL)
    {
        rt_timer_start(diag_clk_timer);
        LOG_I("clkdiag: started, %d samples every %dms", DIAG_SAMPLES, DIAG_PERIOD_MS);
    }
    else
    {
        LOG_W("clkdiag: rt_timer_create failed, no clock self-check");
    }
#endif

    init_pin();
    lcd_device = rt_device_find(LCD_DEVICE_NAME);

#ifdef BSP_USING_PM
    rt_event_init(&btn_event, "btn", RT_IPC_FLAG_FIFO);

    int8_t wakeup_pin;
    uint16_t gpio_pin;
    GPIO_TypeDef *gpio;

    gpio = GET_GPIO_INSTANCE(SLEEP_CTRL_PIN);
    gpio_pin = GET_GPIOx_PIN(SLEEP_CTRL_PIN);

    wakeup_pin = HAL_HPAON_QueryWakeupPin(gpio, gpio_pin);

    if (wakeup_pin >= 0)
    {
        /* ── 息屏（sleep）期间的按键唤醒 ──
         * 唤醒源在 HPAON 域，双边沿触发：按下和松手都能把 CPU 叫醒。 */
        pm_enable_pin_wakeup(wakeup_pin, AON_PIN_MODE_DOUBLE_EDGE);

        /* ── 关机（hibernate）期间的按键唤醒 ──
         * 这一套走的是 PMUC 域的寄存器，和上面 HPAON 那套完全是两回事，
         * 少配一边就会出现「关得掉、开不了」。
         *
         * 两个约束来自芯片本身，不是我随便填的：
         *   1) 引脚序号只能用 0 或 1 —— PMUC_WSR_PIN_ALL 只包含 PIN0/PIN1；
         *   2) 电平用「高」—— KEY1 是高电平有效（BSP_KEY1_ACTIVE_HIGH=1），
         *      按下时引脚被拉高，所以要让 PMU 在高电平上唤醒。
         *
         * 这两句写一次就一直有效（寄存器在掉电前保持），
         * 所以放在启动时配，不用等到关机那一刻再配。
         * 顺带打开 RTC 唤醒，以后想做「闹钟开机」直接就能用。 */
        HAL_PMU_SelectWakeupPin(0, (uint8_t)wakeup_pin);     /* 0 = hibernate 的 PIN0 */
        HAL_PMU_EnablePinWakeup(0, AON_PIN_MODE_HIGH);
        HAL_PMU_EnableRtcWakeup();

        LOG_I("pm: wakeup ready  KEY1(pin %d) -> AON %d  |  sleep=double-edge, hibernate=high-level",
              SLEEP_CTRL_PIN, wakeup_pin);
    }
    else
    {
        /* 换板子/改引脚时可能落到非 AON 引脚上。那种情况下按键叫不醒，
         * 但整个固件不该因此起不来 —— 所以只告警，不 assert。 */
        LOG_W("pm: KEY1(pin %d) 不是 AON 唤醒源，息屏/关机后将无法用按键唤醒", SLEEP_CTRL_PIN);
    }

    gui_ctx_init();
    gui_pm_init(lcd_device, pm_event_handler);
#endif /* BSP_USING_PM */

    /* ── 蓝牙校时 ──
     * 放在这里、LVGL 初始化之前，理由有两个：
     *   1) ble_app_init() 只依赖 RT-Thread（邮箱 + 线程 + 定时器），不碰 LVGL，
     *      所以它可以先起来；
     *   2) 协议栈从使能到 BLE_POWER_ON_IND 有一段启动时间（要等 LCPU 那边
     *      把射频拉起来），早些开始等，用户进表盘时它多半已经就绪。
     * 顺序上必须在 gui_pm_init 之后：再早 PM 框架还没建好。 */
    ble_app_init();

    /* init littlevGL */
    {
        rt_err_t r = littlevgl2rtt_init(LCD_DEVICE_NAME);
        RT_ASSERT(RT_EOK == r);
    }

    lv_ex_data_pool_init();
    resource_init();
#if LV_USING_FREETYPE_ENGINE
    lv_freetype_open_font(true);                                /* open freetype */
#endif
    gui_app_init();

#ifdef BSP_USING_PM
    button_event_task = lv_timer_create(button_event_task_entry, 30, 0);
#endif /* BSP_USING_PM */
    keypad_default_handler_register(default_keypad_handler);

    /* 开机直接进表盘，不再先落到主菜单。
     *
     * 原厂这里是 gui_app_run("Main")，所以每次开机看到的是应用列表；
     * 用户要的是「开机先显示表盘」。
     *
     * 为什么直接启动 "clock" 而不是「先 Main 再 clock」：
     * 应用框架（middleware/app_fwk）不要求桌面 app 先跑起来才能跑别的 app ——
     * 开机第一个 app 走的就是「没有前一个 app」的首启路径，Main 和 clock 走
     * 同一条路，没有额外前提。
     *
     * ⚠ 主菜单没有消失，只是不再自动弹出：短按 KEY1 在「表盘 ⇄ 主菜单」之间
     *   来回切（default_keypad_handler 里就是这句，它不需要 Main 已经在跑，
     *   没跑就启动它）。调试时也可以用串口命令 to_clock / gui_app_run。 */
    gui_app_run("clock");
    lv_disp_trig_activity(NULL);
#if defined(GUI_APP_FRAMEWORK)&&(!defined (APP_TRANS_ANIMATION_NONE))
    lvsf_gesture_init(lv_layer_top());
#endif /* defined(GUI_APP_FRAMEWORK)&&(!defined (APP_TRANS_ANIMATION_NONE)) */

    /* 右边缘「从右往左滑」返回上一页。
     *
     * 必须排在 lvsf_gesture_init() 之后：那个函数在 lv_layer_top() 上建的是
     * 「左边缘往右滑」的检测条（SDK 里右侧那条被 #if 0 注释掉了，见
     * middleware/lvgl/lvsf/lvsf_gesture.c）。两者挂在同一层、互不重叠，
     * 各占一边，顺序其实没有依赖，但摆在一起读起来才清楚是一对。
     *
     * 也必须在 littlevgl2rtt_init() 之后 —— 这个函数要建 LVGL 对象、读屏幕尺寸。
     * 位置就在 gui_app_run("clock") 反正是开机流程的末尾，LVGL 早就起来了。 */
    swipe_back_init();

    while (1)
    {
        int ms;

        rt_pm_request(PM_SLEEP_MODE_IDLE);
        ms = lv_timer_handler();
        rt_pm_release(PM_SLEEP_MODE_IDLE);

#ifdef BSP_USING_PM
        if (gui_is_force_close())
        {
            bool lcd_drawing;
            rt_device_control(lcd_device, RTGRAPHIC_CTRL_GET_BUSY, &lcd_drawing);
            if (!lcd_drawing)
            {
                LOG_I("no input:%d", lv_disp_get_inactive_time(NULL));
                gui_suspend();
                LOG_I("ui resume");
                /* force screen to redraw */
                lv_obj_invalidate(lv_scr_act());
                /* reset activity timer */
                lv_disp_trig_activity(NULL);
            }
            else if (ms > 0)
            {
                rt_thread_mdelay(ms);       /* Just to let the system breathe */
            }
        }
        else
#endif  /* BSP_USING_PM */
        {
            //EventStartB(0);
            if (ms > 0)
                rt_thread_mdelay(ms);       /* Just to let the system breathe */
            //EventStopB(0);
        }

        if (first_loop)
        {
#ifndef WIN32
            //Turn on lcd backlight after power on
            uint8_t brightness = 100;
            rt_device_control(lcd_device, RTGRAPHIC_CTRL_SET_BRIGHTNESS, &brightness);
#endif /* WIN32 */
            first_loop = 0;
        }
    }

}

void app_register(void)
{
}

int app_watch_init(void)
{
    rt_err_t ret = RT_EOK;
    rt_thread_t thread = RT_NULL;


    ret = rt_thread_init(&watch_thread, "app_watch", app_watch_entry, RT_NULL, watch_thread_stack, APP_WATCH_GUI_TASK_STACK_SIZE,
                         RT_THREAD_PRIORITY_MIDDLE, RT_THREAD_TICK_DEFAULT);

    if (RT_EOK != ret)
    {
        return RT_ERROR;
    }
    rt_thread_startup(&watch_thread);
    return RT_EOK;
}

#if !defined (_MSC_VER)
#define APP_MEM_THRESHOLD 16384
void *cxx_mem_allocate(size_t size)
{
    if (size > APP_MEM_THRESHOLD)
    {
        void *p = app_anim_mem_alloc(size, 1);
        rt_kprintf("Allocate %d, %p\n", size, p);
        return p;
    }
    else
        return rt_malloc(size);
}

extern int rt_in_system_heap(void *ptr);
void cxx_mem_free(void *ptr)
{
    if (rt_in_system_heap(ptr))
        rt_free(ptr);
    else
    {
        rt_kprintf("Free %p\n", ptr);
        app_anim_mem_free(ptr);
    }
}
#endif

/* ── 串口命令：手动关机（进休眠） ──
 *
 * 长按 KEY1 也能关机（会先弹确认框），但那是"按键->状态机->对话框"一长串，
 * 中间任何一环出问题都看不出来。这条命令把前半段全绕开，只留最后一步，
 * 用来分段定位很省事：
 *     敲完屏幕立刻黑        -> 休眠通道通
 *     再按一下 KEY1 屏幕亮  -> 唤醒通道也通
 *     敲完没反应            -> 问题在休眠之前，和唤醒无关
 *
 * 唤醒源在启动时就配好了（见 app_watch_entry 里的 HAL_PMU_* 那几行）。 */
#ifdef BSP_USING_PM
#ifdef RT_USING_FINSH
/* 命令名以函数名为准！MSH_CMD_EXPORT(fn, desc) 里的宏展开是
 *     const char __fsym_##cmd##_name[] = #cmd;
 * 也就是把**函数名本身**注册成命令名。所以下面这几个函数刻意不带 cmd_ 前缀 ——
 * 带上的话，串口里就得敲 cmd_rtc_now 才认，而注释和脚本里写的是 rtc_now，
 * 一敲就 "command not found"。这个坑已经踩过一次（2026-09-20）。
 *
 * 参数名不要叫 argc / argv：newlib 里已经有同名的全局符号，
 * 会报 "'argc' redeclared as different kind of symbol"。 */
static int poweroff(int nr_args, char **args)
{
    (void)nr_args;
    (void)args;
    rt_kprintf("power off: entering hibernate, press KEY1 to wake up\n");
    rt_device_control(lcd_device, RTGRAPHIC_CTRL_POWEROFF, NULL);
    pm_shutdown();
    return 0;
}
MSH_CMD_EXPORT(poweroff, enter hibernate / power off);

/* ── 串口命令：关掉/打开无操作息屏 ──
 *
 * 息屏之后整个 GUI 线程被 gui_suspend() 抱住，uart1 也跟着进低功耗，
 * 串口控制台不再响应任何输入。也就是说只要板子睡着了，就没法从串口查东西。
 * 调试时先敲 `keepawake` 把它钉住，之后 show_date、看日志都随便查。
 *
 * 注意：它只是跳过"无操作息屏"这一步，不影响长按关机、也不影响按 KEY1。
 * 重新上电后自动恢复成 0（不持久化）。 */
static int keepawake(int nr_args, char **args)
{
    (void)nr_args;
    (void)args;

    pm_keepawake = pm_keepawake ? RT_FALSE : RT_TRUE;

    /* 顺手把无操作计时清一下，免得刚切到 true 就被上一轮的累积值误判 */
    lv_disp_trig_activity(NULL);

    rt_kprintf("idle screen-off: %s\n", pm_keepawake ? "DISABLED (kept awake)" : "enabled (10s)");
    return 0;
}
MSH_CMD_EXPORT(keepawake, toggle 10s idle screen-off (debug));

/* ── 串口命令：打印当前 RTC 时间 ──
 *
 * 和 SDK 自带的 show_date 区别：这条走的是和表盘完全相同的取时间路径
 * （time() + localtime()），所以它能回答"表盘上那个数字是从哪来的"。
 * 连敲两次（间隔十几秒）对比秒数，就能判断 RTC 到底在不在走数 ——
 * 这是"表盘时间冻住"最直接的分诊手段。 */
static int rtc_now(int nr_args, char **args)
{
    (void)nr_args;
    (void)args;

    time_t now = time(RT_NULL);
    struct tm *t = localtime(&now);

    if (t == RT_NULL)
    {
        rt_kprintf("rtc_now: time() returned nothing\n");
        return 0;
    }

    rt_kprintf("rtc_now: %04d-%02d-%02d %02d:%02d:%02d  (time()=%ld)\n",
               t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
               t->tm_hour, t->tm_min, t->tm_sec, (long)now);
    return 0;
}
MSH_CMD_EXPORT(rtc_now, print RTC time the same way the watchface reads it);

/* ── 串口命令：从串口切到表盘 ──
 *
 * 开机默认落在 Main（蜂窝主菜单），要用按键或触摸才能进表盘。
 * 调试时不想按键就用这条。gui_app_run() 只是往 GUI 邮箱发消息，
 * 从 shell 线程调是安全的。 */
static int to_clock(int nr_args, char **args)
{
    (void)nr_args;
    (void)args;
    rt_kprintf("switching to clock app...\n");
    gui_app_run("clock");
    return 0;
}
MSH_CMD_EXPORT(to_clock, switch to the clock/watchface app);

/* ── 串口命令：蓝牙校时 ──
 *
 *   ble            看状态（协议栈起没起 / 在不在广播 / 连上没有 / 校过几次）
 *   ble on         打开广播（等效于按状态栏里的 BT 按钮）
 *   ble off        关掉广播
 *
 * 为什么必须留一条串口命令：蓝牙是"看不到的那一半"。
 * 屏幕上只能显示广播开没开，而"协议栈到底起没起来""手机连上没""时间写进来没"
 * 这些只有日志说得清。而且板子一息屏串口就哑，所以调试时先 keepawake。
 *
 * 命令名以函数名为准（MSH_CMD_EXPORT(fn,desc) 注册的就是函数名本身），
 * 所以这里函数名就叫 ble，不是 cmd_ble。参数名避开 argc/argv。 */
static int ble(int nr_args, char **args)
{
    if (nr_args >= 2)
    {
        if (0 == rt_strcmp(args[1], "on"))
        {
            ble_app_radio(1);
            rt_kprintf("ble: radio ON requested\n");
            return 0;
        }
        if (0 == rt_strcmp(args[1], "off"))
        {
            ble_app_radio(0);
            rt_kprintf("ble: radio OFF requested\n");
            return 0;
        }
        if (0 == rt_strcmp(args[1], "state"))
        {
            /* 落到下面统一打印 */
        }
        else
        {
            rt_kprintf("ble: usage -> ble | ble on | ble off\n");
            return 0;
        }
    }

    rt_kprintf("ble: state=%s  adv_on=%d  connected=%d  stack_ready=%d  sync_count=%d\n",
               ble_app_state_str(),
               (int)ble_app_radio_on(),
               (int)ble_app_connected(),
               (int)ble_app_ready(),
               (int)ble_app_sync_count());

    /* ★ 顺手把两个时钟打出来 —— 排"板子是睡着了还是跑飞了"必须靠它们：
     *   tick  : RT-Thread 节拍（真实毫秒），一直在涨说明系统还活着
     *   rtc   : 真实时间，一直在涨说明 RTC 没被清
     * 注意别拿它跟串口行首那个 [编号] 比：那个编号比真实时间快 32.77 倍，
     * 原因见 ble_app.c 末尾「关于日志时间戳」。 */
    {
        time_t     now = time(RT_NULL);
        struct tm *t   = localtime(&now);

        rt_kprintf("ble: tick=%u  rtc=%04d-%02d-%02d %02d:%02d:%02d\n",
                   (unsigned)rt_tick_get(),
                   t ? t->tm_year + 1900 : 0, t ? t->tm_mon + 1 : 0, t ? t->tm_mday : 0,
                   t ? t->tm_hour : 0, t ? t->tm_min : 0, t ? t->tm_sec : 0);
    }

    return 0;
}
MSH_CMD_EXPORT(ble, BLE time sync: ble | ble on | ble off);
#endif /* RT_USING_FINSH */
#endif /* BSP_USING_PM */

/* ══════════════════════════════════════════════════════════════════════════
 *  上划面板那几个按钮要用的「能力入口」
 * ══════════════════════════════════════════════════════════════════════════
 *  面板（app_clock_status_bar.c）只负责"点"，真正干活的东西住在这儿：
 *  lcd_device 和 PM 状态机都是本文件的东西。所以这边各开一个入口函数，
 *  面板那边 extern 声明后直接调 —— 它不需要知道 lcd_device、PM 这些细节。
 *
 *  刻意放在上面那两个 #endif 之外：面板是无条件调用的，这两个符号必须
 *  任何配置下都存在（关掉 finsh 或 PM 也不能链接不上）。所以不用
 *  MSH_CMD_EXPORT，就是个普通函数。
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── 背光档位循环 ──
 *
 * 0 = 关背光、100 = 全亮 —— 见 drv_lcd.c 的 lcd_set_brightness()，
 * 它是真的去调背光 PWM 的（LCD_USING_PWM_AS_BACKLIGHT 那套），不是空架子。
 * 这件事在动手前专门核实过：RTGRAPHIC_CTRL_SET_BRIGHTNESS 在
 * drv_lcd.c 的 msg 处理里有 case，不然就成了又一个死控件。
 *
 * 从第二档开始循环：第一次点它应该"变暗一点"，而不是先跳到 100（开机本来就是 100）。 */
static const uint8_t watch_bright_levels[] = { 100, 60, 30, 10 };
static uint8_t watch_bright_idx = 0;

/* 当前背光（%）。开机就是 100（app_watch_init 里那句 SET_BRIGHTNESS 100）。
 *
 * 手电筒 app（gui_apps/torch）需要"进来之前是多少、退出时恢复回去"，
 * 所以这里额外开一对 get/set 入口 —— 和 app_watch_sleep_now 一样的思路：
 * 能力留在本文件，别人只看到两个函数，不用知道 lcd_device 是什么。 */
static uint8_t watch_bright_cur = 100;

uint8_t app_watch_brightness_get(void)
{
    return watch_bright_cur;
}

/* 直接设背光（%）。0 = 关背光、100 = 全亮。
 * ⚠ 谁改谁负责改回来：手电筒退出时会调 set(saved)，否则用户会莫名其妙
 *   发现"屏幕亮度变了"。 */
void app_watch_brightness_set(uint8_t b)
{
    if (lcd_device == RT_NULL)
    {
        rt_kprintf("bright: no lcd device\n");
        return;
    }

    if (b > 100)
    {
        b = 100;
    }

    watch_bright_cur = b;
    rt_device_control(lcd_device, RTGRAPHIC_CTRL_SET_BRIGHTNESS, &b);
    rt_kprintf("bright: set %d%%\n", (int)b);
}

/* 循环档位：从第二档开始，第一次点应该"变暗一点"，而不是先跳到 100
 * （开机本来就是 100）。 */
void app_watch_brightness_cycle(void)
{
    watch_bright_idx = (uint8_t)((watch_bright_idx + 1) %
                                 (sizeof(watch_bright_levels) / sizeof(watch_bright_levels[0])));

    app_watch_brightness_set(watch_bright_levels[watch_bright_idx]);
}

/* ── 立刻息屏 ──
 *
 * 走的是和无操作超时**完全相同**的入口（gui_pm_fsm(GUI_PM_ACTION_SLEEP)，
 * 见上面 app_watch_entry 里那句），区别只是不等那 10 秒。
 * 顺带说明为什么不是"仿照短按 KEY1"：button_event_handler 只在屏幕已经
 * 不活跃（!gui_is_active()）时才把按键喂给 PM 状态机，亮屏时短按 KEY1 是
 * 翻页/回桌面，不会息屏。
 *
 * ⚠ 但必须先处理蓝牙：息屏 = gui_suspend + PM 深睡 = 射频域断电，
 *   正在广播/已连接时直接睡下去，就是"手机刚连上就断"那个坑
 *   （详见 ble_app.c 里 ble_app_wants_awake() 上面那段）。所以先按一次 BT
 *   把蓝牙收掉再睡 —— ble_app_radio(0) 会同时清掉 want_on，
 *   否则睡醒之后 want_on 还挂着 1，无操作息屏会被一直按住。 */
void app_watch_sleep_now(void)
{
#ifdef BSP_USING_PM
    /* ⚠ 这里**不看** pm_keepawake。keepawake 的契约是"只跳过无操作息屏这一步"
     * （见上面那条命令的注释：不影响长按关机、也不影响按 KEY1）。
     * 而"息屏"按钮是用户明确下的一次指令，不是超时自动行为 ——
     * 拿调试开关把它挡掉，表现就是"按了没反应"，正好是这次要修掉的病。
     *
     * 实锤：第一版这里加了 pm_keepawake 判断，用户按下去串口里只出现
     *   sleep: refused, keepawake is ON
     * 屏幕上什么都不会发生，而他看不到串口。调试时想保住串口，就别按这个按钮。 */

    if (ble_app_radio_on() || ble_app_connected() || ble_app_wants_awake())
    {
        rt_kprintf("sleep: BLE busy -> radio off first\n");
        ble_app_radio(0);

        /* 让 bleapp 线程把射频关干净再睡。只是等一次跨线程往返，
         * 短暂阻塞 GUI 线程可接受（按一次按钮而已）。 */
        rt_thread_mdelay(250);
    }

    rt_kprintf("sleep: request now\n");
    gui_pm_fsm(GUI_PM_ACTION_SLEEP);
#else
    rt_kprintf("sleep: PM not enabled in this build\n");
#endif
}

/* ── 串口命令：背光档位循环 ──
 *
 * 存在的理由只有一个：**不进菜单也能验证背光这条路是通的** —— 它和面板上的
 * 「亮度」按钮调的是同一个函数。这样"串口验证通过"才等于"按钮也是好的"，
 * 否则屏幕上有没有变暗只能靠眼睛，而这台设备拿不到截图。
 *
 * 放在这里（而不是上面那一堆命令里）是因为上面那批被包在
 * #ifdef BSP_USING_PM 里，而这条只受 finsh 开关管。 */
#ifdef RT_USING_FINSH
static int bright(int nr_args, char **args)
{
    (void)nr_args;
    (void)args;
    app_watch_brightness_cycle();
    return 0;
}
MSH_CMD_EXPORT(bright, cycle LCD backlight 100/60/30/10%);
#endif /* RT_USING_FINSH */

INIT_APP_EXPORT(app_watch_init);
