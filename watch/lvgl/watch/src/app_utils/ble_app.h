/*
 * ble_app.h —— 蓝牙（BLE）校时 + 文件头说明
 *
 * ⚠ 为什么这个文件放在 src/app_utils/ 而不是 gui_apps/clock/：
 *   src/app_utils/SConscript 里本来就写着
 *       if GetDepend('BSP_BLE_SIBLES'):  src += ['ble_app.c']
 *   也就是**只要打开蓝牙，构建系统就要求这个文件存在**。
 *   工程作者当初就是这么安排的（BLE 是系统级服务，不归某一个 App）。
 *   放别处的话编译会在链接前报 "Source ... ble_app.c not found"。
 *
 * 为什么要它：这块板子**没有 RTC 备份电池**。拔电、以及用 sftool 烧录，
 * RTC 域都会掉电归零，开机兜底成 2024-01-01 12:00。板上那个触摸校时面板
 * 只能"每次掉了手动再对一遍"，治标不治本。
 * 蓝牙校时的意义是：手机点一下，时间就过来了，不用连电脑、不用敲串口。
 *
 * 走的是**蓝牙官方标准协议**，不是自定义私有协议：
 *     Current Time Service (CTS)，服务 UUID 0x1805
 *     当前时间特征 0x2A2B（可读 / 可写 / 可通知）
 * 好处是任何通用蓝牙调试 App（nRF Connect、LightBlue…）都能给这块表对时，
 * 不依赖我自己写的上位机。网页端走 Web Bluetooth，也是直接写这个标准特征。
 *
 * 服务端代码不用自己写：SDK 里 middleware/bluetooth/service/ble/gatt_service/
 * bf0_ble_tips.c 已经把 CTS 实现好了（ble_tips_init / BLE_TIPS_SET_CURRENT_TIME），
 * 只是整份 SDK 没有任何示例调用它。这里做的就是把它接起来。
 */
#ifndef __BLE_APP_H__
#define __BLE_APP_H__

#include <rtthread.h>

/*
 * 启动时调一次。会做两件事：
 *   1. 起一个自己的线程（BLE 事件里不能直接注册服务，会阻塞）
 *   2. sifli_ble_enable() 把蓝牙协议栈拉起来
 * 重复调用无副作用。没开蓝牙时是空实现。
 */
void ble_app_init(void);

/* 开/关广播（on!=0 开）。异步：只往邮箱丢个消息，马上返回。 */
void ble_app_radio(int on);

/* 按钮用：切换广播开关。 */
void ble_app_toggle(void);

/* 1 = 正在广播（可以连） */
rt_bool_t ble_app_radio_on(void);
/* 1 = 手机已连上 */
rt_bool_t ble_app_connected(void);
/* 1 = 协议栈已经起来（收到过 BLE_POWER_ON_IND） */
rt_bool_t ble_app_ready(void);
/* 累计成功校时次数。用来判断"刚才那一下到底写进去了没有" */
rt_uint32_t ble_app_sync_count(void);

/*
 * 蓝牙占用着射频时返回 1，此时不要做无操作息屏。
 * 原因：息屏走的是 gui_suspend() -> PM 深睡，射频会被关掉，广播直接断。
 * watch_demo.c 的 button_event_task_entry() 会查这个值。
 */
rt_bool_t ble_app_wants_awake(void);

/* 一行短状态，给串口命令 / UI 显示用 */
const char *ble_app_state_str(void);

#endif /* __BLE_APP_H__ */
