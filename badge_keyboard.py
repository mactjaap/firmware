#!/usr/bin/env python3

import argparse
import os
import select
import sys
import termios
import threading
import time
import tty

import serial

from badge_serial import find_port, key_line, send_key


DEVICE = "/dev/cu.wchusbserial10"
BAUD = 115200

# Pause after every key event so the badge's serial line parser keeps up.
EVENT_PACING = 0.005

WHY_SCANCODE = 0xE3

# Ctrl-<letter> -> WHY+<letter>.  Ctrl-I/J/M are Tab/Enter in a terminal and
# are handled as such.  The help text is generated from this map.
CTRL_MAP = {
    ord(letter) - ord("A") + 1: letter
    for letter in "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    if letter not in "IJM"
}


def tx(ser, scancode, down, text=0):
    print(
        f"[TX] {key_line(scancode, down, text).decode('ascii').strip()}",
        file=sys.stderr
    )

    send_key(ser, scancode, down, text)
    time.sleep(EVENT_PACING)


def press(ser, scancode, text=0):
    tx(
        ser,
        scancode,
        True,
        text
    )

    tx(
        ser,
        scancode,
        False,
        0
    )


def why(ser, letter):
    why_scancode = WHY_SCANCODE

    letter = letter.upper()

    key_scancode = (
        0x04 +
        ord(letter) -
        ord("A")
    )

    tx(
        ser,
        why_scancode,
        True,
        0
    )

    tx(
        ser,
        key_scancode,
        True,
        ord(letter.lower())
    )

    tx(
        ser,
        key_scancode,
        False,
        0
    )

    tx(
        ser,
        why_scancode,
        False,
        0
    )


def ascii_scancode(c):
    o = ord(c)

    if "a" <= c <= "z":
        return (
            0x04 +
            ord(c) -
            ord("a")
        )

    if "A" <= c <= "Z":
        return (
            0x04 +
            ord(c) -
            ord("A")
        )

    if "1" <= c <= "9":
        return (
            0x1E +
            ord(c) -
            ord("1")
        )

    if c == "0":
        return 0x27

    table = {
        " ": 0x2C,
        "-": 0x2D,
        "_": 0x2D,
        "=": 0x2E,
        "+": 0x2E,
        "[": 0x2F,
        "{": 0x2F,
        "]": 0x30,
        "}": 0x30,
        "\\": 0x31,
        "|": 0x31,
        ";": 0x33,
        ":": 0x33,
        "'": 0x34,
        '"': 0x34,
        "`": 0x35,
        "~": 0x35,
        ",": 0x36,
        "<": 0x36,
        ".": 0x37,
        ">": 0x37,
        "/": 0x38,
        "?": 0x38,
        "!": 0x1E,
        "@": 0x1F,
        "#": 0x20,
        "$": 0x21,
        "%": 0x22,
        "^": 0x23,
        "&": 0x24,
        "*": 0x25,
        "(": 0x26,
        ")": 0x27,
    }

    return table.get(c)


ESCAPES = {
    b"\x1b[A": 0x52,   # Up
    b"\x1b[B": 0x51,   # Down
    b"\x1b[C": 0x4F,   # Right
    b"\x1b[D": 0x50,   # Left

    b"\x1b[H": 0x4A,   # Home
    b"\x1b[F": 0x4D,   # End

    b"\x1bOH": 0x4A,
    b"\x1bOF": 0x4D,

    b"\x1b[3~": 0x4C,  # Delete
    b"\x1b[5~": 0x4B,  # Page Up
    b"\x1b[6~": 0x4E,  # Page Down
}


def read_escape(fd):
    result = bytearray(b"\x1b")
    deadline = time.monotonic() + 0.05

    while time.monotonic() < deadline:
        readable, _, _ = select.select(
            [fd],
            [],
            [],
            0.005
        )

        if not readable:
            continue

        b = os.read(fd, 1)

        if not b:
            break

        result.extend(b)

        current = bytes(result)

        if current in ESCAPES:
            break

        if current.endswith(b"~"):
            break

        # Any other complete CSI/SS3 sequence ends with a final byte.
        if (
            len(current) >= 3
            and current[1:2] in (b"[", b"O")
            and 0x40 <= current[-1] <= 0x7E
        ):
            break

    return bytes(result)


def serial_reader(ser, stop):
    while not stop.is_set():
        try:
            data = ser.read(1024)

            if data:
                os.write(
                    sys.stderr.fileno(),
                    data
                )

        except Exception as exc:
            if not stop.is_set():
                print(
                    f"\r\n[serial reader stopped: {type(exc).__name__}: {exc}]",
                    file=sys.stderr
                )
            return

        time.sleep(0.01)


def ctrl_help():
    letters = " ".join(CTRL_MAP[value] for value in sorted(CTRL_MAP))
    return f"Ctrl-<X> = WHY+<X> for X in: {letters}"


def main():
    parser = argparse.ArgumentParser(
        description="Forward terminal keystrokes to the badge's serial keyboard."
    )
    parser.add_argument(
        "--port",
        default=None,
        help=f"serial device (default: $BADGE_PORT, auto-detect, then {DEVICE})",
    )
    parser.add_argument(
        "--baud",
        type=int,
        default=BAUD,
        help=f"serial baud rate (default: {BAUD})",
    )
    args = parser.parse_args()

    device = args.port or find_port(DEVICE)

    with serial.Serial(device, args.baud, timeout=0) as ser:
        print(
            f"Connected to {device}",
            file=sys.stderr
        )

        print(
            ctrl_help(),
            file=sys.stderr
        )

        print(
            "Ctrl-] exits",
            file=sys.stderr
        )

        stop = threading.Event()

        thread = threading.Thread(
            target=serial_reader,
            args=(ser, stop),
            daemon=True
        )

        thread.start()

        try:
            forward_keys(ser)
        finally:
            stop.set()
            thread.join(timeout=1.0)

    print(
        "\nDisconnected",
        file=sys.stderr
    )


def forward_keys(ser):
    fd = sys.stdin.fileno()
    is_tty = os.isatty(fd)

    if not is_tty:
        print(
            "stdin is not a terminal; forwarding input until EOF",
            file=sys.stderr
        )

    old = termios.tcgetattr(fd) if is_tty else None

    try:
        if is_tty:
            tty.setraw(fd)

            attrs = termios.tcgetattr(fd)
            attrs[1] |= termios.OPOST | termios.ONLCR
            termios.tcsetattr(
                fd,
                termios.TCSANOW,
                attrs
            )

        while True:
            b = os.read(fd, 1)

            if not b:
                break

            value = b[0]

            if value == 0x1D:
                break

            if value == 0x1B:
                sequence = read_escape(fd)

                if sequence == b"\x1b":
                    # A lone ESC key press.
                    press(
                        ser,
                        0x29
                    )
                    continue

                scancode = ESCAPES.get(
                    sequence
                )

                # Unknown sequences are ignored rather than sent as ESC.
                if scancode is not None:
                    press(
                        ser,
                        scancode
                    )

                continue

            if value in (0x0A, 0x0D):
                press(
                    ser,
                    0x28
                )

                continue

            if value == 0x09:
                press(
                    ser,
                    0x2B
                )

                continue

            if value == 0x7F:
                press(
                    ser,
                    0x2A
                )

                continue

            if value in CTRL_MAP:
                letter = CTRL_MAP[value]

                why(
                    ser,
                    letter
                )

                continue

            if 0x20 <= value <= 0x7E:
                character = chr(value)

                scancode = ascii_scancode(
                    character
                )

                if scancode is not None:
                    press(
                        ser,
                        scancode,
                        value
                    )

    finally:
        if old is not None:
            termios.tcsetattr(
                fd,
                termios.TCSADRAIN,
                old
            )


if __name__ == "__main__":
    main()
