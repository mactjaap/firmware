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

    def get_lines(self):
        with self.lock:
            return list(self.lines)

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



def run_test(results, number, description, test_func):
    step(number, description)

    try:
        details = test_func()

        if details is None:
            details = []

        elif isinstance(details, str):
            details = [details]

        results.append(
            {
                "number": number,
                "description": description,
                "passed": True,
                "error": "",
                "details": list(details),
            }
        )

        print(
            f"\nPASS: {description}",
            file=sys.stderr
        )

    except Exception as exc:
        results.append(
            {
                "number": number,
                "description": description,
                "passed": False,
                "error": str(exc),
                "details": [],
            }
        )

        print(
            f"\nNOT PASSED: {description}",
            file=sys.stderr
        )

        print(
            f"Reason: {exc}",
            file=sys.stderr
        )

def print_summary(results):
    print(
        "\n\n============================================================",
        file=sys.stderr
    )

    print(
        "MINI BROWSER TEST SUMMARY",
        file=sys.stderr
    )

    print(
        "============================================================",
        file=sys.stderr
    )

    for result in results:
        status = "PASS" if result["passed"] else "NOT PASSED"

        print(
            f"{result['number']:>2}. {status:<10} {result['description']}",
            file=sys.stderr
        )

        if result["error"]:
            print(
                f"    {result['error']}",
                file=sys.stderr
            )

        for detail in result.get("details", []):
            print(
                f"    > {detail}",
                file=sys.stderr
            )

    passed = sum(
        1
        for result in results
        if result["passed"]
    )

    total = len(results)
    failed = total - passed

    print(
        "------------------------------------------------------------",
        file=sys.stderr
    )

    print(
        f"Passed:     {passed}/{total}",
        file=sys.stderr
    )

    print(
        f"Not passed: {failed}/{total}",
        file=sys.stderr
    )

    print(
        "============================================================\n",
        file=sys.stderr
    )


def go_home(badge):
    badge.clear_log()
    badge.why("H")

    badge.wait_for(
        r"HTTP 200.*https://minibrowser\.macip\.net",
        20
    )

    badge.settle(1.0)


def activate_home_link(badge, link_number, expected_pattern, timeout=25):
    go_home(badge)

    badge.clear_log()
    badge.type_text(str(link_number))
    badge.settle(0.2)

    badge.enter()
    badge.settle(0.4)

    badge.wait_for(
        rf"activating link {link_number}",
        10
    )

    badge.wait_for(
        expected_pattern,
        timeout
    )

    badge.settle(1.0)



def latest_content_lines(badge, max_lines=5):
    lines = badge.get_lines()

    blocks = []
    current = None

    for line in lines:
        if line.strip() == "--- CONTENT START ---":
            current = []
            continue

        if line.strip() == "--- CONTENT END ---":
            if current is not None:
                blocks.append(current)
                current = None
            continue

        if current is not None:
            current.append(line)

    if not blocks:
        return []

    content = blocks[-1]

    useful = []

    for line in content:
        line = line.strip()

        if not line:
            continue

        if line.startswith("--------------------------------"):
            continue

        useful.append(line)

        if len(useful) >= max_lines:
            break

    return useful




def extract_search_results(badge, query="esp32", max_results=3):
    lines = badge.get_lines()

    blocks = []
    current = None

    for line in lines:
        if line.strip() == "--- CONTENT START ---":
            current = []
            continue

        if line.strip() == "--- CONTENT END ---":
            if current is not None:
                blocks.append(current)
                current = None
            continue

        if current is not None:
            current.append(line)

    if not blocks:
        return []

    content = blocks[-1]
    results = []

    for line in content:
        cleaned = line.strip()

        if not cleaned:
            continue

        if re.match(r"^\[\d+\]", cleaned):
            results.append(cleaned)

        if len(results) >= max_results:
            break

    if len(results) < max_results:
        for line in content:
            cleaned = line.strip()

            if not cleaned or cleaned in results:
                continue

            if cleaned.startswith("---"):
                continue

            if query.lower() in cleaned.lower():
                results.append(cleaned)

            if len(results) >= max_results:
                break

    return results[:max_results]



def main():
    badge = Badge()
    results = []

    print(
        "\nMini Browser automated regression test",
        file=sys.stderr
    )

    print(
        f"Serial device: {DEVICE}",
        file=sys.stderr
    )

    try:
        time.sleep(2.0)

        test_number = 1

        def test_home():
            go_home(badge)

        run_test(
            results,
            test_number,
            "Load Mini Browser home page",
            test_home
        )
        test_number += 1

        recommended_links = [
            (
                5,
                "Wiby - search the classic web",
                r"HTTP 200.*https://wiby\.me"
            ),
            (
                6,
                "Marginalia Search - lightweight search",
                r"HTTP 200.*marginalia"
            ),
            (
                7,
                "FrogFind - search and simplify pages",
                r"HTTP 200.*frogfind"
            ),
            (
                8,
                "Hacker News - technology news and discussion",
                r"HTTP 200.*news\.ycombinator\.com"
            ),
            (
                9,
                "NPR Text - text-oriented news",
                r"HTTP 200.*npr"
            ),
            (
                10,
                "TEXTFILES.COM - historic text files",
                r"HTTP 200.*textfiles\.com"
            ),
            (
                11,
                "curl - curl project website",
                r"HTTP 200.*curl"
            ),
            (
                12,
                "ifconfig.co - network information",
                r"HTTP 200.*ifconfig\.co"
            ),
        ]

        for link_number, description, pattern in recommended_links:
            def make_test(
                link_number=link_number,
                pattern=pattern
            ):
                def test_link():
                    activate_home_link(
                        badge,
                        link_number,
                        pattern
                    )

                return test_link

            run_test(
                results,
                test_number,
                f"Recommended link {link_number}: {description}",
                make_test()
            )

            test_number += 1

        def test_back():
            activate_home_link(
                badge,
                5,
                r"HTTP 200.*https://wiby\.me"
            )

            badge.clear_log()
            badge.why("B")

            badge.wait_for(
                r"HTTP 200.*https://minibrowser\.macip\.net",
                20
            )

            badge.settle(1.0)

        run_test(
            results,
            test_number,
            "Back history with WHY+B",
            test_back
        )
        test_number += 1

        def test_forward():
            activate_home_link(
                badge,
                5,
                r"HTTP 200.*https://wiby\.me"
            )

            badge.clear_log()
            badge.why("B")

            badge.wait_for(
                r"HTTP 200.*https://minibrowser\.macip\.net",
                20
            )

            badge.settle(1.0)
            badge.clear_log()

            badge.why("G")

            badge.wait_for(
                r"HTTP 200.*https://wiby\.me",
                20
            )

            badge.settle(1.0)

        run_test(
            results,
            test_number,
            "Forward history with WHY+G",
            test_forward
        )
        test_number += 1

        def test_reload():
            go_home(badge)

            badge.clear_log()
            badge.why("R")

            badge.wait_for(
                r"HTTP 200.*https://minibrowser\.macip\.net",
                20
            )

            badge.settle(1.0)

        run_test(
            results,
            test_number,
            "Reload current page with WHY+R",
            test_reload
        )
        test_number += 1

        def test_direct_url():
            go_home(badge)
            badge.clear_log()

            badge.why("E")
            badge.settle(0.5)

            badge.type_text(
                "example.com"
            )

            badge.settle(0.2)

            badge.enter()
            badge.settle(0.4)

            badge.wait_for(
                r"HTTP 200.*https://example\.com",
                20
            )

            badge.settle(1.0)

        run_test(
            results,
            test_number,
            "Open arbitrary URL with WHY+E",
            test_direct_url
        )
        test_number += 1

        def test_edit_url():
            go_home(badge)
            badge.clear_log()

            badge.why("C")
            badge.settle(0.5)

            # Clear existing URL conservatively with many backspaces.
            for _ in range(80):
                badge.backspace()

            badge.settle(0.3)

            badge.type_text(
                "https://example.com"
            )

            badge.settle(0.2)
            badge.enter()
            badge.settle(0.4)

            badge.wait_for(
                r"HTTP 200.*https://example\.com",
                20
            )

            badge.settle(1.0)

        run_test(
            results,
            test_number,
            "Edit current URL with WHY+C",
            test_edit_url
        )
        test_number += 1

        def test_wiby_search():
            activate_home_link(
                badge,
                5,
                r"HTTP 200.*https://wiby\.me"
            )

            badge.clear_log()
            badge.type_text("1")
            badge.settle(0.2)
            badge.enter()
            badge.settle(0.4)

            badge.type_text("esp32")
            badge.settle(0.3)
            badge.enter()
            badge.settle(0.4)

            badge.type_text("2")
            badge.settle(0.2)
            badge.enter()
            badge.settle(0.5)

            badge.wait_for(
                r"HTTP 200.*wiby\.me",
                30
            )

            badge.settle(2.0)

            all_lines = badge.get_lines()

            if not any(
                "esp32" in line.lower()
                for line in all_lines
            ):
                raise RuntimeError(
                    "Wiby returned HTTP 200, but no ESP32 text was found"
                )

            search_results = extract_search_results(
                badge,
                query="esp32",
                max_results=3
            )

            if len(search_results) < 3:
                raise RuntimeError(
                    f"Wiby search worked, but only {len(search_results)} visible result line(s) were captured"
                )

            return [
                "Wiby query: esp32",
                "Result 1: " + search_results[0],
                "Result 2: " + search_results[1],
                "Result 3: " + search_results[2],
            ]

        run_test(
            results,
            test_number,
            "Wiby search for esp32",
            test_wiby_search
        )
        test_number += 1

        def test_google_search():
            go_home(badge)

            badge.clear_log()
            badge.type_text("3")
            badge.settle(0.2)
            badge.enter()
            badge.settle(0.4)

            badge.type_text("esp32")
            badge.settle(0.3)
            badge.enter()
            badge.settle(0.4)

            badge.type_text("4")
            badge.settle(0.2)
            badge.enter()
            badge.settle(0.5)

            badge.wait_for(
                r"HTTP 200.*google",
                30
            )

            badge.settle(2.0)

            all_lines = badge.get_lines()

            if not any(
                "esp32" in line.lower()
                for line in all_lines
            ):
                raise RuntimeError(
                    "Google returned HTTP 200, but no ESP32 text was found"
                )

            search_results = extract_search_results(
                badge,
                query="esp32",
                max_results=3
            )

            if len(search_results) < 3:
                raise RuntimeError(
                    f"Google search worked, but only {len(search_results)} visible result line(s) were captured"
                )

            return [
                "Google query: esp32",
                "Result 1: " + search_results[0],
                "Result 2: " + search_results[1],
                "Result 3: " + search_results[2],
            ]

        run_test(
            results,
            test_number,
            "Google search for esp32",
            test_google_search
        )
        test_number += 1

        def test_home_again():
            go_home(badge)

        run_test(
            results,
            test_number,
            "Return Home with WHY+H",
            test_home_again
        )

        print_summary(results)

        if all(
            result["passed"]
            for result in results
        ):
            return 0

        return 1

    finally:
        badge.close()


if __name__ == "__main__":
    sys.exit(main())
