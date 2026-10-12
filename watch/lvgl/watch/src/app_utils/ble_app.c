/*
 * ble_app.c —— 蓝牙（BLE）校时，服务端实现
 *
 * ⚠ 为什么放在 src/app_utils/ 而不是 gui_apps/clock/：
 *   src/app_utils/SConscript 里本来就写着
 *       if GetDepend('BSP_BLE_SIBLES'):  src += ['ble_app.c']
 *   只要打开蓝牙，构建系统就要求这个文件名存在。BLE 是系统级服务，
 *   不归某个 App —— 表盘只是"其中一个用它的地方"（BT 按钮在状态栏里）。
 *
 * ┌─ 这块芯片上蓝牙是怎么回事（先把架构说清楚，不然出错时无从下手）──────────┐
 * │ SF32LB52X 上是**双核**：蓝牙协议栈（rwip / rwble / 射频调度）整个跑在       │
 * │ LCPU 上，LCPU 的代码在 ROM 里、加一个小 patch；HCPU 这边只是**客户端**，   │
 * │ 通过 mailbox 跟 LCPU 说话。                                                │
 * │ 证据：SDK 里 example/rom_bin/lcpu_general_ble_img/ 有预编译好的 LCPU 镜像  │
 * │ （lcpu_img_56x_52.axf 里有 rwip.o / rwble.o / rf_init / sch_*），          │
 * │ 而 src/app_utils/main.c 里调 rwip_init(0) 那句被 #ifdef SOC_BF0_LCPU 包着。│
 * │                                                                            │
 * │ ★ 实测纠正（2026-09-21，烧录后串口证实）：上面那条"不用搬镜像"的推论**是错的**。│
 * │   HCPU 必须把 LCPU 的固件一起带过去。缺了它症状很误导人：                 │
 * │     lcpu_power_on() 里的 HAL_HPAON_WakeCore() 死等 LCPU 应答，等不到；     │
 * │     串口永远不出 "BLE ready!"，BLE_POWER_ON_IND 永远不来；                │
 * │     而 HCPU 侧一切配置看上去都对（NVDS 通了、HCI 关了、patch 编了）。     │
 * │   补法只有一步 —— 工程的 project/SConstruct 里加：                        │
 * │       AddLCPU(SIFLI_SDK, rtconfig.CHIP)                                   │
 * │   它让构建系统另编一个 core="LCPU" 的子工程（SDK 的 lcpu_general），       │
 * │   产出 lcpu.bin -> lcpu.c -> lcpu.o，随主镜像一起烧进去。                  │
 * │   判断是否生效：编译日志里有 "LINK ...\lcpu\lcpu.elf"，                    │
 * │   且 main.bin 比不加时大约大 5.7KB。                                       │
 * └────────────────────────────────────────────────────────────────────────────┘
 *
 * 代码结构（三块）：
 *   1) 事件层   app_ble_event_handler() —— 收协议栈事件。**只投邮箱，不干活**
 *                （在 BLE 任务上下文里注册服务会阻塞，官方示例也是绕出去做的）
 *   2) 工作线程 ble_thread_entry()      —— 真正注册 CTS 服务 / 起停广播
 *   3) CTS 回调 ble_tips_cb()           —— 手机写时间进来，这里落到 RTC
 *
 * 对外只有 ble_app.h 那几个函数（状态栏 BT 按钮 / 串口 ble 命令都调它）。
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include <string.h>
#include <time.h>

#include "ble_app.h"
#include "ble_ai.h"         /* AI 助手的蓝牙数据通道（注册串口透传服务） */

#ifdef BSP_BLE_TIMES

#include "bf0_sibles.h"
#include "bf0_sibles_advertising.h"
#include "bf0_ble_common.h"
#include "bf0_ble_gap.h"
#include "bf0_ble_tips.h"

/* ── 可调参数 ─────────────────────────────────────────────────────────── */

/* 广播时手机上显示的名字 */
#define BLE_DEV_NAME            "HS-Watch"

/* 广播开多久自动关（毫秒）。省电用：忘了关也不会一直耗着。
 * 5 分钟是给"第一次用"留的余量：手忙脚乱找手机、开浏览器都够。
 *
 * ⚠ 这里的"毫秒"走的是 rt_tick_from_millisecond()，也就是**真实的 5 分钟**。
 *   不要拿串口日志里那个 [123456] 去核对 —— 那个数和真实时间差 32.77 倍，
 *   详见文件末尾「关于日志时间戳」那段。 */
#define BLE_AUTO_OFF_MS         (300000)    /* 5 分钟 */

/* 校时成功后再留多久就关（毫秒）。用户做完一件事就不用再广播了。 */
#define BLE_AFTER_SYNC_MS       (15000)     /* 15 秒 */

/* 本地时区，单位 15 分钟。+32 = UTC+8（中国）。
 * 这个值只影响手机读「本地时间信息(0x2A0F)」时看到的东西，
 * 不影响写进来的时间本身 —— 写进来的年/月/日/时/分/秒是**本地时间**，
 * 直接进 RTC。换时区改这一个数就行。 */
#define BLE_TZ_15MIN            (32)

/* 开机后等多久才开始点蓝牙（毫秒）。
 *
 * ★ 为什么要等：`sifli_ble_enable()` 里含 LCPU 上电 + **全频段射频校准**，
 *   它会抢 LCD 的时钟/总线。实测对照（同一块板子，只差蓝牙）：
 *       无蓝牙：LCD 3.3s 开始 -> 12.0s 完成 -> 13.4s 屏亮
 *       有蓝牙：LCD 3.4s 开始 -> 48.1s 完成 -> 49.5s 屏亮   ← 黑屏快 50 秒
 *   原因是 LCD 初始化那几步（读 CO5300 的 ID）在校准期间拿不到正确的时钟，
 *   只能重试到校准结束。
 *   让蓝牙晚一点起，表盘就先亮出来给用户看；校准挪到表盘已经显示之后。
 *
 * 15 秒这个数：LCD 12 秒完成、13.4 秒亮、LVGL 首帧约 15 秒。
 * 想再快/再稳就调它。 */
#define BLE_STACK_START_DELAY_MS  (15000)

/* ── 线程间消息 ───────────────────────────────────────────────────────── */
#define BLE_MSG_POWER_ON        1
#define BLE_MSG_RADIO_ON        2
#define BLE_MSG_RADIO_OFF       3

/* 工作线程等消息的超时（毫秒）。等超时不是错误 —— 是拿来当"心跳"用的：
 * 每 30 秒打一行状态，证明板子还醒着、广播还开着。
 *
 * 为什么非要这个心跳（2026-09-21 加了它才查出来的事）：
 *   屏幕一息，gui_suspend() 会把系统按进低功耗，串口控制台随之全哑。
 *   于是"板子睡着了"和"板子跑飞了"在串口上长得一模一样 —— 都是没输出。
 *   有心跳就能区分：广播中每隔 30 秒必须有一行。 */
#define BLE_HEARTBEAT_MS        (30000)

/* 预热兜底：从 ble_app_init 起最多按住屏幕多久（毫秒）。
 * 协议栈正常十几秒就起来；万一它没起来，也不能把表盘永久钉亮 ——
 * 到点就放弃，让表能正常息屏省电。 */
#define BLE_WARMUP_MAX_MS       (90000)

/* ── 状态 ─────────────────────────────────────────────────────────────── */
typedef struct
{
    uint8_t      inited;        /* ble_app_init 跑过 */
    uint8_t      warming_up;    /* 已安排起蓝牙，但协议栈还没就绪 */
    uint8_t      ready;         /* 收到过 BLE_POWER_ON_IND */
    uint8_t      tips_ready;    /* CTS 服务注册成功 */
    uint8_t      adv_inited;    /* sibles_advertising_init 跑过 */
    uint8_t      adv_on;        /* 正在广播 */
    uint8_t      connected;
    uint8_t      conn_idx;
    uint8_t      want_on;       /* 上层想要的状态（power-on 之前先记着） */
    rt_uint32_t  sync_count;
    rt_uint32_t  warm_start;    /* 开始预热那一刻的 tick，给兜底超时用 */
    rt_timer_t   auto_off;
} ble_env_t;

static ble_env_t g_ble;

static struct rt_mailbox g_mb;
static rt_uint32_t       g_mb_pool[8];
static rt_thread_t       g_thread = RT_NULL;

/* （重新）挂上"自动关广播"的定时器。
 *
 * 注意 rt_timer_control 的 RT_TIMER_CTRL_SET_TIME 要的是 **tick**，
 * 不是毫秒 —— 直接塞毫秒进去，在 tick 为 1000Hz 时正好差 1000 倍，
 * 表现是"说好 3 分钟，结果 3 秒就关了"。这里统一做换算。 */
static void ble_auto_off_arm(rt_uint32_t ms)
{
    rt_uint32_t ticks;

    if (g_ble.auto_off == RT_NULL)
    {
        return;
    }

    ticks = (rt_uint32_t)rt_tick_from_millisecond((rt_int32_t)ms);

    rt_timer_control(g_ble.auto_off, RT_TIMER_CTRL_SET_TIME, &ticks);
    rt_timer_start(g_ble.auto_off);
}

/* ══════════════════════════════════════════════════════════════════════
 *  CTS 服务：手机写时间进来 -> 落到 RTC
 * ══════════════════════════════════════════════════════════════════════ */

static ble_tips_cur_time_t        g_ct;    /* 当前时间（读/写都用它当缓冲） */
static ble_tips_local_time_info_t g_lti;
static ble_tips_ref_time_info_t   g_rti;

/* struct tm -> CTS 结构。注意两个换算：
 *   day_of_week: tm_wday 是 0=周日..6=周六；CTS 要求 1=周一..7=周日
 *   year       : tm_year 是从 1900 起的偏移，CTS 要绝对年份 */
static void rtc_to_ct(ble_tips_cur_time_t *ct)
{
    time_t     now = time(RT_NULL);
    struct tm *t   = localtime(&now);

    rt_memset(ct, 0, sizeof(*ct));

    if (t == RT_NULL)
    {
        return;
    }

    ct->date_time.year = (uint16_t)(t->tm_year + 1900);
    ct->date_time.month = (uint8_t)(t->tm_mon + 1);
    ct->date_time.day   = (uint8_t)t->tm_mday;
    ct->date_time.hour  = (uint8_t)t->tm_hour;
    ct->date_time.min   = (uint8_t)t->tm_min;
    ct->date_time.sec   = (uint8_t)t->tm_sec;
    ct->day_of_week     = (uint8_t)((t->tm_wday == 0) ? 7 : t->tm_wday);
    ct->fraction_256    = 0;
    ct->adjust_reason   = 1;    /* bit0 = 手动更新 */
}

/* 把手机写进来的时间落进 RTC。
 *
 * 先做合法性检查再写：这是**外部输入**，不能默认它是对的。
 * 一个越界的年份（比如 1970 或者 65535）写进去，表盘会显示成什么样先不说，
 * 校时面板 / 未校时判断全都会跟着乱。宁可拒掉并打日志。 */
static void ble_apply_time(const ble_tips_cur_time_t *ct)
{
    int y  = ct->date_time.year;
    int mo = ct->date_time.month;
    int d  = ct->date_time.day;
    int h  = ct->date_time.hour;
    int mi = ct->date_time.min;
    int s  = ct->date_time.sec;

    rt_kprintf("ble_time: phone wrote %04d-%02d-%02d %02d:%02d:%02d (dow=%d reason=%d)\n",
               y, mo, d, h, mi, s, ct->day_of_week, ct->adjust_reason);

    if (y < 2024 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > 31 ||
            h > 23 || mi > 59 || s > 59)
    {
        rt_kprintf("ble_time: REJECTED, field out of range (nothing written)\n");
        return;
    }

    set_date(y, mo, d);
    set_time(h, mi, s);

    /* 立刻回读一次 —— 和触摸校时面板一样的习惯：不靠"应该写进去了"，
     * 要把 RTC 现在真实的值打出来。 */
    {
        time_t     now = time(RT_NULL);
        struct tm *t   = localtime(&now);

        if (t != RT_NULL)
        {
            rt_kprintf("ble_time: applied -> rtc now %04d-%02d-%02d %02d:%02d:%02d\n",
                       t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                       t->tm_hour, t->tm_min, t->tm_sec);
        }
        else
        {
            rt_kprintf("ble_time: applied, but read-back failed\n");
        }
    }

    g_ble.sync_count++;

    /* 用户已经做完事了，把自动关的时间缩短，别一直广播耗电 */
    ble_auto_off_arm(BLE_AFTER_SYNC_MS);
}

/* CTS 回调。SDK 的 bf0_ble_tips.c 在这几个事件上叫我们：
 *   GET_CURRENT_TIME      -> 必须返回 ble_tips_cur_time_t*
 *   GET_LOCAL_TIME_INFO   -> 必须返回 ble_tips_local_time_info_t*
 *   GET_REF_TIME_INFO     -> 必须返回 ble_tips_ref_time_info_t*
 *   SET_CURRENT_TIME      -> 手机写进来了，返回值没人看
 *   SET_LOCAL_TIME_INFO   -> 手机调时区，返回值没人看 */
static uint8_t *ble_tips_cb(uint8_t conn_idx, uint8_t event, uint8_t *value)
{
    (void)conn_idx;

    switch (event)
    {
    case BLE_TIPS_GET_CURRENT_TIME:
        rtc_to_ct(&g_ct);
        rt_kprintf("ble_time: phone read current time -> %04d-%02d-%02d %02d:%02d:%02d\n",
                   g_ct.date_time.year, g_ct.date_time.month, g_ct.date_time.day,
                   g_ct.date_time.hour, g_ct.date_time.min, g_ct.date_time.sec);
        return (uint8_t *)&g_ct;

    case BLE_TIPS_GET_LOCAL_TIME_INFO:
        return (uint8_t *)&g_lti;

    case BLE_TIPS_GET_REF_TIME_INFO:
        return (uint8_t *)&g_rti;

    case BLE_TIPS_SET_CURRENT_TIME:
        if (value != RT_NULL)
        {
            ble_apply_time((const ble_tips_cur_time_t *)value);
        }
        return RT_NULL;

    case BLE_TIPS_SET_LOCAL_TIME_INFO:
        if (value != RT_NULL)
        {
            g_lti = *(ble_tips_local_time_info_t *)value;
            rt_kprintf("ble_time: phone set local time info tz=%d dst=%d\n",
                       g_lti.time_zone, g_lti.dst_offset);
        }
        return RT_NULL;

    default:
        return RT_NULL;
    }
}

/* ══════════════════════════════════════════════════════════════════════
 *  广播
 * ══════════════════════════════════════════════════════════════════════ */

/* 这个宏做三件事：定义 context、定义一个转发函数、把转发函数注册进 BLE 事件链。
 * 必须放在文件作用域。 */
SIBLES_ADVERTISING_CONTEXT_DECLAR(g_adv_ctx);

static uint8_t ble_adv_evt_handler(uint8_t event, void *context, void *data)
{
    (void)context;

    switch (event)
    {
    case SIBLES_ADV_EVT_ADV_STARTED:
    {
        sibles_adv_evt_startted_t *e = (sibles_adv_evt_startted_t *)data;

        g_ble.adv_on = (e->status == 0) ? 1 : 0;
        /* ★ 广播真的起来了，预热阶段才算结束。
         * 见 ble_app_wants_awake() 里那段"为什么不能提前清"的说明。 */
        g_ble.warming_up = 0;
        rt_kprintf("ble_adv: started status=%d mode=%d  name=\"%s\"\n",
                   e->status, e->adv_mode, BLE_DEV_NAME);
        break;
    }

    case SIBLES_ADV_EVT_ADV_STOPPED:
    {
        sibles_adv_evt_stopped_t *e = (sibles_adv_evt_stopped_t *)data;

        g_ble.adv_on = 0;
        rt_kprintf("ble_adv: stopped reason=%d mode=%d\n", e->reason, e->adv_mode);
        break;
    }

    default:
        break;
    }

    return 0;
}

static void ble_adv_start(void)
{
    sibles_advertising_para_t para;
    uint8_t ret;

    if (g_ble.ready == 0)
    {
        rt_kprintf("ble_adv: stack not ready yet, defer\n");
        g_ble.want_on = 1;
        return;
    }

    if (g_ble.adv_on)
    {
        rt_kprintf("ble_adv: already advertising\n");
        g_ble.warming_up = 0;
        return;
    }

    if (g_ble.adv_inited == 0)
    {
        char local_name[] = BLE_DEV_NAME;
        bd_addr_t addr;

        rt_memset(&para, 0, sizeof(para));

        /* 把自己的名字设进 GAP，手机连上后读 0x2A00 能看到同一个名字 */
        if (ble_get_public_address(&addr) == HL_ERR_NO_ERROR)
        {
            rt_kprintf("ble_adv: public addr %02x-%02x-%02x-%02x-%02x-%02x\n",
                       addr.addr[5], addr.addr[4], addr.addr[3],
                       addr.addr[2], addr.addr[1], addr.addr[0]);
        }

        {
            ble_gap_dev_name_t *dn = rt_malloc(sizeof(ble_gap_dev_name_t) + rt_strlen(local_name));

            if (dn != RT_NULL)
            {
                dn->len = rt_strlen(local_name);
                rt_memcpy(dn->name, local_name, dn->len);
                ble_gap_set_dev_name(dn);
                rt_free(dn);
            }
        }

        para.own_addr_type = GAPM_STATIC_ADDR;
        para.config.adv_mode = SIBLES_ADV_CONNECT_MODE;
        /* duration=0 -> 一直广播，直到主动停或者被连上 */
        para.config.mode_config.conn_config.duration = 0;
        /* interval 单位 625us，0x30 = 48 * 0.625ms = 30ms */
        para.config.mode_config.conn_config.interval = 0x30;
        para.config.max_tx_pwr = 0x7F;
        /* 断开后自动重新广播 —— 用户关掉手机蓝牙再开，不用重开手表 */
        para.config.is_auto_restart = 1;
        /* scan response 与 adv 相同，省一次组包 */
        para.config.is_rsp_data_duplicate = 1;

        /* 设备名。31 字节的广播包里塞得下，直接放 adv。 */
        para.adv_data.completed_name =
            rt_malloc(rt_strlen(local_name) + sizeof(sibles_adv_type_name_t));
        if (para.adv_data.completed_name != RT_NULL)
        {
            para.adv_data.completed_name->name_len = rt_strlen(local_name);
            rt_memcpy(para.adv_data.completed_name->name, local_name,
                      para.adv_data.completed_name->name_len);
        }

        /* ★ 关键一步：把 CTS 的服务 UUID(0x1805) 放进广播数据。
         *
         * 为什么非放不可：网页端用 Web Bluetooth 的
         *     requestDevice({filters:[{services:['current_time']}]})
         * 是按**广播包里出现的服务 UUID** 筛设备的。不放的话浏览器
         * 根本不会把这块表列出来，只能退化成 acceptAllDevices（一堆设备里瞎找）。
         * 16 位 UUID 在线路上是小端，所以先发 0x05 再发 0x18。 */
        {
            sibles_adv_type_srv_uuid_t *u =
                rt_malloc(sizeof(sibles_adv_type_srv_uuid_t) + sizeof(sibles_adv_uuid_t));

            if (u != RT_NULL)
            {
                u->count = 1;
                u->uuid_list[0].uuid_len = ATT_UUID_16_LEN;
                u->uuid_list[0].uuid.uuid_16[0] = 0x05;
                u->uuid_list[0].uuid.uuid_16[1] = 0x18;
                para.adv_data.completed_uuid = u;
            }
            else
            {
                rt_kprintf("ble_adv: WARN no mem for service uuid, "
                           "browser filter by service will NOT find the watch\n");
            }
        }

        para.evt_handler = ble_adv_evt_handler;

        ret = sibles_advertising_init(g_adv_ctx, &para);

        rt_free(para.adv_data.completed_name);
        rt_free(para.adv_data.completed_uuid);

        if (ret != SIBLES_ADV_NO_ERR)
        {
            rt_kprintf("ble_adv: init failed err=%d\n", ret);
            return;
        }

        g_ble.adv_inited = 1;
        rt_kprintf("ble_adv: advertising context ready\n");
    }

    ret = sibles_advertising_start(g_adv_ctx);
    if (ret != SIBLES_ADV_NO_ERR)
    {
        rt_kprintf("ble_adv: start failed err=%d\n", ret);
        return;
    }

    /* 到点自己关，别让用户忘了之后一直耗电。
     * 走 ble_auto_off_arm 而不是直接 rt_timer_control —— 那里做了 ms->tick 换算，
     * 手写一遍很容易再踩"少乘 1000"的坑。 */
    ble_auto_off_arm(BLE_AUTO_OFF_MS);

    rt_kprintf("ble_adv: start requested (auto off in %d s)\n",
               (int)(BLE_AUTO_OFF_MS / 1000));
}

static void ble_adv_stop(void)
{
    if (g_ble.auto_off != RT_NULL)
    {
        rt_timer_stop(g_ble.auto_off);
    }

    if (g_ble.adv_inited == 0 || g_ble.adv_on == 0)
    {
        rt_kprintf("ble_adv: not advertising, nothing to stop\n");
        return;
    }

    rt_kprintf("ble_adv: stopping\n");
    sibles_advertising_stop(g_adv_ctx);
}

/* 自动关的定时器。回调跑在系统定时器线程里，所以这里只投邮箱。 */
static void ble_auto_off_cb(void *parameter)
{
    (void)parameter;
    rt_kprintf("ble_adv: auto off timer fired\n");
    rt_mb_send(&g_mb, BLE_MSG_RADIO_OFF);
}

/* ══════════════════════════════════════════════════════════════════════
 *  工作线程：真正干活的地方
 * ══════════════════════════════════════════════════════════════════════ */

static void ble_on_power_on(void)
{
    ble_tips_time_env_t env;

    if (g_ble.tips_ready)
    {
        return;
    }

    rt_memset(&env, 0, sizeof(env));

    /* 初始值。手机在读之前，这些东西就是我们要"声称"的状态。
     *   time_zone  : 单位 15 分钟，+32 = UTC+8
     *   dst_offset : 0 = 标准时（不用夏令时）
     *   time_source: 4 = Manual（手工设的）
     *   time_accuracy / days / hours: 255 = 未知 —— 我们确实不知道
     *                RTC 相对真实时间差多少，写"未知"比编一个数字诚实。 */
    env.local_time_inf.time_zone  = (uint8_t)BLE_TZ_15MIN;
    env.local_time_inf.dst_offset = 0;
    env.ref_time_inf.time_source   = 4;
    env.ref_time_inf.time_accuracy = 255;
    env.ref_time_inf.days_update   = 255;
    env.ref_time_inf.hours_update  = 255;

    /* ★ 别忘了 g_lti / g_rti 也要填。
     * env 只是交给 SDK 的初始副本；手机**读** 0x2A0F / 0x2A14 时，走的是
     * ble_tips_get_cbk -> 我们的回调 -> 返回值，也就是 g_lti / g_rti。
     * 不填的话这两个是零初始化的全 0：时区会被读成 0（UTC+0），
     * 手机侧可能据此把时间在 UTC/本地之间来回换算，表现成"校完差 8 小时"。 */
    g_lti.time_zone  = (uint8_t)BLE_TZ_15MIN;
    g_lti.dst_offset = 0;
    g_rti.time_source   = 4;      /* Manual */
    g_rti.time_accuracy = 255;    /* 未知 */
    g_rti.days_update   = 255;
    g_rti.hours_update  = 255;

    rt_kprintf("ble: BLE ready -> register Current Time Service (0x1805)\n");
    ble_tips_init(ble_tips_cb, env);
    g_ble.tips_ready = 1;

    /* ★ AI 助手的蓝牙数据通道。
     *
     * 为什么必须在这里调（而不是在事件回调里）：
     *   它会注册一个 128-bit GATT 服务，**这一步会阻塞协议栈**。
     *   本函数跑在 ble_thread_entry 这个工作线程上，正合适。
     *
     * 为什么必须调（不调会怎样）：
     *   只打开 CONFIG_BSP_BLE_SERIAL_TRANSMISSION 是不够的 ——
     *   服务的 .o 确实编出来了（2332 -> 55924 字节），
     *   但没人引用它，链接器 --gc-sections 会把整段服务当垃圾回收掉，
     *   main.bin 只涨 1664 字节，等于白开。
     *   调了之后 main.bin 应该有**几 KB 以上**的增长，那才是硬证据。 */
    ble_ai_serial_init();

    /* ★ 没校过时的话，开机就自动广播 —— 不让用户去找那个 BT 按钮。
     *
     * 场景就是这块表的真实毛病：RTC 没有备份电池，**一拔电就回到 2024-01-01**。
     * 这时候用户唯一要做的事是"拿手机点一下校时页"，不该再逼他先摸表盘、
     * 再在状态栏里找个 BT 小按钮。
     *
     * 判断依据：年份 ≤ 2024 就当没校过（真机跑的是 2026 年，
     * 而开机兜底值写的是 2024-01-01 12:00）。
     * 校过一次之后年份变成当年，下次开机就不会再自动广播了。 */
    {
        time_t     now = time(RT_NULL);
        struct tm *t   = localtime(&now);

        if (t != RT_NULL && (t->tm_year + 1900) <= 2024)
        {
            rt_kprintf("ble: RTC looks unset (%04d-%02d-%02d) -> auto start advertising\n",
                       t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
            g_ble.want_on = 1;
        }
    }

    if (g_ble.want_on)
    {
        ble_adv_start();
    }
}

static void ble_thread_entry(void *parameter)
{
    rt_uint32_t msg;

    (void)parameter;

    /* ★ 先睡一会儿，把表盘让出来。
     *
     * sifli_ble_enable() 内部要做 LCPU 上电 + 射频校准，整段是阻塞的，
     * 而且会跟 LCD 抢时钟。所以这里刻意延后一点 —— 表盘先亮，蓝牙随后在后台起来。
     *
     * 用 mdelay 而不是抢优先级：我们只是想让出时间片，不想跟 lcd_task 争。
     * （15000 这个数是按"真实毫秒"算的，不是日志里那个虚数。） */
    rt_thread_mdelay(BLE_STACK_START_DELAY_MS);

    rt_kprintf("ble: enabling stack in worker thread (UI stays live)\n");
    sifli_ble_enable();

    for (;;)
    {
        /* 带超时的收消息：超时不是错误，是拿来打心跳的（见 BLE_HEARTBEAT_MS）。 */
        if (rt_mb_recv(&g_mb, &msg, rt_tick_from_millisecond(BLE_HEARTBEAT_MS))
                == -RT_ETIMEOUT)
        {
            /* 心跳只在广播开着的时候打，免得闲着也刷屏。
             * 一行里故意带上 tick 和 RTC 两个时间：
             *   tick —— 系统节拍，用来核对"日志时间戳到底准不准"；
             *   rtc  —— 真实时间，用来核对板子还在不在走。
             * 这两列一比，就能看出板子是醒着还是睡死了。 */
            if (g_ble.adv_on)
            {
                time_t     now = time(RT_NULL);
                struct tm *t   = localtime(&now);

                rt_kprintf("ble: [hb] adv_on=1 connected=%d sync=%u tick=%u "
                           "rtc=%04d-%02d-%02d %02d:%02d:%02d\n",
                           g_ble.connected, (unsigned)g_ble.sync_count,
                           (unsigned)rt_tick_get(),
                           t ? t->tm_year + 1900 : 0, t ? t->tm_mon + 1 : 0,
                           t ? t->tm_mday : 0, t ? t->tm_hour : 0,
                           t ? t->tm_min : 0, t ? t->tm_sec : 0);
            }

            /* 预热阶段也要有一行，不然"协议栈还没起来"同样是无输出。 */
            if (g_ble.warming_up && g_ble.ready == 0)
            {
                rt_kprintf("ble: [hb] still starting, tick=%u\n",
                           (unsigned)rt_tick_get());
            }

            continue;
        }

        switch (msg)
        {
        case BLE_MSG_POWER_ON:
            ble_on_power_on();
            break;

        case BLE_MSG_RADIO_ON:
            ble_adv_start();
            break;

        case BLE_MSG_RADIO_OFF:
            ble_adv_stop();
            g_ble.want_on = 0;
            /* 用户/定时器主动关了，预热阶段也就不算数了，让屏幕能正常息。 */
            g_ble.warming_up = 0;
            break;

        default:
            break;
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════
 *  协议栈事件。**只投邮箱，不做任何可能阻塞的事。**
 * ══════════════════════════════════════════════════════════════════════ */
static int app_ble_event_handler(uint16_t event_id, uint8_t *data, uint16_t len, uint32_t context)
{
    (void)len;
    (void)context;

    switch (event_id)
    {
    case BLE_POWER_ON_IND:
        rt_kprintf("ble: BLE_POWER_ON_IND (stack is up)\n");
        g_ble.ready = 1;
        /* ⚠ 这里**不要**清 warming_up。
         * 协议栈就绪 ≠ 广播已发出；中间还有"注册 CTS → 起广播 → 收到
         * ADV_STARTED 事件"三步（走邮箱、跨线程）。在这段空档里如果
         * warming_up 已经是 0，息屏判断就会当场成立、把屏幕睡掉，
         * 顺带把刚起来的射频一起带走 —— 症状就是"广播只活了 0.3 秒"。
         * 清它的地方只有一个：SIBLES_ADV_EVT_ADV_STARTED。 */
        g_ble.tips_ready = 0;
        rt_mb_send(&g_mb, BLE_MSG_POWER_ON);
        break;

    case BLE_GAP_CONNECTED_IND:
    {
        ble_gap_connect_ind_t *ind = (ble_gap_connect_ind_t *)data;

        g_ble.conn_idx  = ind->conn_idx;
        g_ble.connected = 1;
        rt_kprintf("ble: connected, conn_idx=%d peer=%02x-%02x-%02x-%02x-%02x-%02x\n",
                   ind->conn_idx,
                   ind->peer_addr.addr[5], ind->peer_addr.addr[4],
                   ind->peer_addr.addr[3], ind->peer_addr.addr[2],
                   ind->peer_addr.addr[1], ind->peer_addr.addr[0]);
        break;
    }

    case BLE_GAP_DISCONNECTED_IND:
    {
        ble_gap_disconnected_ind_t *ind = (ble_gap_disconnected_ind_t *)data;

        g_ble.connected = 0;
        rt_kprintf("ble: disconnected reason=%d\n", ind->reason);
        break;
    }

    default:
        break;
    }

    return 0;
}
BLE_EVENT_REGISTER(app_ble_event_handler, NULL);

/* ══════════════════════════════════════════════════════════════════════
 *  对外接口
 * ══════════════════════════════════════════════════════════════════════ */

void ble_app_init(void)
{
    if (g_ble.inited)
    {
        return;
    }

    /* 先清零再置 inited —— 反过来的话 memset 会把刚置的那个 1 抹掉，
     * 变成"重复调用会重复建线程"。 */
    rt_memset(&g_ble, 0, sizeof(g_ble));
    g_ble.inited = 1;
    /* 蓝牙还在路上。这个标志让息屏判断先按住屏幕，别表盘刚亮就睡过去 ——
     * 后面还要自动广播等用户来对时（见 ble_app_wants_awake）。 */
    g_ble.warming_up = 1;
    g_ble.warm_start = rt_tick_get();

    /* ★ 开机就先判一次 RTC，早于协议栈。
     *
     * 为什么不能等到 ble_on_power_on() 再判（那里也判了）：息屏判断用的是
     * 屏幕空闲时间，开机十几秒就够超阈值了。要是等到协议栈起来才知道
     * "哦，这表时间是假的，得广播"，中间那段空档屏幕就会被睡掉 ——
     * 这正是"刚开广播就息屏"的另一半原因。
     * 在这里就把意图（want_on）立起来，后面那一整段就都是有主的。 */
    {
        time_t     now = time(RT_NULL);
        struct tm *t   = localtime(&now);

        if (t != RT_NULL && (t->tm_year + 1900) <= 2024)
        {
            g_ble.want_on = 1;
            rt_kprintf("ble: RTC looks unset at boot (%04d-%02d-%02d) -> "
                       "will advertise once the stack is up\n",
                       t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
        }
    }

    if (rt_mb_init(&g_mb, "blemb", g_mb_pool,
                   sizeof(g_mb_pool) / sizeof(g_mb_pool[0]), RT_IPC_FLAG_FIFO) != RT_EOK)
    {
        rt_kprintf("ble: mailbox init failed, BLE disabled\n");
        return;
    }

    /* 栈给 6K：这个线程现在还要扛 sifli_ble_enable()（里面会建 mbox 线程、
     * 拷 NVDS、准备补丁），4K 太紧。 */
    g_thread = rt_thread_create("bleapp", ble_thread_entry, RT_NULL,
                               6144, RT_THREAD_PRIORITY_MIDDLE, 10);
    if (g_thread == RT_NULL)
    {
        rt_kprintf("ble: thread create failed, BLE disabled\n");
        return;
    }
    rt_thread_startup(g_thread);

    /* 自动关的定时器。软定时器，回调跑在系统定时器线程。 */
    g_ble.auto_off = rt_timer_create("bleoff", ble_auto_off_cb, RT_NULL,
                                     rt_tick_from_millisecond(BLE_AUTO_OFF_MS),
                                     RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);

    /* 注意：这里**不再**直接调 sifli_ble_enable() —— 它阻塞 36 秒，
     * 会把开机卡成黑屏。改由上面那个工作线程去调（见 ble_thread_entry）。 */
}

void ble_app_radio(int on)
{
    if (g_ble.inited == 0)
    {
        return;
    }

    g_ble.want_on = on ? 1 : 0;
    rt_mb_send(&g_mb, on ? BLE_MSG_RADIO_ON : BLE_MSG_RADIO_OFF);
}

void ble_app_toggle(void)
{
    /* 以"用户看到的实际状态"为准来切，而不是 want_on ——
     * 协议栈还没起来时 want_on 和实际不一致，按实际切才符合直觉。 */
    ble_app_radio(g_ble.adv_on ? 0 : 1);
}

rt_bool_t ble_app_radio_on(void)
{
    return g_ble.adv_on ? RT_TRUE : RT_FALSE;
}

rt_bool_t ble_app_connected(void)
{
    return g_ble.connected ? RT_TRUE : RT_FALSE;
}

rt_bool_t ble_app_ready(void)
{
    return g_ble.ready ? RT_TRUE : RT_FALSE;
}

rt_uint32_t ble_app_sync_count(void)
{
    return g_ble.sync_count;
}

rt_bool_t ble_app_wants_awake(void)
{
    /* 只要"想开蓝牙"就一直按住屏幕：息屏 = gui_suspend + PM 低功耗，
     * 射频跟着断，表现成"刚开广播就没了 / 刚连上就断"。
     *
     * ★★ 2026-09-21 实测到的坑（"一直连接失败"的真正原因）★★
     *
     * 原来这里只看 warming_up || adv_on || connected，而 warming_up 是
     * 收到 BLE_POWER_ON_IND 就清掉的。这两件事之间有一个空档：
     *
     *    ...POWER_ON_IND 到 -> warming_up=0        <-- 就在这一瞬间
     *    ...协议栈就绪、注册 CTS、起广播            <-- adv_on 才变 1
     *        （这几步要投邮箱、等 bleapp 线程调度，不是同一个 tick 里做完的）
     *
     * 空档里 wants_awake() 返回 FALSE，而屏幕的空闲计时早就超过 10 秒了
     * （开机至今没人碰过屏幕），于是息屏判断当场成立、状态机翻到 SLEEP。
     * 实测原始日志（串口那个 [编号] 见文件末尾「关于日志时间戳」）：
     *
     *   [565383]  Sibles ADV start 0!            <- 广播刚发出去
     *   [565409]  fsm: fsm:Cur[GUI_STATE_ACTIVE],Action[GUI_PM_ACTION_SLEEP]
     *   [565572]  change_gui_state: ACTIVE -> INACTIVE_PENDING -> INACTIVE
     *   [565610]  gui_suspend / lcd Power off     <- 广播只活了 0.3 秒（真实约 7 ms）
     *
     * 结果就是：这块表"看起来在广播"，实际上刚开就睡死了，手机永远搜不到、
     * 连不上。串口随后也全哑（低功耗把 uart1 时钟关掉），查起来像死机。
     *
     * 所以：want_on 也必须算进来 —— 从"打算开"到"真的开着"整段都别睡。
     * warming_up 也改成广播真起来了才清（见 SIBLES_ADV_EVT_ADV_STARTED）。 */
    if (g_ble.want_on || g_ble.adv_on || g_ble.connected)
    {
        return RT_TRUE;
    }

    /* 预热阶段也按住，但加个兜底：协议栈要是死活起不来，
     * 到点就放手，别让表盘一直亮着耗电。 */
    if (g_ble.warming_up)
    {
        rt_uint32_t elapsed = rt_tick_get() - g_ble.warm_start;

        if (elapsed < (rt_uint32_t)rt_tick_from_millisecond(BLE_WARMUP_MAX_MS))
        {
            return RT_TRUE;
        }

        g_ble.warming_up = 0;
        rt_kprintf("ble: warmup timed out after %d s, give up (let the screen sleep)\n",
                   (int)(BLE_WARMUP_MAX_MS / 1000));
    }

    return RT_FALSE;
}

const char *ble_app_state_str(void)
{
    if (g_ble.ready == 0)
    {
        return g_ble.inited ? "stack starting" : "off";
    }
    if (g_ble.connected)
    {
        return g_ble.sync_count ? "connected, synced" : "connected";
    }
    return g_ble.adv_on ? "advertising" : "ready (radio off)";
}

/* ══════════════════════════════════════════════════════════════════════
 *  关于日志时间戳：串口里那个 [123456] 不是毫秒，比真实时间快 32.77 倍
 * ══════════════════════════════════════════════════════════════════════
 *
 * 这条必须写下来，因为它已经骗过我们两次，而且骗法很隐蔽。
 *
 * 现象：串口每一行前面有 [565383] 这样的数。看上去像"开机后 565383 毫秒"，
 * 于是读日志的人会得到一堆可怕的结论 —— "蓝牙启动花了 9 分钟"、
 * "开机黑屏 50 秒"、"广播要等 70 秒才能连"。**全是假的。**
 *
 * 怎么发现的（2026-09-21）：抓了一段日志，同时用电脑记下起止墙钟时间：
 *     墙钟 30.05 秒        <- PowerShell 自己打的
 *     日志编号从 0 走到 566126
 * 566126 / 30.05 s ≈ 18800/s，而"毫秒"应当是 1000/s。再拿两个独立的数一对：
 *     LVGL 的空闲计时（真按毫秒走）：16859 ms
 *     同一时刻的日志编号            ：565211
 *     565211 / 16859 = 33.5 ≈ 32768 / 1000 = 32.768   ★ 对上了
 * 也就是说：那个编号在**数 32.768 kHz 的 LXT 拍子，却按毫秒打印**，
 * 全部数值虚高 32.768 倍。
 *
 * 折算方法（记熟了能省很多误判）：
 *     真实秒数 = 日志编号 / 32768
 *     日志编号 = 真实秒数 * 32768
 *
 * 按这个折算，真实的开机时序是：
 *     屏幕亮          [13429]  ->  0.41 s
 *     蓝牙协议栈就绪  [564935] ->  17.2 s
 *     广播发出        [565643] ->  17.3 s     ← 不是 70 秒，也不是 9 分钟
 *
 * 注意区分：**RT-Thread 的 rt_tick 本身是准的**（10 秒息屏就是 10 秒，
 * 5 分钟自动关广播就是 5 分钟）。虚的只是日志里打印出来的那个编号。
 * 想知道 rt_tick 的真实值，看心跳行里的 tick= 字段（那个是真 tick）。 */

#else /* !BSP_BLE_TIMES —— 没开蓝牙时的空实现，保证其它文件照常编译 */
void ble_app_init(void) {}
void ble_app_radio(int on)
{
    (void)on;
}
void ble_app_toggle(void) {}
rt_bool_t ble_app_radio_on(void)
{
    return RT_FALSE;
}
rt_bool_t ble_app_connected(void)
{
    return RT_FALSE;
}
rt_bool_t ble_app_ready(void)
{
    return RT_FALSE;
}
rt_uint32_t ble_app_sync_count(void)
{
    return 0;
}
rt_bool_t ble_app_wants_awake(void)
{
    return RT_FALSE;
}
const char *ble_app_state_str(void)
{
    return "disabled";
}

#endif /* BSP_BLE_TIMES */
