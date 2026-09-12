#!/usr/bin/env python3

import os
import select
import sys
import termios
import threading
import time
import tty

import serial


DEVICE = "/dev/cu.wchusbserial10"
BAUD = 115200


def tx(ser, scancode, down, text=0):
    command = f"E {scancode:02X} {1 if down else 0} {text:02X}\n"

    print(
        f"[TX] {command.strip()}",
        file=sys.stderr
    )

    ser.write(
        command.encode("ascii")
    )

    ser.flush()


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
    why_scancode = 0xE3

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

    return bytes(result)


def serial_reader(ser):
    while True:
        try:
            data = ser.read(1024)

            if data:
                os.write(
                    sys.stderr.fileno(),
                    data
                )

        except Exception:
            return

        time.sleep(0.01)


def main():
    ser = serial.Serial(
        DEVICE,
        BAUD,
        timeout=0
    )

    print(
        f"Connected to {DEVICE}",
        file=sys.stderr
    )

    print(
        "Ctrl-E = WHY+E, Ctrl-H = WHY+H, "
        "Ctrl-R = WHY+R, Ctrl-B = WHY+B, "
        "Ctrl-G = WHY+G, Ctrl-F = WHY+F, "
        "Ctrl-Q = WHY+Q",
        file=sys.stderr
    )

    print(
        "Ctrl-] exits",
        file=sys.stderr
    )

    thread = threading.Thread(
        target=serial_reader,
        args=(ser,),
        daemon=True
    )

    thread.start()

    fd = sys.stdin.fileno()
    old = termios.tcgetattr(fd)

    try:
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
                continue

            value = b[0]

            if value == 0x1D:
                break

            if value == 0x1B:
                sequence = read_escape(fd)

                scancode = ESCAPES.get(
                    sequence
                )

                if scancode is not None:
                    press(
                        ser,
                        scancode
                    )
                else:
                    press(
                        ser,
                        0x29
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

            if value in (0x08, 0x7F):
                press(
                    ser,
                    0x2A
                )

                continue

            if 1 <= value <= 26:
                letter = chr(
                    ord("A") +
                    value -
                    1
                )

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
        termios.tcsetattr(
            fd,
            termios.TCSADRAIN,
            old
        )

        ser.close()

        print(
            "\nDisconnected",
            file=sys.stderr
        )


if __name__ == "__main__":
    main()
