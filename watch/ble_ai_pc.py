# -*- coding: utf-8 -*-
r"""
ble_ai_pc.py —— 电脑端：通过蓝牙和黄山派手表（HS-Watch）互传文字

这块手表**没有 WiFi**，要联网只能借电脑/手机的网。这个脚本就是把电脑
当"网卡 + 大脑"：手表发来的问题由它转给大模型，答案再送回手表。

════════════════════════════════════════════════════════════════════════
  怎么用
════════════════════════════════════════════════════════════════════════
  阶段二（电脑当大脑）：
    python ble_ai_pc.py serve       ← ★ 主模式：手表问什么，电脑答什么
    python ble_ai_pc.py ask 你好     ← 不连手表，先验证 key/网络/模型通不通

  阶段一（验证通道）：
    python ble_ai_pc.py test        ← 自检，会说 PASS / FAIL
    python ble_ai_pc.py chat        ← 打字发给手表，收到就打印
    python ble_ai_pc.py scan        ← 只扫描，确认电脑能看见表
    python ble_ai_pc.py probe       ← 连上后把 GATT 服务树全打出来（排查用）
    python ble_ai_pc.py send 你好    ← 发一条就走
    python ble_ai_pc.py listen      ← 只收不发，看手表主动发来的东西

  大脑的配置在 ai_brain_config.json（哪个模型、key、提示词）。
  现在是 "offline" 本地假大脑，照样能把整条链路跑通。

  前跑须知：
  · 手表开机后要等 **30~60 秒**才会开始广播（协议栈起来 + 射频校准）。
  · 手表广播名是 "HS-Watch"。
  · 电脑蓝牙要是开着的（设置 → 蓝牙）。
  · 手表侧串口命令：ai_send <文字> 提问 / ai_info 看状态 / ai_recv 看收到的 /
    ai_echo 0|1 关掉回声。

════════════════════════════════════════════════════════════════════════
  协议（和 ble_ai.c 那份是同一份，改一边要改两边）
════════════════════════════════════════════════════════════════════════
    | 1B CateID | 1B Flag | 2B Length | N B Data |

    Flag = 0x00 完整包  : 头 4 字节，Data 从第 5 字节起
    Flag = 0x01 首包    : 头 4 字节，其中 Length 是**整条消息的总长**
    Flag = 0x02 续包    : 头只有 2 字节！ Data 从第 3 字节起
    Flag = 0x03 末包    : 同续包

  我们用的 CateID = 0x10（官方留给第三方的 BLE_CUSTOMIZE_CATEID）。

  ⚠ 一个反直觉点：完整包（Flag=0）里那 2 字节 Length 服务端**根本没读**，
    它直接用 "写进来的总长 - 4"。所以照写就行，值不重要。
    但首包（Flag=1）里的 Length 是**必须**的，服务靠它分配接收缓冲。

════════════════════════════════════════════════════════════════════════
  为什么这里不写死 UUID
════════════════════════════════════════════════════════════════════════
SDK 里那个 16 字节的 UUID 数组是 "sifli_sd" 的 ASCII：

    73 69 66 6c 69 5f 73 64 | 00 00 | 00 00 00 00 00 00      ← 服务
    73 69 66 6c 69 5f 73 64 | 00 02 | 00 00 00 00 00 00      ← data 特征值

但它**在空口上到底是原序还是反序**，光看源码定不了（BLE 的 128-bit UUID
在线路上是小端，但 SiFli 这一层有没有再翻一次，只有扫一次才知道）。
赌错了脚本就找不到服务，而且报错信息很难看出是这个原因。

所以这里改成**按能力认**：这个服务只有两个特征值 ——
    configure  可读可写（服务端其实不用它）
    data       可读 + 可写 + 可通知   ← 只有它同时具备「可写」和「可通知」
所以"同时能写又能通知的那个"就是 data，跟 UUID 怎么写无关。
UUID 匹配只当加分项，两边写法（原序/反序）都认。

★ 2026-09-23 实测（MTU 527 协商成功）之后，答案有了：**空口上是反序的**。
  手表上报的是
      00000000-0000-0200-6473-5f696c666973
  把 `6473` 翻回来是 `7364`（"sd"），`5f696c666973` 翻回来是 `7369666c695f`
  （"sifli_"）—— 也就是上面那张表的**小端（bytes_le）解释**。
  三个 UUID 的规范写法：
      服务      6c666973-5f69-6473-0000-000000000000
      configure 6c666973-5f69-6473-0001-000000000000
      data      6c666973-5f69-6473-0002-000000000000
  （按能力识别仍然保留 —— 它不依赖这个结论，换个固件/换个芯片也不会失效。）
"""

import argparse
import asyncio
import os
import struct
import sys
import threading
import time

# ── 控制台编码：Windows 下中文别乱码 ──────────────────────────────────
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

try:
    from bleak import BleakScanner, BleakClient
except ImportError:
    print("缺少 bleak 库。装一下：")
    print(r'  "C:\Users\18058\.workbuddy\binaries\python\envs\default\Scripts\python.exe" -m pip install bleak')
    sys.exit(1)


DEV_NAME = "HS-Watch"
CATE_AI = 0x10

# 手表侧回声的前缀（见 ble_ai.c 的 ble_ai_thread_entry）。
#
# 手表固件里有个「回声」：收到什么都回一句 "[watch] got N bytes: ..."。
# 那是阶段一的占位回答，用来在只有电脑端一个脚本时验证双向通道。
#
# 现在电脑端要当大脑了，两边都答会打架。两个办法：
#   · 在手表串口执行 `ai_echo 0` 把回声关掉（干净）
#   · 或者就让 echo 开着，这里把这个前缀的消息**当成回声忽略掉**（省事）
# 下面这个常量实现的是第二种 —— 它对两种状态都成立，不用你操心开关。
ECHO_PREFIX = "[watch] got "

# 连上之后电脑端主动发的第一句。
#
# ★ 为什么需要它：连上 ≠ 能收数据。
#   实测踩到过 —— 连接建立、手表侧 channel_open=1 / up=1，手表也打印了
#   「TX 17 bytes」「tx_err=0」，看着一切正常，**但电脑一个字节都没收到**。
#   原因是手表侧要过几十秒才处理完 CCCD 订阅（它的日志里那行 `cccd enable 1`），
#   在那之前手表发出的数据会被静默丢弃。
#   所以：电脑端订阅完成后先发一句，手表收到它才算链路真的双向可用
#   （手表侧 ai_info 的 rx_pkts≥1）。
READY_MSG = "[pc] ready"

# UUID 的两种可能写法。只用来给候选加权重，不用来"必须匹配"。
UUID_HINTS = ("7369666c", "6c666973")

VERBOSE = False

# 设了环境变量 BLE_AI_LOG=<路径> 的话，输出同时写一份到文件。
# 为什么需要：serve 模式是长期跑的后台进程，被重定向/被中断时
# 管道缓冲里的东西会丢 —— 排查"它到底走到哪一步挂的"就抓瞎了。
#
# ⚠ 别把 BLE_AI_LOG 和 stdout 重定向指向**同一个文件**：
#   这里是追加写，而 shell 的 `>` 不是，两个写入者各写各的偏移量，
#   会互相覆盖出半截行，日志看着像跑了两遍，很难查。
#   真撞上了就在下面这行自动关掉文件那份，只留 stdout。
_LOG_FP = os.environ.get("BLE_AI_LOG")
try:
    if _LOG_FP and os.path.exists(_LOG_FP):
        _a = os.fstat(sys.stdout.fileno())
        _b = os.stat(_LOG_FP)
        if (_a.st_dev, _a.st_ino) != (0, 0) and (_a.st_dev, _a.st_ino) == (_b.st_dev, _b.st_ino):
            _LOG_FP = None
except Exception:
    pass


def log(*a):
    s = " ".join(str(x) for x in a)
    print(s, flush=True)
    if _LOG_FP:
        try:
            with open(_LOG_FP, "a", encoding="utf-8") as f:
                f.write(s + "\n")
        except Exception:
            pass


# ══════════════════════════════════════════════════════════════════════
#  组包 / 拆包
# ══════════════════════════════════════════════════════════════════════

def build_packets(cate, payload, mtu):
    """把一整条 payload 切成若干包，规则见文件头。

    mtu 是协商后的 ATT MTU。一次 write 最多能带 mtu-3 字节（ATT 头占 3）。
    """
    max_write = mtu - 3

    # 装得下 -> 一个完整包
    if 4 + len(payload) <= max_write:
        return [bytes([cate, 0x00]) + struct.pack("<H", len(payload)) + payload]

    # 装不下 -> 分片。首包多 2 字节（总长），续包/末包只有 cate+flag。
    first_cap = max_write - 4
    cont_cap = max_write - 2

    # 能走到这里说明 len(payload) > max_write - 4 == first_cap，
    # 所以首包一定吃不完，后面至少还有一个包。
    pkts = []
    total = len(payload)
    off = 0

    chunk = payload[off:off + first_cap]
    pkts.append(bytes([cate, 0x01]) + struct.pack("<H", total) + chunk)
    off += len(chunk)

    while off < total:
        remain = total - off
        if remain <= cont_cap:
            pkts.append(bytes([cate, 0x03]) + payload[off:off + remain])
            off += remain
        else:
            pkts.append(bytes([cate, 0x02]) + payload[off:off + cont_cap])
            off += cont_cap

    return pkts


class Assembler:
    """把对方发来的包拼回一整条。规则和 ble_ai.c / 服务里那一套一致。"""

    def __init__(self):
        self.buf = None
        self.total = 0
        self.cate = 0

    def feed(self, b):
        """返回 (cate, bytes) 表示拼完整了；返回 None 表示还要等后续包。"""
        if len(b) < 2:
            return None

        cate, flag = b[0], b[1]

        if flag == 0x00:
            if len(b) < 4:
                return None
            n = struct.unpack("<H", b[2:4])[0]
            data = b[4:4 + n]
            # 服务端的做法是「总长 - 4」，不看头里的 Length。
            # 如果对端把头写错了，以实际长度为准，别丢数据。
            if n != len(b) - 4:
                data = b[4:]
            return cate, data

        if flag == 0x01:
            if len(b) < 4:
                return None
            self.total = struct.unpack("<H", b[2:4])[0]
            self.cate = cate
            self.buf = bytearray(b[4:])
            if len(self.buf) >= self.total:
                d = bytes(self.buf[:self.total])
                self.buf = None
                return self.cate, d
            return None

        if flag in (0x02, 0x03):
            if self.buf is None:
                if VERBOSE:
                    log("  [警告] 收到续包但没有首包，丢弃")
                return None
            self.buf += b[2:]
            if flag == 0x03:
                if len(self.buf) < self.total and VERBOSE:
                    log("  [警告] 末包到了但长度不够：%d < %d" % (len(self.buf), self.total))
                d = bytes(self.buf[:self.total])
                self.buf = None
                return self.cate, d
            return None

        if VERBOSE:
            log("  [警告] 未知 Flag = 0x%02x" % flag)
        return None


# ══════════════════════════════════════════════════════════════════════
#  扫描 / 连接
# ══════════════════════════════════════════════════════════════════════

async def scan_devices(timeout=8.0, quiet=False):
    if not quiet:
        log("扫描 %.0f 秒…（手表开机后要等 30~60 秒才开始广播）" % timeout)

    res = await BleakScanner.discover(timeout=timeout, return_adv=True)

    items = []
    if isinstance(res, dict):
        for _addr, pair in res.items():
            try:
                dev, adv = pair
            except Exception:
                continue
            items.append((dev, adv))
    else:
        for d in res:
            items.append((d, None))

    found = None
    if not quiet:
        if not items:
            log("  一个设备都没扫到。检查：电脑蓝牙开着吗？手表广播了吗？")
        for dev, adv in sorted(items, key=lambda x: ((x[0].name or "~"))):
            nm = dev.name or (getattr(adv, "local_name", None) if adv else None) or "(无名)"
            rssi = getattr(adv, "rssi", None)
            if rssi is None:
                rssi = getattr(dev, "rssi", "?")
            mark = ""
            if nm == DEV_NAME:
                mark = "   <<<<<< 就是它"
                found = dev
            log("  %-30s %-20s rssi=%s%s" % (nm, dev.address, rssi, mark))
        log("")

    if found is None:
        for dev, adv in items:
            nm = dev.name or (getattr(adv, "local_name", None) if adv else None)
            if nm == DEV_NAME:
                found = dev
                break

    return found


def find_data_char(client):
    """找出「既能写、又能通知」的那个特征值。见文件头的解释。"""
    hits = []
    for svc in client.services:
        su = str(svc.uuid).lower().replace("-", "")
        svc_bonus = 3 if any(h in su for h in UUID_HINTS) else 0
        for ch in svc.characteristics:
            props = set(ch.properties)
            has_w = bool(props & {"write", "write-without-response"})
            has_n = bool(props & {"notify", "indicate"})
            if has_w and has_n:
                cu = str(ch.uuid).lower().replace("-", "")
                bonus = 3 if any(h in cu for h in UUID_HINTS) else 0
                hits.append((svc_bonus + bonus, svc, ch, props))

    if not hits:
        return None, None, set()

    hits.sort(key=lambda x: -x[0])
    _score, svc, ch, props = hits[0]
    return svc, ch, props


def dump_gatt(client):
    log("GATT 服务树：")
    for svc in client.services:
        log("  服务 %s" % svc.uuid)
        if svc.description:
            log("       (%s)" % svc.description)
        for ch in svc.characteristics:
            log("      特征值 %s  [%s]" % (ch.uuid, ", ".join(ch.properties)))
            for d in ch.descriptors:
                log("          描述符 %s" % d.uuid)
    log("")


async def open_channel(dev, on_disconnect=None):
    """连上设备，找到 data 特征值。（订阅通知由调用方做）

    ⚠ 断连回调**必须**在构造时传进来。
      bleak 没有 `client.set_disconnected_callback()` 这个方法 ——
      写成那样会 AttributeError，而且是在**连上之后**才炸，
      看着像"能连上但马上就断"，极难定位。实测踩过一次。"""
    client = BleakClient(dev, disconnected_callback=on_disconnect)
    await client.connect()

    if not client.is_connected:
        raise RuntimeError("连接失败")

    mtu = getattr(client, "mtu_size", 0) or 0
    log("已连接：%s    MTU=%d（单包最多带 %d 字节）" % (dev.address, mtu, max(mtu - 3, 20)))

    svc, ch, props = find_data_char(client)
    if ch is None:
        log("！！没找到「既能写又能通知」的特征值。把服务树打出来看看：")
        dump_gatt(client)
        raise RuntimeError("找不到 data 特征值")

    log("data 特征值：%s  [%s]" % (ch.uuid, ", ".join(ch.properties)))
    if VERBOSE:
        log("  所属服务：%s" % svc.uuid)

    return client, ch


# ══════════════════════════════════════════════════════════════════════
#  收发
# ══════════════════════════════════════════════════════════════════════

async def send_text(client, ch, text, mtu=None):
    if mtu is None:
        mtu = getattr(client, "mtu_size", 0) or 23
        if mtu < 23:
            mtu = 23

    payload = text.encode("utf-8")
    pkts = build_packets(CATE_AI, payload, mtu)

    for i, p in enumerate(pkts):
        try:
            await client.write_gatt_char(ch, p, response=False)
        except Exception:
            # 有些协议栈对 write-without-response 支持不好，退回带响应的写
            await client.write_gatt_char(ch, p, response=True)
        if len(pkts) > 1:
            await asyncio.sleep(0.03)   # 别把发送队列灌满

    return len(pkts), len(payload)


# ══════════════════════════════════════════════════════════════════════
#  各种模式
# ══════════════════════════════════════════════════════════════════════

async def mode_scan(args):
    dev = await scan_devices(args.timeout)
    if dev is None:
        log("没找到 %s。" % DEV_NAME)
        return 2
    log("找到了：%s  %s" % (DEV_NAME, dev.address))
    return 0


async def mode_probe(args):
    dev = await scan_devices(args.timeout)
    if dev is None:
        log("没找到 %s。" % DEV_NAME)
        return 2

    client = BleakClient(dev)
    await client.connect()
    log("已连接：%s  MTU=%s" % (dev.address, getattr(client, "mtu_size", "?")))
    log("")
    dump_gatt(client)

    svc, ch, props = find_data_char(client)
    if ch is None:
        log("按能力没认出 data 特征值。")
    else:
        log("认出 data 特征值：%s  [%s]" % (ch.uuid, ", ".join(ch.properties)))
        log("父服务：%s" % svc.uuid)

    await client.disconnect()
    return 0


async def mode_send(args):
    dev = await scan_devices(args.timeout)
    if dev is None:
        log("没找到 %s。" % DEV_NAME)
        return 2

    client, ch = await open_channel(dev)
    n, ln = await send_text(client, ch, args.text)
    log("已发送 %d 字节（切成 %d 个包）" % (ln, n))
    await asyncio.sleep(1.0)
    await client.disconnect()
    return 0


async def mode_listen(args):
    dev = await scan_devices(args.timeout)
    if dev is None:
        log("没找到 %s。" % DEV_NAME)
        return 2

    client, ch = await open_channel(dev)
    asm = Assembler()

    def on_notify(_sender, data: bytearray):
        raw = bytes(data)
        if VERBOSE:
            log("  [收 %d 字节] %s" % (len(raw), raw.hex()))
        out = asm.feed(raw)
        if out is None:
            return
        cate, payload = out
        log("[手表 cate=0x%02x] %s" % (cate, payload.decode("utf-8", errors="replace")))

    await client.start_notify(ch, on_notify)
    log("已订阅通知。%s 秒内收到的东西都会打出来（Ctrl+C 结束）…" % args.duration)

    t0 = time.time()
    try:
        while time.time() - t0 < args.duration:
            await asyncio.sleep(0.2)
            if not client.is_connected:
                log("连接断了。")
                break
    except (KeyboardInterrupt, asyncio.CancelledError):
        pass

    try:
        await client.disconnect()
    except Exception:
        pass
    return 0


# ── 自检：一条命令说 PASS/FAIL ────────────────────────────────────────

async def mode_test(args):
    log("═" * 66)
    log("  手表 <-> 电脑 蓝牙通道自检")
    log("═" * 66)
    log("")

    dev = await scan_devices(args.timeout)
    if dev is None:
        log("")
        log("结果：FAIL —— 没扫到 %s。" % DEV_NAME)
        log("  · 手表开机后要等 30~60 秒才开始广播，等一会儿再试")
        log("  · 电脑蓝牙开着吗")
        log("  · 手表是不是已经被别的设备连上了（比如手机）？同一时刻只能连一个")
        return 2

    client, ch = await open_channel(dev)
    asm = Assembler()

    got = []
    raw_log = []

    def on_notify(_sender, data: bytearray):
        raw = bytes(data)
        raw_log.append(raw)
        if VERBOSE:
            log("  [收 %d 字节] %s" % (len(raw), raw.hex()))
        out = asm.feed(raw)
        if out is None:
            return
        cate, payload = out
        got.append(payload.decode("utf-8", errors="replace"))

    await client.start_notify(ch, on_notify)
    log("")

    mtu = getattr(client, "mtu_size", 0) or 23
    if mtu < 23:
        mtu = 23

    # 四个用例，从易到难。第四个会**真的触发分包**（长度 > mtu-4）。
    #
    # ⚠ 别以为"长文本"就一定分包：本机 Windows 把 MTU 协商到了 527，
    #   单包能带 524 字节 —— 137 字节的中文照样一个包发完。
    #   要逼出分包，长度必须超过 mtu-4。所以第 4 个用例的长度是**按 MTU 算出来**的。
    over = max(mtu - 4 + 80, 600)     # 稳稳超过单包上限
    cases = [
        ("短英文（一条完整包）", "hello-from-pc", None),
        ("中文（UTF-8 多字节）", "你好，手表！我是电脑。", None),
        ("长文本（还没到分包线）", "这是一条比较长的测试文本，"
                                 "用来验证较长的内容不会丢："
                                 "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                 "abcdefghijklmnopqrstuvwxyz", None),
        # 回声在手表侧被 BLE_AI_TX_MAX(256) 截断，所以这里不能整句比对，
        # 改为比对"手表说它收到了多少字节"这个前缀 —— 那才是分包对不对的判据。
        ("超 MTU（强制分包）", "x" * over, "got %d bytes" % over),
    ]

    n_pass = 0
    for i, (title, text, expect) in enumerate(cases, 1):
        got.clear()
        raw_log.clear()
        nbytes = len(text.encode("utf-8"))
        npkt, _ln = await send_text(client, ch, text, mtu)
        log("用例 %d：%s" % (i, title))
        log("  发出 %d 字节，切成 %d 个包%s"
            % (nbytes, npkt, "  ← 分包" if npkt > 1 else ""))
        if npkt == 1 and nbytes + 4 > mtu - 3:
            log("  [！] 说好要分包却没分 —— 组包逻辑有问题")

        want = expect if expect else text

        # 等回声（最多 8 秒，长文本要多给点时间）
        t0 = time.time()
        hit = None
        while time.time() - t0 < 8.0:
            await asyncio.sleep(0.1)
            for g in got:
                if want in g:
                    hit = g
                    break
            if hit:
                break
            if not client.is_connected:
                break

        if hit:
            log("  收到回声 %d 个通知 / %d 字节"
                % (len(raw_log), sum(len(r) for r in raw_log)))
            if expect:
                log("  （回声原文被手表截到 256 字节，这里只比对前缀 %r）" % expect)
            log("  >> 通过")
            n_pass += 1
        else:
            log("  等 8 秒没等到回声。")
            if got:
                log("  倒是收到了这些：%r" % (got[:3],))
            elif raw_log:
                log("  收到了 %d 个通知但拼不出完整消息，第一个是：%s"
                    % (len(raw_log), raw_log[0].hex()))
            else:
                log("  一个字都没收到 —— 说明电脑发出的没到手表，"
                    "或者手表的回声没发回来。")
            log("  >> 失败")
        log("")

    try:
        await client.disconnect()
    except Exception:
        pass

    log("═" * 66)
    log("  结果：%d / %d 通过" % (n_pass, len(cases)))
    if n_pass == len(cases):
        log("  >>> PASS —— 双向通道通了。")
        log("      手表把电脑发的文字收全了，并且回了话。")
        log("      下一步：把电脑端接上大模型，手表就能真的回答问题。")
    elif n_pass:
        log("  >>> 部分通过 —— 通的，但某个尺寸/编码上有问题，看上面哪条挂了。")
    else:
        log("  >>> FAIL —— 一条都没通。")
        log("      排查顺序：")
        log("        1. 手表串口控制台有没有打 'ble_ai: channel OPEN'？")
        log("           没有 -> 连接本身没建立（可能连上又被抢走了）")
        log("        2. 有没有打 'ble_ai: RX#1 ...'？")
        log("           没有 -> 电脑发的包手表没收到（UUID/特征值对不对？跑 probe 看）")
        log("        3. 有没有打 'ble_ai: TX ... bytes'？")
        log("           没有 -> 回声没发出去（通道状态不对）")
        log("      用 python ble_ai_pc.py probe 把服务树打出来发我。")
    log("═" * 66)
    return 0 if n_pass == len(cases) else 1


# ── 聊天模式 ──────────────────────────────────────────────────────────

def start_stdin_reader(loop, q):
    def reader():
        while True:
            try:
                line = sys.stdin.readline()
            except Exception:
                break
            if not line:
                loop.call_soon_threadsafe(q.put_nowait, None)
                break
            loop.call_soon_threadsafe(q.put_nowait, line.rstrip("\r\n"))
    t = threading.Thread(target=reader, daemon=True)
    t.start()
    return t


async def mode_chat(args):
    loop = asyncio.get_running_loop()
    q = asyncio.Queue()
    start_stdin_reader(loop, q)

    log("═" * 66)
    log("  聊天模式")
    log("  打字回车 -> 发给手表；手表发来的会打印出来")
    log("  /quit 退出   /info 看状态   /probe 打服务树")
    log("═" * 66)
    log("")

    while True:
        dev = await scan_devices(args.timeout, quiet=True)
        if dev is None:
            log("没扫到 %s，5 秒后重试（Ctrl+C 退出）…" % DEV_NAME)
            await asyncio.sleep(5)
            continue

        disconnected = asyncio.Event()

        def on_disc(_c):
            disconnected.set()

        try:
            client, ch = await open_channel(dev, on_disconnect=on_disc)
        except Exception as e:
            log("连接失败：%s。5 秒后重试…" % e)
            await asyncio.sleep(5)
            continue

        asm = Assembler()

        def on_notify(_sender, data: bytearray):
            raw = bytes(data)
            if VERBOSE:
                log("  [收 %d 字节] %s" % (len(raw), raw.hex()))
            out = asm.feed(raw)
            if out is None:
                return
            _cate, payload = out
            log("[手表] %s" % payload.decode("utf-8", errors="replace"))

        await client.start_notify(ch, on_notify)
        log("")
        log("可以打字了（Ctrl+C 退出）：")
        log("")

        # 主循环：等输入 or 断线
        #
        # 用 wait_for + 超时轮询，而不是「两个 task 竞速 + cancel」——
        # 后者在取消未完成的 task 时会漏 "Task was destroyed but it is pending!"，
        # 而且两个同时完成时还会把已经读到的那行丢掉。
        while True:
            if disconnected.is_set():
                log("")
                log("[系统] 连接断了（手表息屏/走远/被别的设备连了）。自动重连…")
                break

            try:
                line = await asyncio.wait_for(q.get(), timeout=0.5)
            except asyncio.TimeoutError:
                continue

            if line is None:
                log("")
                log("输入结束，退出。")
                try:
                    await client.disconnect()
                except Exception:
                    pass
                return 0

            if not line.strip():
                continue

            if line in ("/quit", "/q", "/exit"):
                log("退出。")
                try:
                    await client.disconnect()
                except Exception:
                    pass
                return 0

            if line == "/info":
                log("  已连接=%s  MTU=%s  特征值=%s"
                    % (client.is_connected, getattr(client, "mtu_size", "?"), ch.uuid))
                continue

            if line == "/probe":
                dump_gatt(client)
                continue

            try:
                n, ln = await send_text(client, ch, line)
                if VERBOSE or n > 1:
                    log("  (发出 %d 字节 / %d 包)" % (ln, n))
            except Exception as e:
                log("  发送失败：%s" % e)

        try:
            await client.disconnect()
        except Exception:
            pass
        await asyncio.sleep(3)


# ══════════════════════════════════════════════════════════════════════
#  阶段二：电脑当「大脑」
# ══════════════════════════════════════════════════════════════════════

def _load_brain():
    """把 ai_brain.py 里的 Brain 拿过来。
    import 放在函数里 —— 这样只跑 test/scan 时不依赖大脑那几个文件。"""
    try:
        from ai_brain import Brain
    except Exception as e:
        log("拿不到大脑模块（ai_brain.py）：%r" % (e,))
        log("  它应该和本脚本放在同一个目录。")
        return None
    return Brain()


async def mode_ask(args):
    """不连手表，直接问一句 —— 确认 key / 网络 / 模型通不通。"""
    brain = _load_brain()
    if brain is None:
        return 2

    log("═" * 66)
    log("  大脑自测（不连手表）")
    log("═" * 66)
    log(brain.describe())
    log("")

    q = args.text or "你好，用一句话介绍你自己"
    log("[问] " + q)
    t0 = time.time()
    a = await asyncio.to_thread(brain.ask, q)
    log("[答] " + a)
    log("")
    log("耗时 %.1f 秒" % (time.time() - t0))

    try:
        from ai_brain import clip_reply, MAX_REPLY_BYTES
        nb = len(a.encode("utf-8"))
        clipped = clip_reply(a)
        log("长度 %d 字节（手表接收缓冲上限 %d）" % (nb, 1024))
        if clipped != a:
            log("⚠ 太长，发给手表时会截断成：")
            log("  " + clipped)
        else:
            log("✓ 长度合适，能原样送到手表")
    except Exception:
        pass
    log("")

    if brain.last_error:
        log("⚠ 这一句是出错时兜底出来的，不是真答案。")
        log("  看上面的报错，多半是：key 没填 / 模型名不对 / 余额不足 / 网络。")
        return 1
    return 0


async def mode_serve(args):
    """★ 阶段二的主模式：连上手表，收到的每句话都交给大脑，答案自动送回。

    要真模型：改 ai_brain_config.json 里的 provider 和 api_key。
    不花钱先试：provider 保持 offline，用的是本地假大脑。
    """
    brain = _load_brain()
    if brain is None:
        return 2

    from ai_brain import clip_reply, MAX_REPLY_BYTES

    log("═" * 66)
    log("  大脑服务模式")
    log("═" * 66)
    log(brain.describe())
    if brain.provider == "offline":
        log("")
        log("  现在是【离线假大脑】—— 不联网、不花钱，能答时间/日期/算术/打招呼。")
        log("  要接真模型：打开 ai_brain_config.json，")
        log("    provider 改成 deepseek / qwen / zhipu / moonshot / hunyuan / openai")
        log("    api_key  填上你申请到的 key")
        log("  改完重新跑本命令即可。")
    log("═" * 66)
    log("")

    while True:
        dev = await scan_devices(args.timeout, quiet=True)
        if dev is None:
            log("没扫到 %s，5 秒后重试（Ctrl+C 退出）…" % DEV_NAME)
            await asyncio.sleep(5)
            continue

        disconnected = asyncio.Event()

        def on_disc(_c):
            disconnected.set()

        try:
            client, ch = await open_channel(dev, on_disconnect=on_disc)
        except Exception as e:
            log("连接失败：%s。5 秒后重试…" % e)
            await asyncio.sleep(5)
            continue

        asm = Assembler()
        q = asyncio.Queue()

        def on_notify(_sender, data: bytearray):
            raw = bytes(data)
            if VERBOSE:
                log("  [收 %d 字节] %s" % (len(raw), raw.hex()))
            out = asm.feed(raw)
            if out is None:
                return
            _cate, payload = out
            # 回调里只入队，别的都交给下面那个循环 —— 大脑要联网，
            # 在通知回调里等 30 秒会把整条 BLE 通知管线堵住。
            q.put_nowait(payload.decode("utf-8", errors="replace"))

        await client.start_notify(ch, on_notify)

        # 订阅完成后立刻发一句就绪信标（原因见文件头 READY_MSG 的注释）。
        # 手表收到它会打印 ble_ai: RX#1，那才是「可以开始发消息了」的信号。
        try:
            n_b, ln_b = await send_text(client, ch, READY_MSG)
            log("已发就绪信标 %r（%d 字节 / %d 包）" % (READY_MSG, ln_b, n_b))
        except Exception as e:
            log("发就绪信标失败：%s（不影响，只是少一个判据）" % e)

        log("")
        log("开始服务。现在谁给手表发消息，都会走一遍「大脑」再回给手表。")
        log("  · 手表侧串口：  ai_send 你好        （模拟手表发起一次提问）")
        log("  · 手表的回声会被自动忽略，不用先关 ai_echo")
        log("")

        try:
            while not disconnected.is_set():
                try:
                    text = await asyncio.wait_for(q.get(), timeout=0.5)
                except asyncio.TimeoutError:
                    continue

                if text.startswith(ECHO_PREFIX):
                    if VERBOSE:
                        log("  (忽略手表回声)")
                    continue

                log("[手表问] %s" % text)
                t0 = time.time()
                answer = await asyncio.to_thread(brain.ask, text)   # 别阻塞事件循环
                log("[大脑答] %s" % answer)
                log("          (%.1f 秒)" % (time.time() - t0))

                # ★ 必须在电脑端截，不能等手表端硬截。
                # 手表接收缓冲是 1024 字节（ble_ai.c 的 BLE_AI_RX_MAX），
                # 超了会被硬砍；一个汉字 3 字节，硬砍很可能切在字符中间，
                # 表上末尾就是一串乱码。这里按字节安全边界截，还带个省略号。
                send_me = clip_reply(answer)
                if send_me != answer:
                    log("          回答太长：%d 字节 → 截到 %d 字节再发"
                        % (len(answer.encode("utf-8")), len(send_me.encode("utf-8"))))

                try:
                    n, ln = await send_text(client, ch, send_me)
                    log("          已回送 %d 字节 / %d 包" % (ln, n))
                except Exception as e:
                    log("          回送失败：%s" % e)
                log("")
        finally:
            try:
                await client.disconnect()
            except Exception:
                pass

        if disconnected.is_set():
            log("[系统] 连接断了，3 秒后重连…")
            await asyncio.sleep(3)


# ══════════════════════════════════════════════════════════════════════

def main():
    global VERBOSE

    ap = argparse.ArgumentParser(
        description="电脑端：通过蓝牙和黄山派手表互传文字",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    ap.add_argument("mode", nargs="?", default="test",
                    choices=["test", "chat", "scan", "probe", "send", "listen",
                             "serve", "ask"],
                    help="默认 test（自检）")
    ap.add_argument("text", nargs="?", default="",
                    help="send / ask 模式要带的文字")
    ap.add_argument("-t", "--timeout", type=float, default=8.0,
                    help="扫描时长（秒），默认 8")
    ap.add_argument("-d", "--duration", type=float, default=60.0,
                    help="listen 模式听多久（秒），默认 60")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="把每个原始包也打出来（排查分包问题用）")
    args = ap.parse_args()

    VERBOSE = args.verbose

    if args.mode == "send" and not args.text:
        ap.error("send 模式要带一段文字，例如：python ble_ai_pc.py send 你好")

    fn = {
        "test": mode_test,
        "chat": mode_chat,
        "scan": mode_scan,
        "probe": mode_probe,
        "send": mode_send,
        "listen": mode_listen,
        "serve": mode_serve,
        "ask": mode_ask,
    }[args.mode]

    try:
        rc = asyncio.run(fn(args))
    except KeyboardInterrupt:
        log("")
        log("已中断。")
        rc = 130
    sys.exit(rc or 0)


if __name__ == "__main__":
    main()
