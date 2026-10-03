#!/usr/bin/env python3

import argparse
import asyncio
import sys

from bleak import BleakClient


async def main(device, timeout):
    print(f"Connecting to {device} ...", file=sys.stderr)

    try:
        async with BleakClient(device, timeout=timeout) as client:
            print(f"Connected: {client.is_connected}")
            print()

            for service in client.services:
                print(f"SERVICE {service.uuid}")

                if service.description:
                    print(f"  Description: {service.description}")

                for char in service.characteristics:
                    props = ", ".join(char.properties)

                    print(f"  CHAR {char.uuid}")
                    print(f"    Properties: {props or '-'}")

                    if char.description:
                        print(f"    Description: {char.description}")

                    for descriptor in char.descriptors:
                        print(f"    DESC {descriptor.uuid}")

                print()

    except Exception as exc:
        print(f"ERROR: {type(exc).__name__}: {exc}", file=sys.stderr)
        raise SystemExit(2)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="List GATT services of a BLE device.")
    parser.add_argument("device", help="device address / UUID (see ble_ids.py)")
    parser.add_argument(
        "--timeout", type=float, default=15.0, help="connect timeout in seconds (default: 15)"
    )
    args = parser.parse_args()
    asyncio.run(main(args.device, args.timeout))
