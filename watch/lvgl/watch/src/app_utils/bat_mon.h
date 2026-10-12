/**
 * @file    bat_mon.h
 * @brief   电池监测：电压 -> 电量百分比，以及「插没插充电线 / 充满没充满」
 *
 * 为什么单独一个文件
 * ------------------
 * 这是「读硬件」的事，和「怎么画」无关。表盘现在用它，以后上划面板想显示电量
 * 也是调这几个函数，不用把表盘里的代码抄一遍。
 *
 * 两条数据来源
 * ------------
 *   电压 —— ADC 设备 "bat1" 的通道 7。这一路在芯片内部已经用分压电阻接到了
 *           VBAT，外面不用再焊分压电阻（见 adc/README.md 的 ADC 配置流程）。
 *           需要 proj.conf 里开 CONFIG_BSP_USING_ADC1=y，否则 rt_device_find
 *           找不到 "bat1"，本模块会返回「读不到」，界面显示 "--%"。
 *   充电 —— PMU 充电状态寄存器 CHG_SR。**只读寄存器**，不初始化充电器、
 *           不注册中断、不改任何 PMU 配置，所以不需要 BSP_USING_CHARGER 开关。
 */
#ifndef __BAT_MON_H__
#define __BAT_MON_H__

#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    BAT_ST_UNKNOWN = 0,     /* 读不到电池（没插电池 / ADC 没开） */
    BAT_ST_DISCHARGE,       /* 电池供电 */
    BAT_ST_CHARGING,        /* 插着线，正在充 */
    BAT_ST_FULL,            /* 插着线，已经充满 */
} bat_state_t;

typedef struct
{
    int         voltage_mv;   /* 电池电压（mV）；<0 = 读不到 */
    int         percent;      /* 0..100；<0 = 读不到 */
    bat_state_t state;

    /* 下面几个是给串口诊断用的原始值，界面不用 */
    rt_uint8_t  vbus;         /* 1 = PMU 认为 VBUS 就绪（插着线） */
    rt_uint8_t  eoc;          /* 1 = PMU 报「充电结束」 */
    rt_uint8_t  hw_state;     /* CHG_SR.CHG_STATE：1=OFF 2=PWRUP 4=IDLE 8=PRECC 0x10=CC 0x20=CV 0x40=EOC */
    int         spread_mv;    /* 同一次里连读几遍的极差（mV）：真电池 ~2，悬空 ~100 */
    rt_uint32_t chg_sr;       /* CHG_SR 整寄存器 */
    rt_uint32_t chg_cr1;      /* CHG_CR1 整寄存器（bit0 = 充电使能） */
} bat_info_t;

/**
 * @brief 读一次电池信息。内部带低通滤波，连续调用不会抖动。
 *
 * @return 指向模块内部常驻的结构体，**不要保存后长期使用**，也不要 free。
 *         （每次调用都返回同一个地址，内容会被下一次调用覆盖。）
 *
 * 注意：一次调用大约要 ~66 ms —— 驱动内部每读一遍是 22 次采样去极值再平均，
 *       本模块为了判断「是不是真的接了电池」要连读 3 遍。所以别放进每秒都跑的
 *       循环里，10 秒一次足够（表盘就是这么用的）。
 */
const bat_info_t *bat_mon_get(void);

/**
 * @brief 把最新一次结果打一行到串口（方便不用眼睛看屏幕就能确认读数）。
 */
void bat_mon_dump(void);

#ifdef __cplusplus
}
#endif

#endif /* __BAT_MON_H__ */
