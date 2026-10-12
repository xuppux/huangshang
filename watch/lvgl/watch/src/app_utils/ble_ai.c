/*
 * ble_ai.c —— AI 助手的蓝牙数据通道（手表 <-> 电脑/手机）
 *
 * 目标：让手表能跟对端双向传文字。
 * 背景：这块板**没有 WiFi**，要联网只能借手机/电脑的网 —— 这条通道
 *       就是把数据送去/取回的方式。联网、调大模型都交给对端做。
 *
 * ══════════════════════════════════════════════════════════════════════
 *  为什么不自己写一个 GATT 服务
 * ══════════════════════════════════════════════════════════════════════
 * SDK 里已经有一个官方的「串口透传服务」：
 *     middleware/bluetooth/service/ble/gatt_service/bf0_sibles_serial_trans_service.c
 * 它就是干这个的，而且比手写好得多：
 *   · **自带分包** —— 首包带总长度，续包/末包不带，对端按 Flag 拼回来。
 *     所以**发多长的文本都行**，完全不受 BLE 单包 20 字节的限制。
 *   · **自带类别 ID** —— 多个功能共用一个 GATT 服务，靠 CateID 区分。
 *   · **发送侧全自动组头** —— 调用者只管给原始数据，包头/分包它自己弄。
 *
 * 服务长这样（128-bit UUID 的 16 个字节是 "sifli_sd" 的 ASCII）：
 *     服务   : 73 69 66 6c 69 5f 73 64 00 00 00 00 00 00 00 00
 *     configure: ... 00 01 ...   （可读可写，服务端其实不用它，写进去没反应）
 *     data     : ... 00 02 ...   （可读 + 可写 + 可通知 ← 我们只跟这个打交道）
 *
 * ⚠ 通道"打开"不需要任何握手：服务把 OPEN 事件挂在 BLE_GAP_CONNECTED_IND 上，
 *   也就是说**对端一连上，回调就收到 BLE_SERIAL_TRAN_OPEN**。
 *   电脑端只要连上 + 订阅 data 的通知，就能直接收发。
 *
 * ══════════════════════════════════════════════════════════════════════
 *  数据格式（对端必须遵循）
 * ══════════════════════════════════════════════════════════════════════
 *     | 1B CateID | 1B Flag | 2B Length | N B Data |
 *
 *   Flag: 0x00 = 完整包 / 0x01 = 首包 / 0x02 = 续包 / 0x03 = 末包
 *
 *   ⚠ Length 只在 **Flag=0 的完整包**里有意义。这不是笔误 —— 服务源码里
 *     完整包分支是把 data 直接从 `value + 4` 取的、长度用 `para->len - 4`，
 *     根本没读头里那两字节；只有首包分支才读它当"总长度"。
 *     所以对端发完整包时，头 4 字节要照写（服务会跳过），但值不参与解析。
 *
 *   ⚠ 续包/末包只有 2 字节头：`[cate][flag][data]`，没有 Length。
 *     这对端很容易写错，一次要做对。
 *
 * ══════════════════════════════════════════════════════════════════════
 *  ⚠⚠ 一个「编译通过、宏也开了，但等于没开」的坑
 * ══════════════════════════════════════════════════════════════════════
 * 只打开 CONFIG_BSP_BLE_SERIAL_TRANSMISSION 是**不够的**。实测：
 *     bf0_sibles_serial_trans_service.c   19416 字节
 *     bf0_sibles_serial_trans_service.o    2332 字节   ← 宏没开时：空壳
 *     bf0_sibles_serial_trans_service.o   55924 字节   ← 开宏后：真编了 ✓
 *     但 main.bin 只从 2,568,732 涨到 2,570,396（+1664）
 *
 * 为什么 .o 涨了 53KB、固件却只涨 1.6KB？
 *     因为**没有任何代码调用 ble_serial_tran_init()**，
 *     链接器的 --gc-sections 把整段服务当成"没人用的代码"回收掉了。
 *
 * 所以本文件存在的第二个理由就是这个：让 ble_app.c 在工作线程里调一次
 * ble_ai_serial_init()，服务才真正进固件。
 *
 * 补上调用之后重新编译，验证通过（这一串证据缺一不可）：
 *     main.bin   2,570,396 -> 2,574,104   (+3708)
 *     .text.ble_serial_tran_init / _send_data / _set_cbk / _get_cbk   都在 map 里
 *     .data.serial_trans_att_db / .bss.g_serial_tran_env               都在 map 里
 *     __SerialTranExport_start__ .. __end__ = 8 字节 = 1 个回调条目（就是我们的 0x10）
 *     main.bin 里能搜到 "ble_ai:" / "ai_send" / "ai_info" / "ai_recv" 字符串
 *
 * ══════════════════════════════════════════════════════════════════════
 *  调用时机（很讲究）
 * ══════════════════════════════════════════════════════════════════════
 * ble_serial_tran_init() 会注册 GATT 服务，**这一步会阻塞**。
 * 所以：
 *   ✅ 在 ble_app.c 的 ble_on_power_on() 里调 —— 那里已经在工作线程上
 *   ❌ 不要在 BLE_EVENT_REGISTER 注册的事件回调里调 —— 那是协议栈线程
 *
 * 完整的调用链（可以照着在日志里核对）：
 *     app_ble_event_handler (BLE_POWER_ON_IND) -> rt_mb_send(BLE_MSG_POWER_ON)
 *       -> ble_thread_entry 收邮箱 -> ble_on_power_on() -> ble_ai_serial_init()
 *
 * ══════════════════════════════════════════════════════════════════════
 *  ★★ 为什么收数据之后要绕一个线程，不直接在回调里回
 * ══════════════════════════════════════════════════════════════════════
 * 我们的回调 ble_ai_cb 是被服务的 set_cbk 同步调用的，而 set_cbk 又是在
 * sibles 收到 SIBLES_VALUE_WRITE_IND 时同步调的 —— 和 BLE 事件分发同一个
 * 上下文。在这个上下文里**不能阻塞**。
 *
 * 而 ble_serial_tran_send_data() 是可能阻塞的：它内部走 sibles_write_value()，
 * 发送队列满的时候会 rt_thread_mdelay() 重试最多 20 次（默认 50ms 一次，
 * 最坏累计一秒）。在这一秒里整个 BLE 事件分发都会停住。
 *
 * 所以这里的做法是：
 *     回调：只做 memcpy + rt_sem_release（都是非阻塞的），立刻返回
 *     工作线程：拿信号量，慢慢做发送
 * 这不是为了好看 —— 这也是**阶段二接大模型时唯一正确的落点**：
 * 以后「把问题发给电脑、等电脑的答案回来」就写在这个工作线程里。
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include <string.h>

#include "ble_ai.h"

#ifdef BSP_BLE_SERIAL_TRANSMISSION

#include "bf0_sibles.h"
#include "bf0_ble_common.h"
#include "bf0_sibles_serial_trans_service.h"

/* ── 用哪个类别 ID ──────────────────────────────────────────────────────
 * service 定义了 6 个：
 *   BLE_DFU_CATEID(0x01) / BLE_CONSOLE_CATEID(0x02) / BLE_LOG_CATEID(0x03)
 *   BLE_WATCHFACE_CATEID(0x04) / BLE_EQ_CONFIG_CATEID(0x05)
 *   **BLE_CUSTOMIZE_CATEID(0x10)**
 * 最后那个是官方**留给第三方自定义**的，我们用这个 ——
 * 免得跟 DFU 之类的系统服务抢同一个 ID（撞了会导致回调收错数据）。 */
#define BLE_AI_CATE_ID      BLE_CUSTOMIZE_CATEID

/* 单次接收上限。和服务端 att_db 里给 DATA 特征值分配的 1024 字节对齐。 */
#define BLE_AI_RX_MAX       1024

/* 发送缓冲（finsh 命令拼参数、回声回复用） */
#define BLE_AI_TX_MAX       256

/* 工作线程 */
#define BLE_AI_THREAD_STACK 3072
#define BLE_AI_THREAD_PRIO  RT_THREAD_PRIORITY_LOW   /* 25，比 BLE 线程(19)低，不抢它 */

/* 回调里 kprintf 打正文时最多打多少字符（1024 字节全打出来会把控制台刷爆） */
#define BLE_AI_LOG_MAX      64

/* ── 状态 ─────────────────────────────────────────────────────────────── */
static uint8_t  s_conn   = 0xFF;    /* 对端连接句柄；0xFF = 没有 */
static uint8_t  s_open   = 0;       /* 通道已打开 */
static uint8_t  s_inited = 0;       /* ble_serial_tran_init 跑过 */
static uint8_t  s_echo   = 1;       /* 回声模式：收到就原样回一句（阶段二的占位） */

static uint32_t s_rx_pkts  = 0;
static uint32_t s_rx_bytes = 0;
static uint32_t s_tx_pkts  = 0;
static uint32_t s_tx_err   = 0;

/* 接收缓冲。两个是有原因的：
 *   s_rx   —— 回调（协议栈线程）往里写
 *   s_last —— 工作线程把 s_rx 快照过来，然后慢慢处理
 * 用一个的话，工作线程还在发回声、对端又发来一条，就会读到半新半旧的串。
 *
 * ⚠ 也必须**自己拷一份**：回调参数里的 data 指针指向服务内部的拼包缓冲
 *   （或协议栈的临时 value），回调一返回就可能被释放/复用。
 *   直接把它存下来、后面再去读，是在读一块已经还回去的内存。 */
static char     s_rx  [BLE_AI_RX_MAX + 1];
static char     s_last[BLE_AI_RX_MAX + 1];
static uint16_t s_rx_len   = 0;
static uint16_t s_last_len = 0;
static uint32_t s_last_seq = 0;

static struct rt_semaphore s_rx_sem;
static rt_thread_t         s_worker = RT_NULL;

/* ══════════════════════════════════════════════════════════════════════
 *  数据回调 —— 跑在 BLE 事件上下文，**只做非阻塞的事**
 * ══════════════════════════════════════════════════════════════════════ */
static void ble_ai_cb(ble_serial_tran_event_t event, uint8_t *param)
{
    switch (event)
    {
    case BLE_SERIAL_TRAN_OPEN:
    {
        ble_serial_open_t *e = (ble_serial_open_t *)param;

        if (e != RT_NULL)
        {
            s_conn = e->handle;
            s_open = 1;
            rt_kprintf("ble_ai: channel OPEN conn=%d (cate=0x%02x)\n",
                       e->handle, BLE_AI_CATE_ID);
        }
        break;
    }

    case BLE_SERIAL_TRAN_DATA:
    {
        ble_serial_tran_data_t *d = (ble_serial_tran_data_t *)param;
        uint16_t n;

        if (d == RT_NULL || d->data == RT_NULL)
        {
            break;
        }

        n = d->len;
        if (n > BLE_AI_RX_MAX)
        {
            /* 超出我们自己缓冲的部分截掉，但计数按真实的记 —— 日志里要能看出来 */
            n = BLE_AI_RX_MAX;
        }

        rt_memcpy(s_rx, d->data, n);
        s_rx[n] = 0;
        s_rx_len = n;
        s_last_seq++;
        s_rx_pkts++;
        s_rx_bytes += d->len;

        /* 打日志可以（现有代码也这么干），但要节制：正文截到 64 字符 */
        rt_kprintf("ble_ai: RX#%u %d bytes: \"%.*s%s\"\n",
                   (unsigned)s_last_seq, d->len,
                   (int)((n > BLE_AI_LOG_MAX) ? BLE_AI_LOG_MAX : n), s_rx,
                   (n > BLE_AI_LOG_MAX) ? "..." : "");

        /* ★ 唤醒工作线程去处理。release 是非阻塞的，回调因此能立刻返回。 */
        rt_sem_release(&s_rx_sem);
        break;
    }

    case BLE_SERIAL_TRAN_CLOSE:
    {
        ble_serial_close_t *e = (ble_serial_close_t *)param;

        s_open = 0;
        s_conn = 0xFF;
        rt_kprintf("ble_ai: channel CLOSE conn=%d\n",
                   (e != RT_NULL) ? e->handle : -1);
        break;
    }

    case BLE_SERIAL_TRAN_ERROR:
    {
        ble_serial_tran_error_t *e = (ble_serial_tran_error_t *)param;

        s_tx_err++;
        rt_kprintf("ble_ai: ERROR code=%d cate=0x%02x\n",
                   (e != RT_NULL) ? e->error : -1,
                   (e != RT_NULL) ? e->cate_id : 0);
        break;
    }

    default:
        /* BLE_SERIAL_TRAN_SEND_AVAILABLE 走不到这里 ——
         * 服务发这个事件时传的 data 是 NULL，而 ble_serial_callback_event_notify()
         * 开头就有 `if (!data) return;`，所以它到不了回调。不影响我们（我们不等它）。 */
        break;
    }
}

/* ⚠ 这个宏展开后定义一个放进 "SerialTranExport" section 的静态常量。
 *   服务在 init 时把整段 section 当回调表读走。必须放在文件作用域。 */
BLE_SERIAL_TRAN_EXPORT(BLE_AI_CATE_ID, ble_ai_cb);

/* ══════════════════════════════════════════════════════════════════════
 *  ★★ 阶段二的落点就在这个函数里
 *
 *  现在它做的是"回声"：把手表收到的东西原样回一句。
 *  这不是耍花样的调试代码，是**回答器的占位实现** ——
 *  有它在，只跑电脑端一个脚本就能验证双向通道；
 *  阶段二把这里换成"把问题转给电脑、等它调完大模型把答案送回来"，
 *  上层（回调、命令、缓冲）一行都不用动。
 *
 *  为什么要快照到 s_last 再处理：见 s_rx 的注释。
 * ══════════════════════════════════════════════════════════════════════ */
static void ble_ai_thread_entry(void *parameter)
{
    (void)parameter;

    for (;;)
    {
        if (rt_sem_take(&s_rx_sem, RT_WAITING_FOREVER) != RT_EOK)
        {
            continue;
        }

        /* 先快照。之后 s_rx 再被下一次接收覆盖也不影响我们。 */
        rt_memcpy(s_last, s_rx, s_rx_len + 1);
        s_last_len = s_rx_len;

        if (!s_echo)
        {
            continue;
        }

        if (!ble_ai_is_up())
        {
            /* 刚连上就断之类的情况。不必刷屏，静默丢掉。 */
            continue;
        }

        {
            char reply[BLE_AI_TX_MAX];

            rt_snprintf(reply, sizeof(reply), "[watch] got %u bytes: %s",
                        (unsigned)s_last_len, s_last);

            /* ─── 阶段二：把上面这两行换成「调大模型」───
             * 打印日志可以留着，方便对着控制台看流程。 */
            ble_ai_send(reply);
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════
 *  对外接口
 * ══════════════════════════════════════════════════════════════════════ */

void ble_ai_serial_init(void)
{
    if (s_inited)
    {
        return;
    }
    s_inited = 1;

    /* 先把工作线程和信号量立起来，再注册服务 ——
     * 反过来的话，服务一注册完对端就可能连上发数据，那时信号量还不存在。 */
    if (rt_sem_init(&s_rx_sem, "ble_ai", 0, RT_IPC_FLAG_FIFO) != RT_EOK)
    {
        rt_kprintf("ble_ai: sem init failed, echo disabled\n");
    }
    else
    {
        s_worker = rt_thread_create("ble_ai", ble_ai_thread_entry, RT_NULL,
                                    BLE_AI_THREAD_STACK, BLE_AI_THREAD_PRIO, 10);
        if (s_worker != RT_NULL)
        {
            rt_thread_startup(s_worker);
        }
        else
        {
            rt_kprintf("ble_ai: thread create failed, echo disabled\n");
        }
    }

    /* 注册 GATT 服务。会读 SerialTranExport section 建回调表，
     * 所以上面那个 BLE_SERIAL_TRAN_EXPORT 必须已经链接进来了。 */
    ble_serial_tran_init();

    rt_kprintf("ble_ai: serial-transmission service requested "
               "(cate=0x%02x, adv name \"HS-Watch\")\n", BLE_AI_CATE_ID);
}

int ble_ai_send(const char *text)
{
    ble_serial_tran_data_t d;
    int ret;

    if (text == RT_NULL || text[0] == 0)
    {
        return -1;
    }

    if (!s_open || s_conn == 0xFF)
    {
        rt_kprintf("ble_ai: not connected, cannot send "
                   "(connect from PC first; the watch advertises as \"HS-Watch\")\n");
        return -1;
    }

    d.handle  = s_conn;
    d.cate_id = BLE_AI_CATE_ID;
    d.len     = (uint16_t)rt_strlen(text);
    d.data    = (uint8_t *)text;

    /* 服务内部自动组头 + 按 MTU 分包，长文本也能发。
     * 返回值是写进去的字节数（含包头的），失败返回 0 或负的错误码。 */
    ret = ble_serial_tran_send_data(&d);

    if (ret > 0)
    {
        s_tx_pkts++;
        rt_kprintf("ble_ai: TX %d bytes (packet %d bytes incl. header)\n",
                   (int)d.len, ret);
    }
    else
    {
        s_tx_err++;
        rt_kprintf("ble_ai: TX FAILED ret=%d\n", ret);
    }

    return (ret > 0) ? (int)d.len : ret;
}

int ble_ai_is_up(void)
{
    return (s_open && s_conn != 0xFF) ? 1 : 0;
}

void ble_ai_set_echo(int on)
{
    s_echo = on ? 1 : 0;
    rt_kprintf("ble_ai: echo %s\n", s_echo ? "ON" : "OFF");
}

/* ══════════════════════════════════════════════════════════════════════
 *  串口命令（验证用）
 * ══════════════════════════════════════════════════════════════════════ */

/* ai_send <text>  —— 把参数用空格拼起来发出去。
 * 不拼的话 `ai_send hello world` 只会发 "hello"，后面两截被当成多余参数丢掉。
 *
 * ⚠ 函数名必须**和命令名一模一样**：MSH_CMD_EXPORT(name, desc) 展开成
 *   FINSH_FUNCTION_EXPORT_CMD(name, name, desc)，第二参也当函数名用。
 *   写成 cmd_ai_send 会报 "'ai_send' undeclared here"，第一次就踩了这个。 */
static int ai_send(int argc, char **argv)
{
    char buf[BLE_AI_TX_MAX];
    int  n = 0;
    int  i;

    if (argc < 2)
    {
        rt_kprintf("usage: ai_send <text>   (multiple words are joined with spaces)\n");
        return -1;
    }

    buf[0] = 0;
    for (i = 1; i < argc; i++)
    {
        n += rt_snprintf(buf + n, sizeof(buf) - n, "%s%s",
                         (i > 1) ? " " : "", argv[i]);
        if (n >= (int)sizeof(buf) - 1)
        {
            n = sizeof(buf) - 1;
            rt_kprintf("ai_send: text too long, truncated to %d bytes\n", n);
            break;
        }
    }

    return ble_ai_send(buf);
}
MSH_CMD_EXPORT(ai_send, ai_send <text> : send a line of text over BLE to the peer);

static int ai_info(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    rt_kprintf("ble_ai: service_inited=%d channel_open=%d conn=%d up=%d echo=%d\n",
               s_inited, s_open, (s_conn == 0xFF) ? -1 : s_conn,
               ble_ai_is_up(), s_echo);
    rt_kprintf("ble_ai: rx_pkts=%u rx_bytes=%u tx_pkts=%u tx_err=%u\n",
               (unsigned)s_rx_pkts, (unsigned)s_rx_bytes,
               (unsigned)s_tx_pkts, (unsigned)s_tx_err);
    rt_kprintf("ble_ai: cate_id=0x%02x rx_max=%d last_seq=%u last_len=%u\n",
               BLE_AI_CATE_ID, BLE_AI_RX_MAX,
               (unsigned)s_last_seq, (unsigned)s_last_len);

    return 0;
}
MSH_CMD_EXPORT(ai_info, ai_info : dump BLE data-channel state and counters);

static int ai_recv(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (s_last_seq == 0)
    {
        rt_kprintf("ble_ai: nothing received yet\n");
        return 0;
    }

    rt_kprintf("ble_ai: last RX #%u, %u bytes: \"%s\"\n",
               (unsigned)s_last_seq, (unsigned)s_last_len, s_last);

    /* 顺便把字节值也打出来，中文/二进制内容用肉眼对时才看得清 */
    {
        uint16_t i;

        rt_kprintf("ble_ai: raw:");
        for (i = 0; i < s_last_len; i++)
        {
            rt_kprintf(" %02x", (unsigned char)s_last[i]);
        }
        rt_kprintf("\n");
    }

    return 0;
}
MSH_CMD_EXPORT(ai_recv, ai_recv : show the last line received over BLE);

/* ai_echo [0|1] —— 回声开关。不带参数只看当前值。
 * 阶段二手表自己会回答之后，这个开关就没用了（或者可以直接关掉）。 */
static int ai_echo(int argc, char **argv)
{
    if (argc < 2)
    {
        rt_kprintf("ble_ai: echo is %s.  usage: ai_echo 0|1\n", s_echo ? "ON" : "OFF");
        return 0;
    }

    if (argv[1][0] == '0')
    {
        ble_ai_set_echo(0);
    }
    else if (argv[1][0] == '1')
    {
        ble_ai_set_echo(1);
    }
    else
    {
        rt_kprintf("usage: ai_echo 0|1\n");
    }

    return 0;
}
MSH_CMD_EXPORT(ai_echo, ai_echo 0|1 : turn the loopback echo on/off);

#else  /* !BSP_BLE_SERIAL_TRANSMISSION */

/* 宏没开时给空实现 —— 这样 ble_app.c 可以无条件调用，不会链接失败。
 * （正常路径下这个宏是开的；留这一手是为了"改了 proj.conf 忘了重编"时
 *   报错能停在明确的地方，而不是莫名其妙地 undefined reference。） */
void ble_ai_serial_init(void)
{
    rt_kprintf("ble_ai: DISABLED (CONFIG_BSP_BLE_SERIAL_TRANSMISSION is not set)\n");
}

int ble_ai_send(const char *text)
{
    (void)text;
    return -1;
}

int ble_ai_is_up(void)
{
    return 0;
}

void ble_ai_set_echo(int on)
{
    (void)on;
}

#endif /* BSP_BLE_SERIAL_TRANSMISSION */
