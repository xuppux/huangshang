/* 本文件由 watchface-tools/make_assets.py 自动生成，请勿手改。
 * 想改字号/配色/留白 —— 改 make_assets.py 的 CONFIG 区然后重跑，这里会跟着更新。 */
#ifndef WF_LAYOUT_H
#define WF_LAYOUT_H

#define WF_SCR_W               390
#define WF_SCR_H               450

/* ---- 大号时间 HH:MM ---- */
#define WF_TIME_Y              140
#define WF_DIGIT_W             82
#define WF_DIGIT_H             105
#define WF_COLON_W             39
#define WF_COLON_H             105
/* 时间行在屏上的左起 x（水平居中；0号格/1号格/冒号/2号格/3号格 依次排开）*/
#define WF_TIME_X0             11

/* ---- 日期行 ---- */
#define WF_DATE_Y              259
#define WF_DATE_H              37
#define WF_SDIGIT_W            32
#define WF_YM_W                34
#define WF_YD_W                31
#define WF_WEEK_W              59
#define WF_DAY_GAP             0
#define WF_WEEK_SEP            22

#endif /* WF_LAYOUT_H */
