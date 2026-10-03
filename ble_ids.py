#!/usr/bin/env python3

import argparse
import asyncio

from bleak import BleakScanner


async def main(timeout):
    devices = await BleakScanner.discover(
        timeout=timeout,
        return_adv=True,
    )

    for address, (device, adv) in sorted(devices.items()):
        name = adv.local_name or device.name or "(no name)"
        print(f"{address}  {adv.rssi:4} dBm  {name}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="List nearby BLE devices by address.")
    parser.add_argument(
        "--timeout", type=float, default=15.0, help="scan time in seconds (default: 15)"
    )
    args = parser.parse_args()
    asyncio.run(main(args.timeout))
