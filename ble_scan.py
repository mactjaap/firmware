#!/usr/bin/env python3

import argparse
import asyncio
import sys

from bleak import BleakScanner


async def main(timeout):
    print(f"Scanning for BLE devices for {timeout:g} seconds...\n", file=sys.stderr)

    devices = await BleakScanner.discover(
        timeout=timeout,
        return_adv=True,
    )

    entries = []

    for address, (device, adv) in devices.items():
        entries.append((
            adv.rssi,
            device.address,
            adv.local_name or device.name or "(no name)",
            adv.service_uuids,
            adv.manufacturer_data,
        ))

    entries.sort(reverse=True)

    for rssi, address, name, services, manufacturer in entries:
        print(f"{address}")
        print(f"  Name:         {name}")
        print(f"  RSSI:         {rssi} dBm")
        print(f"  Services:     {services or '-'}")

        if manufacturer:
            print("  Manufacturer:")
            for company_id, data in manufacturer.items():
                print(
                    f"    0x{company_id:04X}: "
                    f"{data.hex()}"
                )
        else:
            print("  Manufacturer: -")

        print()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Scan for nearby BLE devices.")
    parser.add_argument(
        "--timeout", type=float, default=15.0, help="scan time in seconds (default: 15)"
    )
    args = parser.parse_args()
    asyncio.run(main(args.timeout))
