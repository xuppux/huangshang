/**
 * @file    bat_mon.c
 * @brief   电池监测实现 —— 电压走 ADC，充电状态走 PMU 状态寄存器
 */
#include "bat_mon.h"

#include <rtdevice.h>
#include <string.h>

#include "bf0_hal.h"
#include "bf0_hal_pmu.h"

/* ── 数据来源 ──────────────────────────────────────────────────────────────
 * "bat1" 这个名字是驱动写死的：drv_adc.c 的 sifli_adc_init() 里，
 * 寄存器实例是 gpadc1 的话就把设备注册成 bat1（见 drv_adc.c:845~865）。
 * 通道 7 也是写死的：SF32LB52X 上只有这一路内部接在 VBAT 上，
 * 芯片里已经带好 1/2 分压，外面不要自己再加分压电阻。 */
#define BAT_ADC_DEV     "bat1"
#define BAT_ADC_CH      7

/* 读数的单位是 0.1mV，不是 mV。
 *
 * 两个独立证据：
 *   1. drv_adc.c 的 sifli_get_adc_value()：*value = sifli_adc_get_float_mv(...) * 10
 *   2. SDK 自带的 battery_table.c 里 100% 对应 41808 —— 41808mV = 41.8V 显然不可能，
 *      按 0.1mV 读就是 4180.8mV，正好是一节满电锂电。
 * 串口里会看到驱动自己打的那行 "ch[7]voltage=40218;"，那个 40218 就是 0.1mV，
 * 别误以为是 40V。 */
#define BAT_RAW_PER_MV  10

/* 低于这个电压就认为「没接电池 / 量到的不是电池」，界面显示 --% 而不是乱报一个 0%。 */
#define BAT_MIN_MV      2500

/* ── 怎么判断「根本没接电池」────────────────────────────────────────────
 * 这是最容易骗过人的一处：**没接电池时 CH8 的读数不是 0，而是 ~4.3 V。**
 *
 * 证据（立创官方 ADC 例程自带的两张截图，用的就是这个 ch[7]）：
 *   接入电池前：43481 / 42953 / 43533 / 43165        -> 4.30 ~ 4.35 V
 *   接入电池后：35539 / 35556 / 37310                -> 3.55 ~ 3.73 V
 * 悬空的 VBAT 引脚被充电器漏电流顶到了 4.3 V 左右，比满电锂电池（4.2 V）还高。
 * 只按电压卡是卡不住的 —— 悬空值在 4.26~4.37 V 之间漂，任何一种阈值都会
 * 「一会儿说有电池、一会儿说没有」，界面上就是 100% 和 --% 来回跳。
 *
 * 真正的差别在**噪声**：悬空是高阻节点，读数抖得厉害（实测 35 / 36 / 61 / 65 / 105 mV）；
 * 接了真电池是低阻节点，两次读数的差只有 1~2 mV（立创例程里 35539 -> 35556）。
 * 所以判据用「同一次里连读几遍的极差」，而不是绝对电压 —— 这个指标中间空着十几倍。
 *
 * 注意驱动每一遍读本身已经做过 22 次采样、去掉最大最小再平均（drv_adc.c:659），
 * 所以拿到的极差是**真的在抖**，不是采样噪声。 */
#define BAT_MAX_MV      4270    /* 满电锂电池最多 4.2 V，再高不可能是电池 */
#define BAT_SPREAD_MV   15      /* 连读极差超过这么多 -> 悬空（真电池 ~2 mV） */
#define BAT_GOOD_RUN    2       /* 从「读不到」恢复要连续几次干净读数 */

/* 一次读几遍。ADC 驱动内部本身已经做了 22 次采样取平均，
 * 这里再多读几遍是为了拿到「极差」这个判据，不是为了更准。 */
#define BAT_SAMPLE_N    3

typedef struct
{
    rt_uint16_t mv;
    rt_uint8_t  pct;
} bat_point_t;

/* ── 电压 → 电量 曲线 ──────────────────────────────────────────────────────
 * 数据不是编的：从 SDK 自带的 middleware/battery/battery_table.c 里每 5% 取一个点，
 * 单位换成 mV（原表是 0.1mV）。
 *
 * 为什么分「放电 / 充电」两条：充电时电池有极化电压，同样读到 4.00V，
 * 放电状态下是 80%，充电状态下其实只有 60% 出头。用一条曲线在充电时会虚高。
 *
 * ⚠ 这是「通用锂电」曲线，不是这块电池的实测曲线。想看更准的数字，
 *   找电池厂家要一份 电压-电量 曲线，把下面两张表换掉即可，代码不用动。
 */
static const bat_point_t curve_dis[] =
{
    {4181, 100}, {4065, 95}, {4032, 90}, {4020, 85}, {4013, 80}, {4009, 75},
    {3996, 70},  {3978, 65}, {3961, 60}, {3939, 55}, {3900, 50}, {3875, 45},
    {3861, 40},  {3852, 35}, {3839, 30}, {3820, 25}, {3796, 20}, {3761, 15},
    {3719, 10},  {3681, 5},  {3504, 0},
};

static const bat_point_t curve_chg[] =
{
    {4200, 100}, {4111, 95}, {4078, 90}, {4066, 85}, {4059, 80}, {4055, 75},
    {4042, 70},  {4024, 65}, {4007, 60}, {3985, 55}, {3947, 50}, {3921, 45},
    {3908, 40},  {3898, 35}, {3885, 30}, {3866, 25}, {3843, 20}, {3807, 15},
    {3757, 10},  {3728, 5},  {3558, 0},
};

/* 表里的电压是从高到低排的，所以找「第一个比 mv 低的点」，再和它前一个点线性插值。 */
static int pct_from_curve(const bat_point_t *t, int n, int mv)
{
    int i;

    if (mv >= t[0].mv)
    {
        return 100;
    }

    for (i = 0; i < n; i++)
    {
        if (mv > t[i].mv)
        {
            break;
        }
    }

    if (i >= n)                 /* 比最低点还低 */
    {
        return 0;
    }

    return t[i].pct + (t[i - 1].pct - t[i].pct) * (mv - t[i].mv)
           / (t[i - 1].mv - t[i].mv);
}


static bat_info_t s_info;
static int        s_filt_mv   = -1;      /* 低通滤波后的电压，mV；-1 = 还没有初值 */
static rt_uint8_t s_no_dev_log = 0;      /* 「找不到 bat1」只吼一次，别刷屏 */
static rt_uint8_t s_bad_state = 0;       /* 1 = 当前判为「读不到电池」 */
static rt_uint8_t s_good_run  = 0;       /* 连续几次读数干净（用于迟滞） */

static void bat_mon_read_hw(bat_info_t *o)
{
    /* 充电状态全部来自 PMU 的**只读**状态寄存器。
     *
     * 为什么不用 SDK 那套 rt_charge_* 接口（customer/peripherals/charger/）：
     * 那套要开 BSP_USING_CHARGER，会跟着初始化充电器、注册 PMUC 中断、
     * 还牵动关机/休眠的唤醒源判断。我们只是想知道「插没插线」，没必要动那些。
     * HAL_PMU_ChgReadStatus() / HAL_PMU_ChgGetHwState() 内部压根没用到 handle
     * （见 bf0_hal_pmu.c:1125、1191，就是读 hwp_pmuc->CHG_SR 一个寄存器），
     * 所以传 NULL 也安全。 */
    o->vbus      = HAL_PMU_ChgReadStatus(RT_NULL, PMU_CHG_IRQ_VBUS_RDY) ? 1 : 0;
    o->eoc       = HAL_PMU_ChgReadStatus(RT_NULL, PMU_CHG_IRQ_EOC) ? 1 : 0;
    o->hw_state  = (rt_uint8_t)HAL_PMU_ChgGetHwState(RT_NULL);
    o->chg_sr    = hwp_pmuc->CHG_SR;
    o->chg_cr1   = hwp_pmuc->CHG_CR1;
}

static int bat_mon_read_mv(int *spread_out)
{
    rt_device_t          dev;
    rt_uint32_t          raw;
    rt_err_t             r;
    int                  i;
    int                  lo = 0x7FFFFFFF;
    int                  hi = -0x7FFFFFFF;
    int                  sum = 0;

    dev = rt_device_find(BAT_ADC_DEV);
    if (dev == RT_NULL)
    {
        if (s_no_dev_log == 0)
        {
            s_no_dev_log = 1;
            rt_kprintf("bat: device \"%s\" not found -> "
                       "proj.conf needs CONFIG_BSP_USING_ADC1=y\n", BAT_ADC_DEV);
        }
        return -1;
    }

    r = rt_adc_enable((rt_adc_device_t)dev, BAT_ADC_CH);
    if (r != RT_EOK)
    {
        rt_kprintf("bat: adc enable ch%d failed (%d)\n", BAT_ADC_CH, (int)r);
        return -1;
    }

    for (i = 0; i < BAT_SAMPLE_N; i++)
    {
        int mv;

        raw = rt_adc_read((rt_adc_device_t)dev, BAT_ADC_CH);
        mv  = (int)(raw / BAT_RAW_PER_MV);

        if (mv < lo) { lo = mv; }
        if (mv > hi) { hi = mv; }
        sum += mv;
    }

    rt_adc_disable((rt_adc_device_t)dev, BAT_ADC_CH);

    if (spread_out)
    {
        *spread_out = hi - lo;
    }

    /* 报中间值而不是平均值：平均值会被某一遍的野值带偏，
     * 而我们要判的恰恰是「有没有野值」。 */
    return (lo + hi) / 2;
}


const bat_info_t *bat_mon_get(void)
{
    int mv;
    int spread = 0;
    int bad;

    bat_mon_read_hw(&s_info);

    mv = bat_mon_read_mv(&spread);

    s_info.spread_mv = spread;

    if (mv < 0)
    {
        s_info.voltage_mv = -1;
        s_info.percent    = -1;
        s_info.state      = BAT_ST_UNKNOWN;
        return &s_info;
    }

    /* 「悬空 / 没电池」判定，三条任意中一条就成立：
     *   1. 连读极差大  -> 高阻节点，不是电池
     *   2. 电压低得不可能 -> 电池不在，或者接线不对
     *   3. 电压高得不可能 -> 悬空被漏电流顶起来了
     * 判成「读不到」时界面画灰色的 --%，绝不画一个假的 100%。
     *
     * 用的是原始 mv 而不是滤过的值：滤过的值会被之前的悬空读数污染，
     * 判据必须直接看本次实测。
     */
    bad = (spread > BAT_SPREAD_MV) || (mv < BAT_MIN_MV) || (mv > BAT_MAX_MV);

    /* 迟滞，防止界面在「有/没有电池」之间来回跳：
     *   转成「读不到」——立刻生效（读数异常就该马上不再显示数字）；
     *   转回「有电池」——要求连续 BAT_GOOD_RUN 次都干净才认。
     * 这个不对称是有意的：显示一个假数字，比短暂显示 --% 更糟。 */
    if (bad)
    {
        s_bad_state = 1;
        s_good_run  = 0;
    }
    else if (s_bad_state != 0)
    {
        if (++s_good_run >= BAT_GOOD_RUN)
        {
            s_bad_state = 0;
        }
    }

    if (s_bad_state != 0)
    {
        /* 顺手把滤波器清掉。不清的话，之前 4.3 V 的悬空读数会留在滤波器里，
         * 等真电池接上来还要花十几次采样（100 多秒）才收敛到真实电压。 */
        s_filt_mv         = -1;
        s_info.voltage_mv = mv;     /* 留着真实读数，串口排查时才看得出到底多少伏 */
        s_info.percent    = -1;
        s_info.state      = BAT_ST_UNKNOWN;
        return &s_info;
    }

    /* 一阶低通：新值只占 1/4。
     * 不加这个的话，屏幕上的数字会跟着负载一跳一跳（亮屏瞬间能掉好几个百分点）。
     * 电池电压本来就是慢变量，10 秒一次、连续 4 次才追上真实值，够用。 */
    if (s_filt_mv < 0)
    {
        s_filt_mv = mv;
    }
    else
    {
        s_filt_mv = (s_filt_mv * 3 + mv) / 4;
    }

    s_info.voltage_mv = s_filt_mv;

    if (s_info.vbus == 0)
    {
        s_info.state   = BAT_ST_DISCHARGE;
        s_info.percent = pct_from_curve(curve_dis, sizeof(curve_dis) / sizeof(curve_dis[0]),
                                        s_filt_mv);
    }
    else if (s_info.eoc || s_info.hw_state == HAL_PMU_CHG_HW_ST_EOC)
    {
        s_info.state   = BAT_ST_FULL;
        s_info.percent = pct_from_curve(curve_chg, sizeof(curve_chg) / sizeof(curve_chg[0]),
                                        s_filt_mv);
    }
    else
    {
        s_info.state   = BAT_ST_CHARGING;
        s_info.percent = pct_from_curve(curve_chg, sizeof(curve_chg) / sizeof(curve_chg[0]),
                                        s_filt_mv);
    }

    return &s_info;
}


void bat_mon_dump(void)
{
    static const char *const st_name[] = { "unknown", "discharge", "charging", "full" };
    const bat_info_t *o = &s_info;
    int st = (int)o->state;

    /* 只读指针不能拿来纠正取值，所以夹在一个局部变量上。 */
    if (st < 0 || st > (int)BAT_ST_FULL)
    {
        st = (int)BAT_ST_UNKNOWN;
    }

    /* 这一行的每个字段都能和串口里驱动自己打的 "ch[7]voltage=xxxxx" 对上：
     * 那个 xxxxx 是 0.1mV，除以 10 就是这里的 voltage_mv。
     * spread 是连读 BAT_SAMPLE_N 遍的极差 —— 怀疑板子没接电池时看这个数：
     * 真电池 ~2 mV，悬空 ~100 mV。 */
    rt_kprintf("bat: %d mV -> %d%%  state=%s  spread=%d mV  "
               "vbus=%d eoc=%d hw=0x%02x  CHG_SR=0x%08x CHG_CR1=0x%08x\n",
               o->voltage_mv, o->percent, st_name[st], o->spread_mv,
               o->vbus, o->eoc, o->hw_state,
               (unsigned)o->chg_sr, (unsigned)o->chg_cr1);
}
