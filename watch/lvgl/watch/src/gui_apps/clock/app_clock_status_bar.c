#include "app_clock_status_bar.h"
#include "app_mem.h"
#include "ble_app.h"         /* 蓝牙校时开关 + 状态（实现在 src/app_utils/ble_app.c） */

typedef struct
{
    lv_color_t title_color;
    uint32_t  title_font_size;
    lv_color_t content_color;
    uint32_t  content_font_size;
    lv_coord_t content_height;
    const uint8_t *p_title;
    const uint8_t *p_content;
} notification_msg_cntxt_t;

static lv_obj_t *app_clock_main_status_bar;
static lv_obj_t *status_bar_area_up;
static lv_obj_t *status_bar_area_down;

/* 这里原来有 alarm_enabled / timer_enabled 两个 static 标志位，2026-09-21 删除。
 * 它们是原厂 demo 的遗留：只有"定义"和"被按钮翻转"两处，**全工程没有任何
 * 代码读取过它们** —— 所以按 ALARM/TIMER 除了让按钮变个色，什么都不会发生。 */


static lv_obj_t *app_clock_tileview;


/* 四个格子的控制位。
 *
 * 2026-09-21 之前，三个格子全是摆设，而且是两种不同的"假"：
 *   GPS   标着 LV_BTNMATRIX_CTRL_DISABLED —— 压根按不动。这块板子上
 *         **没有 GPS/北斗**（板级 board.conf 和整个 SDK 里搜不到 gnss/gps），
 *         这个位置从一开始就不该叫 GPS。
 *   ALARM / TIMER  按得动，但只翻转 alarm_enabled / timer_enabled 两个
 *         **没有任何代码读取**的 static 标志位（全工程只有定义和赋值）。
 *         除了按钮自己变个色，什么都不会发生。
 *
 * 现在四个都是真功能，而且都核过底层确实实现了：
 *   蓝牙校时  BT 的开关。CHECKABLE 让按钮自带开/关视觉反馈。
 *   手动校时  打开年/月/日 面板（app_clock_band.c 那套）。
 *   亮度      背光 100/60/30/10% 循环（drv_lcd.c 的 lcd_set_brightness）。
 *   息屏      立刻 gui_pm_fsm(SLEEP)，= 不等那 10 秒。
 *
 * 顺序要和 btnm_map 一一对应（按行读）。 */
static const lv_btnmatrix_ctrl_t btnm_ctrl_map[] =
{
    1 | LV_BTNMATRIX_CTRL_CHECKABLE, 1,   /* 蓝牙校时 | 手动校时 */
    1,                               1,   /* 亮度     | 息屏 */
};

/* 面板按钮要用的三个能力入口。它们分别住在别的文件里，各自只暴露一个
 * 函数，这里 extern 声明后直接调 —— 面板不需要知道 lcd_device、PM 状态机、
 * 校时面板内部结构这些细节。 */
extern void app_watch_brightness_cycle(void);   /* watch_demo.c  背光档位 */
extern void app_watch_sleep_now(void);          /* watch_demo.c  立刻息屏 */
extern void app_clock_band_open_calib(void);    /* app_clock_band.c 手动校时 */

/* ⚠⚠ 末尾那个空串 "" 是**数组终止符**，不是多余的按钮，绝对不能删！
 *
 * lv_btnmatrix.c 的 allocate_btn_areas_and_controls() 是这样数按钮的：
 *      while(map[i] && map[i][0] != '\0') { ... ; i++; }
 * 也就是说它靠"空字符串"来发现表尾。删掉它，循环就会一直往后读 .rodata，
 * 把相邻的常量当指针用 —— 实测读出一个 0xFD8E0100，再去取 map[i][0]
 * 当场 mem manage fault（DACCVIOL）。2026-09-21 已踩，改完崩在
 * lv_btnmatrix_set_map 里。
 *
 * 空串本身不会被计成按钮（btn_cnt 只数非 "\n" 且非空串的项），所以这里
 * 仍然是 4 个按钮、2 行，布局就是 2×2。
 *
 * ⚠ 标签改成中文后，有一件事必须配套做：**给按钮项单独设中文字体**。
 * 按钮上的字用的是 LV_PART_ITEMS 的字体（lv_btnmatrix.c 里是
 * lv_obj_init_draw_label_dsc(obj, LV_PART_ITEMS, ...)），默认主题给的是
 * LV_FONT_DEFAULT，未必带中文字形 —— 不设就会看到四个空框。
 * 见 control_panel_content_init() 里的 lv_obj_set_style_text_font(...)。
 *
 * 顺序和 btnm_ctrl_map 一一对应（按行读）。 */
static const char *btnm_map[] = {"蓝牙校时", "手动校时", "\n",
                                 "亮度",     "息屏",     ""
                                };
static const notification_msg_cntxt_t notify_msgs[] =
{
    {
        LV_COLOR_MAKE(0xFF, 0xFF, 0xFF), FONT_SUBTITLE,
        LV_COLOR_MAKE(0x00, 0xFF, 0x00), FONT_SUPER, LV_PCT(40),
        "思澈科技欢迎您",
        "思澈科技欢迎您"
    },
    {
        LV_COLOR_MAKE(0xFF, 0xFF, 0xFF), FONT_SUBTITLE,
        LV_COLOR_MAKE(0x00, 0xFF, 0x00), FONT_HUGE, LV_PCT(20),
        "思澈科技欢迎您",
        "思澈科技欢迎您"
    },
    {
        LV_COLOR_MAKE(0xFF, 0xFF, 0xFF), FONT_SUBTITLE,
        LV_COLOR_MAKE(0x00, 0xFF, 0x00), FONT_BIGL, LV_PCT(20),
        "思澈科技欢迎您",
        "思澈科技欢迎您"
    },
    {
        LV_COLOR_MAKE(0xFF, 0xFF, 0xFF), FONT_SUBTITLE,
        LV_COLOR_MAKE(0x00, 0x00, 0x00), FONT_NORMAL, LV_PCT(30),
        "关于我们",
        "思澈科技是一家专注于物联网技术的公司，提供一站式的物联网解决方案。最先提出嵌入式MCU+GPU的物联网解决方案，为客户提供更高性能、更低功耗的物联网产品。"
    },

    {
        LV_COLOR_MAKE(0xFF, 0xFF, 0xFF), FONT_NORMAL,
        LV_COLOR_MAKE(0x00, 0x00, 0xFF), FONT_SMALL, LV_PCT(20),
        "我们是谁",
        "思澈科技成立於2019年3月，總部位於上海張江高科技園區，在重慶、北京、深圳、蘇州均設有分子公司，團隊成員均來自於美國、中國的一線電晶體設計企業，包括Marvell、 Broadcom、Amazon、 紫光展銳、聯發科等，碩士以上學歷占比超過80%； 團隊骨幹具有豐富的產品定義->自主研發->大規模量產的全流程經驗，由這些骨幹成員主導研發的晶片累計出貨超過10億顆。"
    },

};


#define PX_5mm LV_DPX(32) //160 is 1 inch(about 2.5cm)
/* 这块屏 dpi≈300（实测 PX_5mm 落到 60px），所以 2mm ≈ 24px。
 * 面板四周原来用 5mm(60px) 留白，四方格被挤在中间显得空 —— 改用 2mm。 */
#define PX_2mm LV_DPX(13) //160 is 1 inch(about 2.5cm)
#define PX_1cm LV_DPX(64) //160 is 1 inch(about 2.5cm)

/* ── 2026-09-21：原厂的「渐变标题」拆掉了 ────────────────────────────────
 *
 * 原来这里有个 ENABLE_GRADIENT_LABEL 开关，配上下面那个 gradient_label()，
 * 会在面板顶上画一条**被文字镂空的红蓝渐变色带**，文字是
 * "Functioooooooooooooooons"（把 o 拖长是为了铺满那张写死 200x45 的遮罩，
 * 不然渐变露不全 —— 纯演示效果，不承载信息）。
 * 用户看到的是"标题栏怎么是一串 Functioooooo，是乱码吗"。
 *
 * 现在换成普通中文标签「功能」。整段渐变机制（开关宏 + gradient_label() +
 * add_mask_event_cb() + MASK_* 宏）一并删除，不留死代码。
 * 顺带说明：那张遮罩是 lv_canvas_draw_text 用**默认字体**画的，
 * 尺寸写死 200x45 —— 想保留渐变还得赌中文字形存在，不如直接删掉。
 * ──────────────────────────────────────────────────────────────────────── */

/* ══════════════════════════════════════════════════════════════════════════
 *  蓝牙校时的状态显示
 * ══════════════════════════════════════════════════════════════════════════
 *  BT 按钮只负责开/关广播，但"协议栈起没起来""手机连上没""时间写进来没"
 *  这些都是用户看不见的。所以在按钮下面挂一行字说明当前处于哪一步 ——
 *  跟表盘上「未校时 · 点这里校准」是同一个思路：别让人猜。
 *
 *  为什么用 lv_timer 轮询而不是让蓝牙那边直接改 label：
 *  连上/写完时间这两个事件发生在蓝牙线程里，而 LVGL **不是线程安全**的，
 *  跨线程碰 widget 会随机崩。所以这边只读一个快照（ble_app_* 都是
 *  读几个 uint8），200ms 一次，代价可以忽略。
 */
static lv_obj_t   *ble_status_label = NULL;
static lv_timer_t *ble_status_timer = NULL;
static const char *ble_status_last  = NULL;   /* 指向下面那些字符串字面量 */

static void ble_status_refresh(void)
{
    const char *txt;

    if (ble_status_label == NULL)
    {
        return;
    }

    if (ble_app_connected())
    {
        txt = (ble_app_sync_count() > 0)
              ? "蓝牙：已连上 · 已校时"
              : "蓝牙：已连上";
    }
    else if (ble_app_radio_on())
    {
        txt = "蓝牙：广播中 · 用手机对时";
    }
    else if (ble_app_ready())
    {
        txt = "蓝牙：就绪 · 点「蓝牙校时」";
    }
    else
    {
        txt = "蓝牙：启动中";
    }

    /* 比指针就行：上面返回的全是字符串字面量，内容是稳定的 */
    if (txt != ble_status_last)
    {
        ble_status_last = txt;
        lv_label_set_text(ble_status_label, txt);
    }
}

static void ble_status_timer_cb(lv_timer_t *task)
{
    (void)task;
    ble_status_refresh();
}

static void app_clock_main_press_to_show_status_bar(lv_event_t *event)
{

    if (LV_EVENT_PRESSED == event->code)
    {
        //rt_kprintf("app_clock_main_press_to_show_status_bar\n");

        lv_obj_set_tile_id(app_clock_main_status_bar, 0, 1, false);
        lv_obj_clear_flag(app_clock_main_status_bar, LV_OBJ_FLAG_HIDDEN);
        //Bring status bar to foreground
        lv_obj_move_foreground(app_clock_main_status_bar);
    }
}



static void app_clock_main_status_bar_event_cb(lv_event_t *event)
{
    lv_obj_t *obj = lv_event_get_target(event);


    switch (event->code)
    {
    case LV_EVENT_RELEASED:
    case LV_EVENT_VALUE_CHANGED:
    {
        rt_uint32_t active_pos = (rt_uint32_t)lv_event_get_param(event);
        rt_kprintf("LV_EVENT_VALUE_CHANGED  %d\n", active_pos);

        if (1 == active_pos)
            lv_obj_add_flag(app_clock_main_status_bar, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_clear_flag(app_clock_main_status_bar, LV_OBJ_FLAG_HIDDEN);

        if (1 == active_pos) lv_ext_font_reset();


        break;
    }
    case LV_EVENT_SHORT_CLICKED:
    case LV_EVENT_LONG_PRESSED:
    case LV_EVENT_CLICKED:

    case LV_EVENT_FOCUSED:
    default:
        //printf("Released\n");

        break;
    }
}

static void btnm_event_handler(lv_event_t *event)
{
    lv_obj_t *obj = lv_event_get_target(event);

    if (event->code == LV_EVENT_VALUE_CHANGED)
    {
        const char *txt = lv_btnmatrix_get_btn_text(obj, lv_btnmatrix_get_selected_btn(obj));

        if (txt)
        {
            /* 每次点击都打一行。★ 必须同时打**格子编号**：
             * 串口控制台是 ASCII 的，中文标签会打成一串 "?"，
             * 光看 "?" 根本分不出点的是哪个格子（实测踩到）。
             * 编号对应 btnm_map 按行读的顺序：0 蓝牙校时 / 1 手动校时 /
             * 2 亮度 / 3 息屏。 */
            rt_kprintf("panel: btn[%d] \"%s\"\n",
                       (int)lv_btnmatrix_get_selected_btn(obj), txt);

            /* 蓝牙校时：开/关广播。状态行 200ms 后自己会跟上，这里立刻刷一次
             * 是为了让手指按下去就看见字变了，不用等下一拍。 */
            if (0 == strcmp(txt, "蓝牙校时"))
            {
                ble_app_toggle();
                ble_status_refresh();
            }
            else if (0 == strcmp(txt, "手动校时"))
            {
                app_clock_band_open_calib();
            }
            else if (0 == strcmp(txt, "亮度"))
            {
                app_watch_brightness_cycle();
            }
            else if (0 == strcmp(txt, "息屏"))
            {
                app_watch_sleep_now();
            }
        }
    }
}

/* 原厂的 gradient_label() / add_mask_event_cb() / MASK_* 宏在 2026-09-21
 * 整体删除，面板标题换成了普通中文标签「功能」。
 * 来历和删除原因见文件前面 PX_2mm 下面那段说明。 */

/* 量一下某个元素相对面板的左右留白，打到串口。
 * 左右相等才算真的居中 —— "看着别扭"这种事靠肉眼猜不如直接量。 */
static void panel_dump_center(const char *name, lv_obj_t *obj, lv_obj_t *par)
{
    lv_area_t a, p;
    int left, right;

    lv_obj_get_coords(obj, &a);
    lv_obj_get_coords(par, &p);
    left  = (int)(a.x1 - p.x1);
    right = (int)(p.x2 - a.x2);

    rt_kprintf("panel: %-7s x=[%d..%d] y=[%d..%d] %dx%d  left=%d right=%d %s\n",
               name, (int)a.x1, (int)a.x2, (int)a.y1, (int)a.y2,
               (int)(a.x2 - a.x1 + 1), (int)(a.y2 - a.y1 + 1), left, right,
               (left == right) ? "OK-centered" : "**NOT-centered**");
}

/* ══════════════════════════════════════════════════════════════════════════
 *  上划面板第三页：功能按钮 + 蓝牙状态行
 * ══════════════════════════════════════════════════════════════════════════
 * 排布改成 flex 竖向流 + 交叉轴居中，不再用 lv_obj_align_to 串糖葫芦。
 *
 * 为什么换掉 align_to：它在**调用的那一刻**就把 x 算死了，之后谁变宽都不会重算
 * （实现见 lvgl_v8/src/core/lv_obj_pos.c：OUT_BOTTOM_MID 就是
 * "base 宽/2 − obj 宽/2"，算完即定）。这面板上正好有个会变宽的东西 ——
 * 蓝牙状态行的字随状态变长：
 *      "蓝牙：启动中"          6 字
 *      "蓝牙：已连上 · 已校时" 12 字
 * 字一变长，label 只会往右长、左边不动，看着就是"这句字没居中"。
 * flex 每次布局都重算位置，天然没这个毛病。
 *
 * 顺手清掉一样死东西：原厂 demo 留的两个调试滑块
 * （"clock redraw time & steps" 下面那两条）。`lv_obj_set_event_cb` 两行
 * 都被注释掉了，拖它不会有任何效果。
 *
 * ── 2026-09-21 二次调整：四方格铺满 ───────────────────────────────────
 * 原来三处让内容缩在中间，看着"没铺满"：
 *   ① 面板 pad 用 PX_5mm，这块屏 dpi≈300 ⇒ 整整 60px 的边，四周一圈白给；
 *   ② btnm 宽度写死 LV_PCT(75)，又把剩下的宽度砍掉四分之一
 *      （(390-120)*0.75 ≈ 202px，而它本可以占满 390-120=270px）；
 *   ③ 高度是内容自适应，只占面板上半截，下半截全空着。
 * 现在改成：pad 收到 PX_2mm(24px)、宽度 100%、高度用 flex_grow 吃掉剩余的
 * 竖向空间 —— 四个格子自动撑到"左右贴边、上下到底"，换屏幕尺寸也不用重算。
 */
static void control_panel_content_init(lv_obj_t *par)
{
    lv_obj_t *label1;
    lv_obj_t *btnm1;

    /* 面板自己当容器：竖向排列、子项水平居中、行距 2mm。
     * "居中"从此由布局保证，不再依赖某一次手动对齐的结果。 */
    lv_obj_set_flex_flow(par, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(par, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(par, PX_2mm, 0);
    lv_obj_set_style_pad_row(par, PX_2mm, 0);

    /* 标题。成了 flex 的子项就不用再手动对齐：父容器带 layout 时，
     * 子项的 align 样式会被忽略，位置一律由 layout 决定。
     *
     * 用白色是因为面板底是纯黑（见本文件 app_clock_main_status_bar_init 里
     * pages[2] 的 bg_color = LV_COLOR_BLACK）。
     * 这里原来画的是那条**被文字镂空的红蓝渐变色带**，文字
     * "Functioooooooooooooooons"（o 拖长是为了铺满写死 200x45 的遮罩，
     * 让整条渐变露出来）—— 用户看到的就是"标题栏怎么是一串 Functioooooo"。
     * 已换成普通中文标签。 */
    label1 = lv_label_create(par);
    lv_label_set_text(label1, "功能");
    lv_ext_set_local_font(label1, FONT_SUBTITLE, lv_color_white());

    btnm1 = lv_btnmatrix_create(par);
    lv_btnmatrix_set_map(btnm1, btnm_map);

    /* ★ 中文字体必须单独设在 LV_PART_ITEMS 上，否则按钮上是四个空框。
     *
     * 原因：按钮上的字不是用 LV_PART_MAIN 画的 —— lv_btnmatrix.c 里是
     * lv_obj_init_draw_label_dsc(obj, LV_PART_ITEMS, ...)；
     * 而 lv_ext_set_local_font() 内部只设 LV_PART_MAIN|LV_STATE_DEFAULT
     * （见 lvsf_font.c），对按钮矩阵不生效。默认主题给的 LV_FONT_DEFAULT
     * 又未必带中文字形（原来按钮写英文很可能就是这个原因）。
     * LV_EXT_FONT_GET 取的就是主题对应字号那套字体 —— 状态行用 FONT_NORMAL
     * 显示中文是正常的，证明它有中文字形。
     *
     * 只设字体、**不设颜色**：默认主题里按钮文字颜色是随
     * checked / disabled 状态变的，用本地样式盖掉会把选中态一起盖没。 */
    lv_obj_set_style_text_font(btnm1, LV_EXT_FONT_GET(FONT_NORMAL), LV_PART_ITEMS);

    /* 宽度占满内容区（原来 75%，白白让掉四分之一）；高度交给 flex_grow 撑满剩余空间。
     * 列方向的 grow 只作用于主轴(竖向)，横向宽度仍由上面这行决定，
     * 所以"铺满"是横向贴边 + 纵向到底，两件事互相不冲突。 */
    lv_obj_set_width(btnm1, LV_PCT(100));
    lv_obj_set_flex_grow(btnm1, 1);
    lv_obj_add_event_cb(btnm1, btnm_event_handler, LV_EVENT_ALL, NULL);
    lv_btnmatrix_set_ctrl_map(btnm1, btnm_ctrl_map);

    /* ── 蓝牙状态行 ────────────────────────────────────────────────────
     * 挂在按钮正下方。琥珀色(#FFC107)：和表盘上「未校时」的提示同一套
     * 视觉语言 —— 这块屏上黄色一律表示"有件事等你做"。
     * ble_status_last 先清成 NULL，逼第一拍把文字写进去。
     */
    ble_status_label = lv_label_create(par);
    lv_label_set_text(ble_status_label, "蓝牙：启动中");
    /* 注意用 lv_color_make（函数），不是 LV_COLOR_MAKE（花括号初始化宏，不能当实参） */
    lv_ext_set_local_font(ble_status_label, FONT_NORMAL, lv_color_make(0xFF, 0xC1, 0x07));
    /* 撑满整行 + 文字居中。
     * flex 只管把"框"摆正，框里那行字怎么放是 label 自己的事：不设 width 时
     * label 是"内容宽"，文字一变长就只往右长，左边不动。设成 100% 宽 + 居中，
     * 换多少字它都还在正中间。 */
    lv_obj_set_width(ble_status_label, LV_PCT(100));
    lv_obj_set_style_text_align(ble_status_label, LV_TEXT_ALIGN_CENTER, 0);

    ble_status_last  = NULL;
    ble_status_timer = lv_timer_create(ble_status_timer_cb, 200, NULL);
    ble_status_refresh();

    /* 一次性自检：把三样东西的左右留白量出来（左右相等才算居中），
     * 再把 4 个格子的真实像素尺寸打出来 —— "铺满没铺满"要看数字，别靠肉眼猜。 */
    lv_obj_update_layout(par);
    rt_kprintf("panel: container %dx%d  pad=%d row=%d\n",
               (int)lv_obj_get_width(par), (int)lv_obj_get_height(par),
               (int)PX_2mm, (int)PX_2mm);
    panel_dump_center("title", label1, par);
    panel_dump_center("btnm", btnm1, par);
    panel_dump_center("status", ble_status_label, par);

    /* 按钮矩阵的内部数据是公开的（lv_btnmatrix.h 里有结构体定义），
     * 直接把每个格子的面积读出来，省得"看着像铺满了"这种玄学结论。 */
    {
        lv_btnmatrix_t *bm = (lv_btnmatrix_t *)btnm1;

        if (bm->button_areas != NULL)
        {
            rt_kprintf("panel: grid btn_cnt=%d row_cnt=%d  item_pad row=%d col=%d\n",
                       (int)bm->btn_cnt, (int)bm->row_cnt,
                       (int)lv_obj_get_style_pad_row(btnm1, LV_PART_MAIN),
                       (int)lv_obj_get_style_pad_column(btnm1, LV_PART_MAIN));

            for (uint16_t i = 0; i < bm->btn_cnt; i++)
            {
                lv_area_t *b = &bm->button_areas[i];
                rt_kprintf("panel: grid cell[%d] %dx%d\n", (int)i,
                           (int)(b->x2 - b->x1 + 1), (int)(b->y2 - b->y1 + 1));
            }

            /* 标签和字体也打出来。屏幕上到底有没有中文字形只能靠眼睛看，
             * 但"字符串有没有正确编进固件""字体有没有真的设到 LV_PART_ITEMS 上"
             * 这两件事从串口就能确认 —— 免得又出现"改了但没生效"查不出来。 */
            for (uint16_t i = 0; i < bm->btn_cnt; i++)
            {
                rt_kprintf("panel: label[%d] \"%s\"\n", (int)i,
                           lv_btnmatrix_get_btn_text(btnm1, i));
            }

            rt_kprintf("panel: item font=%p  ext FONT_NORMAL=%p\n",
                       (void *)lv_obj_get_style_text_font(btnm1, LV_PART_ITEMS),
                       (void *)LV_EXT_FONT_GET(FONT_NORMAL));
        }
    }
}

/*
    drop down hidden msg list
*/
static void msg_list_content_init(lv_obj_t *par)
{
    lv_obj_t *label_header;
    lv_obj_t *align_base;

    label_header = lv_label_create(par);
    lv_label_set_text(label_header, "Notifications");
    lv_ext_set_local_font(label_header, FONT_TITLE, LV_COLOR_WHITE);
    lv_obj_set_size(label_header, LV_PCT(100), LV_PCT(5));
    lv_obj_align(label_header, LV_ALIGN_TOP_MID, 0, LV_PCT(5));

    align_base = label_header;
    for (uint32_t i = 0; i < sizeof(notify_msgs) / sizeof(notify_msgs[0]); i++)
    {
        lv_obj_t *title_label = lv_label_create(par);
        lv_obj_set_width(title_label, LV_PCT(80));
        lv_label_set_long_mode(title_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_ext_set_local_font(title_label, notify_msgs[i].title_font_size, notify_msgs[i].title_color);
        lv_label_set_text(title_label, notify_msgs[i].p_title);


        lv_obj_t *content_label_container = lv_obj_create(par);
        lv_obj_set_size(content_label_container, LV_PCT(100), notify_msgs[i].content_height);
        lv_ext_set_local_bg(content_label_container, LV_COLOR_GRAY, LV_OPA_80);


        lv_obj_t *content_label = lv_label_create(content_label_container);
        lv_obj_set_width(content_label, LV_PCT(100));
        lv_label_set_long_mode(content_label, LV_LABEL_LONG_WRAP);
        lv_ext_set_local_font(content_label, notify_msgs[i].content_font_size, notify_msgs[i].content_color);
        lv_label_set_text(content_label, notify_msgs[i].p_content);
        lv_obj_set_layout(content_label, LV_LAYOUT_FLEX);


        lv_obj_align_to(title_label, align_base, LV_ALIGN_OUT_BOTTOM_LEFT, 0, PX_5mm);
        lv_obj_align_to(content_label_container, title_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 0);
        align_base = content_label_container;
    }
}

void app_clock_main_status_bar_init(lv_obj_t *par, lv_obj_t *clock_tileview)
{
    rt_uint16_t i;
    lv_obj_t *tileview;
    lv_obj_t *pages[3];
    lv_obj_t *status_bar_area;

    //create a invisible object at top of parent, and shown status bar when press it
    for (i = 0; i < 2; i++)
    {
        status_bar_area = lv_obj_create(par);
        lv_obj_set_size(status_bar_area, LV_HOR_RES_MAX, (LV_VER_RES_MAX >> 4));
        lv_obj_set_style_border_opa(status_bar_area, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_scrollbar_mode(status_bar_area, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(status_bar_area, LV_OBJ_FLAG_PRESS_LOCK); //Allow press event to tileview
        lv_obj_add_event_cb(status_bar_area, app_clock_main_press_to_show_status_bar, LV_EVENT_ALL, NULL);
        lv_obj_set_style_bg_opa(status_bar_area, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);


        if (0 == i)
        {
            lv_obj_align(status_bar_area, LV_ALIGN_BOTTOM_MID, 0, 0);
            status_bar_area_down = status_bar_area;
        }
        else if (1 == i)
        {
            lv_obj_align(status_bar_area, LV_ALIGN_TOP_MID, 0, 0);
            status_bar_area_up = status_bar_area;
        }

    }

    //create tile view , page 0  for content, page 1 is transparent
    tileview = lv_tileview_create(par);
    app_clock_main_status_bar = tileview;
    lv_obj_add_flag(tileview, LV_OBJ_FLAG_SCROLL_ONE);
    lv_obj_set_scrollbar_mode(tileview, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(tileview, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);


    for (i = 0; i < 3; i++)
    {
        pages[i] = lv_tileview_add_tile(tileview, 0, i, LV_DIR_VER);

        if (i == 1)
        {
            lv_obj_set_style_bg_opa(pages[i], LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        else
        {
            lv_color_t color = LV_COLOR_BLACK;
            lv_obj_set_style_bg_opa(pages[i], LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(pages[i], color, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        lv_obj_set_size(pages[i], LV_HOR_RES_MAX, LV_VER_RES_MAX);
        lv_obj_set_pos(pages[i], 0, (LV_VER_RES_MAX * i));
        lv_obj_set_scrollbar_mode(pages[i], LV_SCROLLBAR_MODE_OFF);
    }

    msg_list_content_init(pages[0]);
    control_panel_content_init(pages[2]);

    //scroll to page[1]
    lv_obj_set_tile_id(tileview, 0, 1, false);
    lv_obj_add_event_cb(tileview, app_clock_main_status_bar_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_flag(tileview, LV_OBJ_FLAG_HIDDEN);

    app_clock_tileview = clock_tileview;
}

void app_clock_main_status_bar_deinit(void)
{
    /* 先删定时器再删对象：定时器回调里会碰 ble_status_label，
     * 反过来做的话，最后一次回调可能落在已经释放的 label 上。 */
    if (ble_status_timer != NULL)
    {
        lv_timer_del(ble_status_timer);
        ble_status_timer = NULL;
    }
    ble_status_label = NULL;
    ble_status_last = NULL;

    lv_obj_del(app_clock_main_status_bar);
    lv_obj_del(status_bar_area_up);
    lv_obj_del(status_bar_area_down);

    app_clock_main_status_bar = NULL;
    status_bar_area_up = NULL;
    status_bar_area_down = NULL;
    lv_ext_font_reset();
}
