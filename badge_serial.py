"""Shared serial helpers for the WHY2025 badge host tools."""

import os
import sys
from typing import Any, Optional

# WCH CH34x USB-serial bridge used on the badge.
WCH_VID = 0x1A86


def find_port(default: Optional[str] = None) -> Optional[str]:
    """Return the badge serial port.

    Order: $BADGE_PORT, a single auto-detected WCH CH34x device, `default`.
    """
    env = os.environ.get("BADGE_PORT")
    if env:
        return env

    try:
        from serial.tools import list_ports
    except ImportError:
        return default

    candidates = sorted(
        port.device
        for port in list_ports.comports()
        if port.vid == WCH_VID
    )

    if sys.platform == "darwin":
        # macOS lists both /dev/tty.* and /dev/cu.*; prefer the callout device.
        cu = [device for device in candidates if device.startswith("/dev/cu.")]
        candidates = cu or candidates

    if len(candidates) == 1:
        return candidates[0]

    if len(candidates) > 1:
        print(
            "Multiple WCH serial devices found ("
            + ", ".join(candidates)
            + "); set BADGE_PORT or pass the port explicitly.",
            file=sys.stderr,
        )

    return default


def key_line(scancode: int, down: bool, text: int = 0) -> bytes:
    """Encode one badge serial-keyboard event: E <scancode> <down> <text>."""
    return f"E {scancode:02X} {1 if down else 0} {text:02X}\n".encode("ascii")


def send_key(ser: Any, scancode: int, down: bool, text: int = 0) -> None:
    """Send one key event to the badge's serial keyboard."""
    ser.write(key_line(scancode, down, text))
    ser.flush()
