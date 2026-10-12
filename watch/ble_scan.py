"""扫描附近的 BLE 广播，专门盯黄山派手表（HS-Watch）。

为什么要有这个脚本：
  网页（Web Bluetooth）把"扫描"这一步藏起来了 —— 它弹出的设备列表里没有
  UUID、没有 RSSI、没有时间轴，出了问题只能说一句"列表是空的"，
  根本分不清是手表没广播、还是电脑没扫到、还是筛选条件不匹配。

  这个脚本把广播本身摊开来看：每个包里有什么服务 UUID、每隔多久来一次、
  什么时候突然没了。判断"手表到底在不在广播"就靠它。

用法（在 watch 目录下）：
    <venv>\\Scripts\\python.exe ble_scan.py            # 扫 30 秒
    <venv>\\Scripts\\python.exe ble_scan.py -t 90      # 扫 90 秒
    <venv>\\Scripts\\python.exe ble_scan.py --all       # 连别的设备也列出来
"""

import argparse
import asyncio
import sys
import time

NAME_HINT = "HS-Watch"
CTS_UUID = "00001805-0000-1000-8000-00805f9b34fb"


def ts() -> str:
    return time.strftime("%H:%M:%S")


async def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("-t", "--seconds", type=float, default=30.0)
    ap.add_argument("--all", action="store_true", help="列出所有 BLE 设备，不只是手表")
    ap.add_argument("--expect", default=NAME_HINT)
    args = ap.parse_args()

    try:
        from bleak import BleakScanner
    except ImportError:
        print("缺少 bleak：pip install bleak")
        return 2

    hits: dict[str, dict] = {}
    t0 = time.monotonic()

    def on_adv(device, adv):
        name = (adv.local_name or device.name or "").strip()
        uuids = [u.lower() for u in (adv.service_uuids or [])]
        interesting = (
            args.expect.lower() in name.lower()
            or CTS_UUID in uuids
            or args.all
        )
        if not interesting:
            return

        key = device.address
        rec = hits.setdefault(key, {"name": name, "first": None, "last": None,
                                    "n": 0, "uuids": set(), "rssi": None})
        now = time.monotonic() - t0
        if rec["first"] is None:
            rec["first"] = now
            rec["uuids"] = set(uuids)
            print(f"\n{ts()}  ★ 发现 {name or '(无名)'}  [{device.address}]")
            print(f"      服务 UUID: {uuids if uuids else '（广播包里没有服务 UUID！）'}")
        rec["last"] = now
        rec["n"] += 1
        rec["rssi"] = adv.rssi
        rec["uuids"] |= set(uuids)

    print(f"=== 扫描 {args.seconds:.0f} 秒（找 \"{args.expect}\"）===")
    print("如果这期间一行 ★ 都没有，就是手表没在广播 —— 别去查电脑和浏览器了。\n")

    scanner = BleakScanner(detection_callback=on_adv)
    await scanner.start()
    try:
        await asyncio.sleep(args.seconds)
    finally:
        await scanner.stop()

    print("\n" + "=" * 62)
    if not hits:
        print("结果：没有扫到任何目标广播。")
        print("      => 手表侧的问题：没开广播 / 广播已超时 / 刚开就睡死了。")
        return 1

    for addr, r in hits.items():
        span = (r["last"] - r["first"]) if r["first"] is not None else 0
        rate = (r["n"] / span) if span > 0.5 else float("nan")
        print(f"{r['name'] or '(无名)'}  [{addr}]")
        print(f"  广播包数 : {r['n']}")
        print(f"  首见     : +{r['first']:.1f}s")
        print(f"  末见     : +{r['last']:.1f}s   （跨度 {span:.1f}s）")
        print(f"  出现速率 : {rate:.1f} 包/秒   （RSSI 末次 {r['rssi']}）")
        print(f"  服务 UUID: {sorted(r['uuids']) or '（空）'}")
        if CTS_UUID not in r["uuids"]:
            print("  ⚠ 广播包里没有 0x1805 —— 网页按服务筛选会找不到这块表")
    print("=" * 62)
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
