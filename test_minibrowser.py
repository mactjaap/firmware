#!/usr/bin/env python3

import re
import sys
import threading
import time

import serial


DEVICE = "/dev/cu.wchusbserial10"
BAUD = 115200

DEFAULT_TIMEOUT = 20.0


class Badge:
    def __init__(self, device=DEVICE, baud=BAUD):
        self.serial = serial.Serial(
            device,
            baud,
            timeout=0.1
        )

        self.lines = []
        self.lock = threading.Lock()
        self.running = True

        self.reader_thread = threading.Thread(
            target=self._reader,
            daemon=True
        )

        self.reader_thread.start()

    def _reader(self):
        buffer = bytearray()

        while self.running:
            try:
                data = self.serial.read(1024)

                if not data:
                    continue

                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()

                buffer.extend(data)

                while b"\n" in buffer:
                    line, _, buffer = buffer.partition(b"\n")

                    text = line.decode(
                        "utf-8",
                        errors="replace"
                    ).rstrip("\r")

                    with self.lock:
                        self.lines.append(text)

                        if len(self.lines) > 3000:
                            self.lines = self.lines[-2000:]

            except Exception as exc:
                if self.running:
                    print(
                        f"\nSerial reader error: {exc}",
                        file=sys.stderr
                    )

                return

    def close(self):
        self.running = False

        try:
            self.serial.close()
        except Exception:
            pass

    def clear_log(self):
        with self.lock:
            self.lines.clear()

    def send_event(self, scancode, down, text=0):
        command = (
            f"E {scancode:02X} "
            f"{1 if down else 0} "
            f"{text:02X}\n"
        )

        print(
            f"\n[TEST TX] {command.strip()}",
            file=sys.stderr
        )

        self.serial.write(
            command.encode("ascii")
        )

        self.serial.flush()

    def press(self, scancode, text=0, delay=0.08):
        self.send_event(
            scancode,
            True,
            text
        )

        time.sleep(delay)

        self.send_event(
            scancode,
            False,
            0
        )

        time.sleep(delay)

    def why(self, letter):
        letter = letter.upper()

        why_scancode = 0xE3

        key_scancode = (
            0x04 +
            ord(letter) -
            ord("A")
        )

        self.send_event(
            why_scancode,
            True,
            0
        )

        time.sleep(0.08)

        self.send_event(
            key_scancode,
            True,
            ord(letter.lower())
        )

        time.sleep(0.08)

        self.send_event(
            key_scancode,
            False,
            0
        )

        time.sleep(0.08)

        self.send_event(
            why_scancode,
            False,
            0
        )

        time.sleep(0.30)

    def settle(self, seconds=0.5):
        self.serial.flush()
        time.sleep(seconds)

    def enter(self):
        self.press(0x28)

    def backspace(self):
        self.press(0x2A)

    def up(self):
        self.press(0x52)

    def down(self):
        self.press(0x51)

    def left(self):
        self.press(0x50)

    def right(self):
        self.press(0x4F)

    def wait_for(self, pattern, timeout=DEFAULT_TIMEOUT):
        print(
            f"\n[WAIT] {pattern}",
            file=sys.stderr
        )

        regex = re.compile(pattern)

        deadline = time.monotonic() + timeout

        checked = 0

        while time.monotonic() < deadline:
            with self.lock:
                current = list(self.lines)

            for line in current[checked:]:
                if regex.search(line):
                    print(
                        f"\n[MATCH] {line}",
                        file=sys.stderr
                    )

                    return line

            checked = len(current)

            time.sleep(0.05)

        raise TimeoutError(
            f"Timeout waiting for: {pattern}"
        )

    def type_text(self, text, key_delay=0.06):
        for character in text:
            scancode = ascii_scancode(character)

            if scancode is None:
                raise ValueError(
                    f"Unsupported character: {character!r}"
                )

            self.press(
                scancode,
                ord(character),
                delay=0.04
            )

            time.sleep(key_delay)


def ascii_scancode(c):
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


def step(number, description):
    print(
        f"\n\n========== TEST {number}: {description} ==========",
        file=sys.stderr
    )


def main():
    badge = Badge()

    failures = []

    print(
        "\nMini Browser automated smoke test",
        file=sys.stderr
    )

    print(
        f"Serial device: {DEVICE}",
        file=sys.stderr
    )

    try:
        time.sleep(2.0)

        step(
            1,
            "Load Mini Browser home page"
        )

        badge.clear_log()

        badge.why("H")

        badge.wait_for(
            r"HTTP 200.*https://minibrowser\.macip\.net",
            20
        )

        badge.settle(1.0)

        print(
            "\nPASS: home page loaded",
            file=sys.stderr
        )

        step(
            2,
            "Activate link 5 to Wiby"
        )

        badge.clear_log()

        badge.type_text("5")
        badge.settle(0.2)

        badge.enter()
        badge.settle(0.3)

        badge.wait_for(
            r"activating link 5.*https://wiby\.me",
            10
        )

        badge.wait_for(
            r"HTTP 200.*https://wiby\.me",
            20
        )

        badge.settle(1.0)

        print(
            "\nPASS: link navigation works",
            file=sys.stderr
        )

        step(
            3,
            "Test Back history"
        )

        badge.clear_log()

        badge.why("B")

        badge.wait_for(
            r"HTTP 200.*https://minibrowser\.macip\.net",
            20
        )

        badge.settle(1.0)

        print(
            "\nPASS: back history works",
            file=sys.stderr
        )

        step(
            4,
            "Open arbitrary URL with WHY+E"
        )

        badge.clear_log()

        badge.why("E")
        badge.settle(0.5)

        badge.type_text(
            "example.com"
        )
        badge.settle(0.2)

        badge.enter()
        badge.settle(0.3)

        badge.wait_for(
            r"HTTP 200.*https://example\.com",
            20
        )

        badge.settle(1.0)

        print(
            "\nPASS: direct URL entry works",
            file=sys.stderr
        )

        step(
            5,
            "Return Home"
        )

        badge.clear_log()

        badge.why("H")

        badge.wait_for(
            r"HTTP 200.*https://minibrowser\.macip\.net",
            20
        )

        badge.settle(1.0)

        print(
            "\nPASS: WHY+H works",
            file=sys.stderr
        )

        print(
            "\n\n========================================",
            file=sys.stderr
        )

        print(
            "ALL MINI BROWSER TESTS PASSED",
            file=sys.stderr
        )

        print(
            "========================================\n",
            file=sys.stderr
        )

        return 0

    except Exception as exc:
        print(
            "\n\n========================================",
            file=sys.stderr
        )

        print(
            f"TEST FAILED: {exc}",
            file=sys.stderr
        )

        print(
            "========================================\n",
            file=sys.stderr
        )

        return 1

    finally:
        badge.close()


if __name__ == "__main__":
    sys.exit(main())
