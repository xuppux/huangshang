"""用电脑直接给手表蓝牙校时 —— 绕开浏览器，验证固件这一侧。

为什么需要它：
  网页校时（Web Bluetooth）这条路上一旦失败，责任方有两个：手表固件，
  或者浏览器/系统蓝牙栈。从页面上的报错分不清是谁 --- "连接失败"三个字
  两边都可能是原因。

  这个脚本用几条命令走一模一样的 GATT 路径（同样连 0x1805、同样读写 0x2A2B），
  **只不过客户端从浏览器换成了 Python**。于是：
      这个脚本能通、网页不通  -> 问题在浏览器/系统蓝牙那一侧
      这个脚本也不通          -> 问题在手表固件那一侧
  一次就能把责任切开。

用法：
    <venv>\\Scripts\\python.exe ble_sync.py            # 连上、读一次、写时间、再读回
    <venv>\\Scripts\\python.exe ble_sync.py --read      # 只读，不改表
    <venv>\\Scripts\\python.exe ble_sync.py --name HS-Watch
"""

import argparse
import asyncio
import datetime as dt
import sys

CTS_SVC = "00001805-0000-1000-8000-00805f9b34fb"
CT_CHR = "00002a2b-0000-1000-8000-00805f9b34fb"

DOW = ["周一", "周二", "周三", "周四", "周五", "周六", "周日"]


def parse(payload: bytes) -> str:
    """CTS 当前时间：10 字节，低字节在前。"""
    if len(payload) < 10:
        return f"(只有 {len(payload)} 字节，不像 CTS 时间包)"
    year = payload[0] | (payload[1] << 8)
    mo, day, h, mi, s, dow, frac, reason = payload[2:10]
    wd = DOW[dow - 1] if 1 <= dow <= 7 else f"?{dow}"
    return (f"{year:04d}-{mo:02d}-{day:02d} {h:02d}:{mi:02d}:{s:02d} "
            f"({wd}, frac={frac}, reason=0x{reason:02x})")


def build(now: dt.datetime) -> bytes:
    """组 10 字节 CTS 包。星期按蓝牙规定 1=周一..7=周日。"""
    dow = now.isoweekday()          # Python 的 isoweekday() 正好就是 1=周一..7=周日
    return bytes([
        now.year & 0xFF, (now.year >> 8) & 0xFF,
        now.month, now.day, now.hour, now.minute, now.second,
        dow, 0, 1,                  # frac=0, adjust_reason bit0=手动更新
    ])


async def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", default="HS-Watch")
    ap.add_argument("--read", action="store_true", help="只读时间，不写")
    ap.add_argument("--timeout", type=float, default=25.0)
    args = ap.parse_args()

    try:
        from bleak import BleakClient, BleakScanner
    except ImportError:
        print("缺少 bleak：pip install bleak")
        return 2

    print(f"=== 找 {args.name}（最多等 {args.timeout:.0f} 秒）===")
    dev = await BleakScanner.find_device_by_filter(
        lambda d, a: args.name.lower() in ((a.local_name or d.name or "").lower()),
        timeout=args.timeout,
    )
    if dev is None:
        print("没找到设备。手表没在广播，或者广播已经超时了。")
        return 1

    print(f"找到 {dev.name}  [{dev.address}]")

    async with BleakClient(dev, timeout=20.0) as client:
        print(f"GATT 已连接: {client.is_connected}")

        svcs = client.services
        print("服务列表：")
        for s in svcs:
            print(f"  {s.uuid}  {s.description}")
        if CTS_SVC not in [s.uuid.lower() for s in svcs]:
            print("⚠ 没有 0x1805 —— 固件里 CTS 没注册上")
            return 3

        before = await client.read_gatt_char(CT_CHR)
        print(f"\n读回（写入前）: {parse(before)}")

        if args.read:
            return 0

        now = dt.datetime.now()
        payload = build(now)
        print(f"写入本机时间  : {now:%Y-%m-%d %H:%M:%S}  ->  {list(payload)}")
        # response=True => Write Request（带 ACK），和网页 ch.writeValue() 是同一种写
        await client.write_gatt_char(CT_CHR, payload, response=True)
        print("写入成功（Write Request 已被手表确认）")

        await asyncio.sleep(0.6)
        after = await client.read_gatt_char(CT_CHR)
        print(f"读回（写入后）: {parse(after)}")

        # 同样做一次回读核对：差 5 秒内算对
        y = after[0] | (after[1] << 8)
        got = dt.datetime(y, after[2], after[3], after[4], after[5], after[6])
        diff = abs((dt.datetime.now() - got).total_seconds())
        print(f"\n与电脑时间相差 {diff:.0f} 秒 -> "
              + ("核对一致 ✅" if diff <= 5 else "对不上 ❌（写入没生效？）"))
        return 0 if diff <= 5 else 4


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
