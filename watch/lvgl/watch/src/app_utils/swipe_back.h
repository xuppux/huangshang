#ifndef __SWIPE_BACK_H__
#define __SWIPE_BACK_H__

/*
 * swipe_back.h —— 右边缘「从右往左滑」触发返回上一页
 *
 * 用法：在 LVGL / app 框架初始化完成之后调用一次 swipe_back_init()。
 * 目前挂在 src/gui_apps/watch_demo.c 的 main() 里，紧跟着 SDK 自己的
 * lvsf_gesture_init(lv_layer_top()) —— 那条负责「左边缘往右滑」，
 * 这条负责「右边缘往左滑」，两条互不干扰。
 *
 * 语义是"触发式"：识别手势，然后调 gui_app_goback() 让框架播一段返回动画。
 * 画面不跟手。原因见 swipe_back.c 顶部（手动跟手那条路的 abort 语义会
 * 把页面状态搞错位，导致返回后蜂窝卡住）。
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 幂等：重复调用只会有一条检测条。必须在 LVGL 初始化之后调用。 */
void swipe_back_init(void);

/* 开 / 关这条手势。
 *
 * 蜂窝（根页面）必须关掉：它没有上一级，滑它会让框架去走
 * app_run_by_id(main) / app_restart_last_suspend()，行为不可预期。
 * 见 src/gui_apps/main/app_mainmenu.c 的 on_resume / on_pause。
 *
 * 关闭时会把检测条的 CLICKABLE 一起清掉 —— 否则那条压在最右 16px 的
 * 透明检测条会继续吃掉触摸，蜂窝最右一列的图标就点不动了。 */
void swipe_back_set_enabled(bool enable);

#ifdef __cplusplus
}
#endif

#endif /* __SWIPE_BACK_H__ */
