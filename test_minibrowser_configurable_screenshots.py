#!/usr/bin/env python3

"""
Configurable Mini Browser / WHY2025 BadgeVMS regression tester.

Edit only the CONFIG section for normal use.

Tests supported:
- Load arbitrary web sites through WHY+E
- Activate numbered links from the Mini Browser home page
- Run GET-form searches
- Exercise WHY-key badge/browser commands
- Print a PASS / NOT PASSED summary

Serial keyboard protocol:
    E <scancode-hex> <down 0|1> <text-hex>

Example:
    E 28 1 00
    E 28 0 00
"""

import base64
import re
import struct
import sys
import time
import tty
import termios
import threading
import zlib
from pathlib import Path

import serial


# ---------------------------------------------------------------------------
# CONFIG
# ---------------------------------------------------------------------------

CONFIG = {
    "serial": {
        "device": "/dev/cu.wchusbserial10",
        "baudrate": 115200,
        "startup_delay": 2.0,
        "default_timeout": 25,
    },

    "screenshots": {
        "enabled": True,
        "directory": "screenshots",
        "timeout": 150,
        "after_home": True,
        "after_sites": True,
        "after_searches": True,
        "after_commands": False,
        "screenshot_on_failure": True,
    },

    # Mini Browser home page.
    "home": {
        "url_pattern": r"HTTP 200.*https://minibrowser\.macip\.net",
    },

    # Generic web-site tests.
    #
    # mode:
    #   "url"       -> WHY+E, then type URL
    #   "home_link" -> return home and activate numbered link
    #
    "sites": [
        {
            "name": "Example.com",
            "mode": "url",
            "url": "example.com",
            "expect": r"HTTP 200.*https://example\.com",
        },
        {
            "name": "MacIP.net",
            "mode": "url",
            "url": "macip.net",
            "expect": r"HTTP 200.*macip\.net",
        },
        {
            "name": "Wiby",
            "mode": "home_link",
            "action": 5,
            "expect": r"HTTP 200.*https://wiby\.me",
        },
        {
            "name": "Hacker News",
            "mode": "home_link",
            "action": 8,
            "expect": r"HTTP 200.*news\.ycombinator\.com",
        },
        {
            "name": "curl",
            "mode": "home_link",
            "action": 11,
            "expect": r"HTTP 200.*curl",
        },
        {
            "name": "ifconfig.co",
            "mode": "home_link",
            "action": 12,
            "expect": r"HTTP 200.*ifconfig.co",
        },
    ],

    # Search tests.
    #
    # source:
    #   "home"      -> search form is on Mini Browser home page
    #   "home_link" -> first open a numbered link from home, then use form
    #
    # submit_label:
    #   unique text contained in the submit control.
    #
    # numbered_fallback:
    #   useful for Google when result titles/URLs are stripped but numbered
    #   result actions are still clearly present.
    #
    "searches": [
        {
            "name": "Wiby search",
            "source": "home_link",
            "home_action": 5,
            "open_expect": r"HTTP 200.*https://wiby\.me",
            "query": "esp32",
            "submit_label": "[search]",
            "result_expect": r"HTTP 200.*wiby\.me",
            "numbered_fallback": None,
        },
        {
            "name": "Google search",
            "source": "home",
            "query": "esp32",
            "submit_label": "[google search]",
            "result_expect": r"HTTP 200.*google",
            "numbered_fallback": {
                "start": 18,
                "stop": 46,
                "minimum": 3,
            },
        },
    ],

    # Badge/browser command tests.
    #
    # command:
    #   One of the WHY shortcuts, e.g. H R B G F M Q C E
    #
    # expect:
    #   Regex that must appear after the command.
    #
    # setup:
    #   Optional built-in setup before sending the command.
    #
    # Available setup values:
    #   "home"
    #   "home_then_wiby"
    #   "home_wiby_back"
    #
    # Keep WHY+Q last if you enable it, because it exits Mini Browser.
    #
    "badge_commands": [
        {
            "name": "Home",
            "command": "H",
            "setup": None,
            "expect": r"HTTP 200.*https://minibrowser\.macip\.net",
        },
        {
            "name": "Reload",
            "command": "R",
            "setup": "home",
            "expect": r"HTTP 200.*https://minibrowser\.macip\.net",
        },
        {
            "name": "Back",
            "command": "B",
            "setup": "home_then_wiby",
            "expect": r"HTTP 200.*https://minibrowser\.macip\.net",
        },
        {
            "name": "Forward",
            "command": "G",
            "setup": "home_wiby_back",
            "expect": r"HTTP 200.*https://wiby\.me",
        },

        # Examples that are disabled by default because they need a more
        # specific assertion or alter application state:
        #
        # {
        #     "name": "Bookmarks",
        #     "command": "M",
        #     "setup": "home",
        #     "expect": r"...",
        # },
        #
        # {
        #     "name": "Quit",
        #     "command": "Q",
        #     "setup": "home",
        #     "expect": r"...",
        # },
    ],
}


# ---------------------------------------------------------------------------
# CONSTANTS
# ---------------------------------------------------------------------------

DEVICE = CONFIG["serial"]["device"]
BAUDRATE = CONFIG["serial"]["baudrate"]
DEFAULT_TIMEOUT = CONFIG["serial"]["default_timeout"]


# ---------------------------------------------------------------------------
# SERIAL / KEYBOARD
# ---------------------------------------------------------------------------

def safe_filename(text):
    value = re.sub(
        r"[^A-Za-z0-9._-]+",
        "-",
        text.strip().lower(),
    )
    value = value.strip("-._")
    return value or "screenshot"


def png_chunk(chunk_type, payload):
    crc = zlib.crc32(chunk_type)
    crc = zlib.crc32(payload, crc) & 0xFFFFFFFF

    return (
        struct.pack(">I", len(payload))
        + chunk_type
        + payload
        + struct.pack(">I", crc)
    )


def write_png_rgb(path, width, height, rgb):
    expected = width * height * 3

    if len(rgb) != expected:
        raise ValueError(
            f"RGB byte count mismatch: got {len(rgb)}, expected {expected}"
        )

    rows = bytearray()
    stride = width * 3

    for y in range(height):
        rows.append(0)
        start = y * stride
        rows.extend(
            rgb[start:start + stride]
        )

    png = bytearray(b"\x89PNG\r\n\x1a\n")

    png.extend(
        png_chunk(
            b"IHDR",
            struct.pack(
                ">IIBBBBB",
                width,
                height,
                8,
                2,
                0,
                0,
                0,
            ),
        )
    )

    png.extend(
        png_chunk(
            b"IDAT",
            zlib.compress(
                bytes(rows),
                9,
            ),
        )
    )

    png.extend(
        png_chunk(
            b"IEND",
            b"",
        )
    )

    path = Path(path)
    path.parent.mkdir(
        parents=True,
        exist_ok=True,
    )
    path.write_bytes(png)


def decode_rle5(data, width, height):
    if len(data) % 5 != 0:
        raise ValueError(
            f"RLE stream length {len(data)} is not divisible by 5"
        )

    expected_pixels = width * height
    produced_pixels = 0
    rgb = bytearray()

    for offset in range(0, len(data), 5):
        count = (
            data[offset]
            | (data[offset + 1] << 8)
        )

        if count == 0:
            raise ValueError(
                f"Invalid zero-length RLE run at byte {offset}"
            )

        r = data[offset + 2]
        g = data[offset + 3]
        b = data[offset + 4]

        produced_pixels += count

        if produced_pixels > expected_pixels:
            raise ValueError(
                "RLE stream expands beyond screenshot dimensions"
            )

        rgb.extend(
            bytes((r, g, b)) * count
        )

    if produced_pixels != expected_pixels:
        raise ValueError(
            (
                f"RLE pixel count mismatch: got {produced_pixels}, "
                f"expected {expected_pixels}"
            )
        )

    return bytes(rgb)



class Badge:
    def __init__(self):
        self.serial = serial.Serial(
            DEVICE,
            BAUDRATE,
            timeout=0.1,
        )

        self.running = True
        self.lock = threading.Lock()
        self.lines = []

        self.reader_thread = threading.Thread(
            target=self._reader,
            daemon=True,
        )
        self.reader_thread.start()

    def _reader(self):
        buffer = bytearray()
        suppress_img = False

        while self.running:
            try:
                data = self.serial.read(1024)

                if not data:
                    continue

                buffer.extend(data)

                while b"\n" in buffer:
                    line, _, buffer = buffer.partition(b"\n")

                    text = line.decode(
                        "utf-8",
                        errors="replace"
                    ).rstrip("\r")

                    stripped = text.strip()

                    if stripped.startswith("IMG BEGIN "):
                        suppress_img = True
                        print(
                            "[SCREENSHOT] Receiving image data...",
                            file=sys.stderr
                        )

                    is_img_line = (
                        stripped.startswith("IMG ")
                        or suppress_img
                    )

                    if not is_img_line:
                        print(text)

                    with self.lock:
                        self.lines.append(text)

                        # FEC screenshots can produce well over 5,000
                        # IMG records. Keep enough history for the complete
                        # transfer so early data/parity groups are not discarded
                        # before IMG END arrives.
                        if len(self.lines) > 20000:
                            self.lines = self.lines[-18000:]

                    if stripped.startswith("IMG END "):
                        suppress_img = False

                    elif stripped.startswith("IMG ERROR "):
                        suppress_img = False

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
            file=sys.stderr,
        )

        self.serial.write(command.encode("ascii"))
        self.serial.flush()

    def press(self, scancode, text=0, delay=0.08):
        self.send_event(scancode, True, text)
        time.sleep(delay)
        self.send_event(scancode, False, 0)
        time.sleep(delay)

    def why(self, letter):
        letter = letter.upper()
        why_scancode = 0xE3
        key_scancode = 0x04 + ord(letter) - ord("A")

        self.send_event(why_scancode, True, 0)
        time.sleep(0.08)
        self.send_event(
            key_scancode,
            True,
            ord(letter.lower()),
        )
        time.sleep(0.08)
        self.send_event(key_scancode, False, 0)
        time.sleep(0.08)
        self.send_event(why_scancode, False, 0)
        time.sleep(0.30)

    def settle(self, seconds=0.5):
        self.serial.flush()
        time.sleep(seconds)


    def capture_screenshot(self, output_path, timeout=None, retries=2):
        if timeout is None:
            timeout = CONFIG["screenshots"]["timeout"]

        last_error = None

        for attempt in range(1, retries + 2):
            try:
                if attempt > 1:
                    print(
                        (
                            f"[SCREENSHOT] Retrying transfer "
                            f"(attempt {attempt}/{retries + 1})..."
                        ),
                        file=sys.stderr
                    )
                    time.sleep(0.5)

                return self._capture_screenshot_once(
                    output_path,
                    timeout
                )

            except (
                ValueError,
                TimeoutError,
                RuntimeError,
            ) as exc:
                last_error = exc

                print(
                    f"[SCREENSHOT] Transfer error: {exc}",
                    file=sys.stderr
                )

        raise RuntimeError(
            (
                f"Screenshot transfer failed after "
                f"{retries + 1} attempts: {last_error}"
            )
        )


    def _capture_screenshot_once(self, output_path, timeout=None):
        if timeout is None:
            timeout = CONFIG["screenshots"]["timeout"]

        output_path = Path(output_path)
        self.clear_log()

        print(
            f"\n[SCREENSHOT] Requesting {output_path}",
            file=sys.stderr
        )

        self.why("S")

        deadline = time.monotonic() + timeout
        begin_index = None
        begin_match = None
        end_index = None
        end_match = None
        error_line = None

        begin_re = re.compile(
            r"IMG BEGIN (\d+) (\d+) RGB24 RLE5FEC1 (\d+) (\d+)"
        )
        end_re = re.compile(
            r"IMG END (\d+) ([0-9A-Fa-f]{8}) (\d+)"
        )

        while time.monotonic() < deadline:
            lines = self.get_lines()

            for index, line in enumerate(lines):
                stripped = line.strip()

                if begin_index is None:
                    match = begin_re.fullmatch(stripped)
                    if match:
                        begin_index = index
                        begin_match = match
                        continue

                if stripped.startswith("IMG ERROR "):
                    error_line = stripped
                    break

                if begin_index is not None:
                    match = end_re.fullmatch(stripped)
                    if match:
                        end_index = index
                        end_match = match
                        break

            if error_line:
                raise RuntimeError(
                    f"Badge screenshot failed: {error_line}"
                )

            if (
                begin_index is not None
                and end_index is not None
                and begin_match is not None
                and end_match is not None
            ):
                break

            time.sleep(0.05)

        if (
            begin_index is None
            or end_index is None
            or begin_match is None
            or end_match is None
        ):
            raise TimeoutError(
                "Timed out waiting for complete IMG screenshot transfer"
            )

        width = int(begin_match.group(1))
        height = int(begin_match.group(2))
        chunk_size = int(begin_match.group(3))
        group_size = int(begin_match.group(4))
        declared_size = int(end_match.group(1))
        declared_crc = int(end_match.group(2), 16)
        declared_chunks = int(end_match.group(3))

        if chunk_size <= 0 or group_size <= 0:
            raise ValueError(
                "Invalid screenshot FEC parameters from badge"
            )

        data_chunks = {}
        parity_chunks = {}
        damaged_records = 0

        data_re = re.compile(
            r"IMG D (\d{6}) (\d{2}) ([0-9A-Fa-f]{8}) ([A-Za-z0-9+/=]+)"
        )
        parity_re = re.compile(
            r"IMG P (\d{6}) ([0-9A-Fa-f]{8}) ([A-Za-z0-9+/=]+)"
        )

        for line in lines[begin_index + 1:end_index]:
            stripped = line.strip()
            match = data_re.fullmatch(stripped)

            if match:
                sequence = int(match.group(1))
                declared_length = int(match.group(2))
                expected_crc = int(match.group(3), 16)

                if sequence >= declared_chunks:
                    continue

                try:
                    chunk = base64.b64decode(
                        match.group(4),
                        validate=True
                    )
                except Exception:
                    damaged_records += 1
                    continue

                if len(chunk) != declared_length:
                    damaged_records += 1
                    continue

                if declared_length <= 0 or declared_length > chunk_size:
                    damaged_records += 1
                    continue

                actual_crc = zlib.crc32(chunk) & 0xFFFFFFFF
                if actual_crc != expected_crc:
                    damaged_records += 1
                    continue

                data_chunks.setdefault(sequence, chunk)
                continue

            match = parity_re.fullmatch(stripped)
            if match:
                group = int(match.group(1))
                expected_crc = int(match.group(2), 16)

                try:
                    parity = base64.b64decode(
                        match.group(3),
                        validate=True
                    )
                except Exception:
                    damaged_records += 1
                    continue

                if len(parity) != chunk_size:
                    damaged_records += 1
                    continue

                actual_crc = zlib.crc32(parity) & 0xFFFFFFFF
                if actual_crc != expected_crc:
                    damaged_records += 1
                    continue

                parity_chunks.setdefault(group, parity)

        repaired_chunks = 0
        total_groups = (
            declared_chunks + group_size - 1
        ) // group_size

        for group in range(total_groups):
            first_sequence = group * group_size
            last_sequence = min(
                first_sequence + group_size,
                declared_chunks
            )
            group_sequences = list(
                range(first_sequence, last_sequence)
            )
            missing = [
                sequence
                for sequence in group_sequences
                if sequence not in data_chunks
            ]

            if not missing:
                continue

            if len(missing) != 1 or group not in parity_chunks:
                raise ValueError(
                    (
                        f"Screenshot FEC could not repair group {group}: "
                        f"missing chunks {missing}, "
                        f"parity={'yes' if group in parity_chunks else 'no'}"
                    )
                )

            missing_sequence = missing[0]
            recovered = bytearray(parity_chunks[group])

            for sequence in group_sequences:
                if sequence == missing_sequence:
                    continue

                chunk = data_chunks[sequence]
                for index, value in enumerate(chunk):
                    recovered[index] ^= value

            if missing_sequence == declared_chunks - 1:
                missing_length = (
                    declared_size
                    - chunk_size * (declared_chunks - 1)
                )
            else:
                missing_length = chunk_size

            if missing_length <= 0 or missing_length > chunk_size:
                raise ValueError(
                    (
                        "Screenshot FEC calculated invalid recovered "
                        f"chunk length {missing_length} for sequence "
                        f"{missing_sequence}"
                    )
                )

            data_chunks[missing_sequence] = bytes(
                recovered[:missing_length]
            )
            repaired_chunks += 1

        still_missing = [
            sequence
            for sequence in range(declared_chunks)
            if sequence not in data_chunks
        ]

        if still_missing:
            raise ValueError(
                f"Screenshot still has missing chunks: {still_missing[:12]}"
            )

        compressed = b"".join(
            data_chunks[sequence]
            for sequence in range(declared_chunks)
        )

        if len(compressed) != declared_size:
            raise ValueError(
                (
                    f"Screenshot compressed-size mismatch: got "
                    f"{len(compressed)}, expected {declared_size}"
                )
            )

        actual_crc = zlib.crc32(compressed) & 0xFFFFFFFF
        if actual_crc != declared_crc:
            raise ValueError(
                (
                    f"Screenshot CRC32 mismatch after FEC: got "
                    f"{actual_crc:08X}, expected {declared_crc:08X}"
                )
            )

        rgb = decode_rle5(compressed, width, height)
        write_png_rgb(output_path, width, height, rgb)

        print(
            (
                f"[SCREENSHOT] Saved {output_path} "
                f"({width}x{height}, "
                f"{len(compressed)} RLE bytes, "
                f"CRC32 {actual_crc:08X}, "
                f"FEC repaired {repaired_chunks} chunk(s), "
                f"discarded {damaged_records} damaged record(s))"
            ),
            file=sys.stderr
        )

        return {
            "path": str(output_path),
            "width": width,
            "height": height,
            "compressed_bytes": len(compressed),
            "crc32": f"{actual_crc:08X}",
            "fec_repaired_chunks": repaired_chunks,
            "damaged_records": damaged_records,
        }

    def enter(self):
        self.press(0x28)

    def backspace(self):
        self.press(0x2A)

    def wait_for(self, pattern, timeout=None):
        if timeout is None:
            timeout = DEFAULT_TIMEOUT

        print(
            f"\n[WAIT] {pattern}",
            file=sys.stderr,
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
                        file=sys.stderr,
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
                delay=0.04,
            )
            time.sleep(key_delay)


def ascii_scancode(c):
    if "a" <= c <= "z":
        return 0x04 + ord(c) - ord("a")

    if "A" <= c <= "Z":
        return 0x04 + ord(c) - ord("A")

    if "1" <= c <= "9":
        return 0x1E + ord(c) - ord("1")

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


# ---------------------------------------------------------------------------
# CONTENT PARSING
# ---------------------------------------------------------------------------

def latest_content_block(badge):
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

    return blocks[-1]


def numbered_actions(content):
    actions = []

    for line in content:
        cleaned = line.strip()

        if not cleaned:
            continue

        matches = list(
            re.finditer(
                r"\[(\d+)\]",
                cleaned,
            )
        )

        for index, match in enumerate(matches):
            number = int(match.group(1))
            label_start = match.end()

            if index + 1 < len(matches):
                label_end = matches[index + 1].start()
            else:
                label_end = len(cleaned)

            label = cleaned[
                label_start:label_end
            ].strip()

            actions.append(
                {
                    "number": number,
                    "label": label,
                    "raw": cleaned,
                }
            )

    return actions


def normalize_control_text(text):
    text = text.strip().lower()
    text = text.strip("[]").strip()

    return re.sub(
        r"\s+",
        " ",
        text,
    )


def find_search_controls(content, submit_label):
    actions = numbered_actions(content)

    wanted = normalize_control_text(
        submit_label
    )

    submit_index = None

    for index, action in enumerate(actions):
        label = normalize_control_text(
            action["label"]
        )

        if label == wanted or wanted in label:
            submit_index = index
            break

    if submit_index is None:
        rendered = " | ".join(
            (
                f"[{action['number']}]"
                f"{action['label']}"
            )
            for action in actions[:30]
        )

        raise RuntimeError(
            (
                "Could not find submit control "
                f"containing {submit_label!r}. "
                f"Visible actions: {rendered}"
            )
        )

    submit_number = actions[
        submit_index
    ]["number"]

    field_number = None

    for index in range(
        submit_index - 1,
        -1,
        -1,
    ):
        action = actions[index]
        low = normalize_control_text(
            action["label"]
        )

        if (
            low.startswith("q:")
            or low.startswith("query:")
            or low.startswith("search:")
            or low == "q"
            or low == "query"
        ):
            field_number = action["number"]
            break

    if field_number is None:
        rendered = " | ".join(
            (
                f"[{action['number']}]"
                f"{action['label']}"
            )
            for action in actions[:30]
        )

        raise RuntimeError(
            (
                f"Found submit [{submit_number}] "
                f"{actions[submit_index]['label']!r}, "
                "but no preceding search field. "
                f"Visible actions: {rendered}"
            )
        )

    return field_number, submit_number



def looks_like_result_url(line):
    cleaned = line.strip()

    if not cleaned:
        return False

    low = cleaned.lower()

    if low.startswith("http://") or low.startswith("https://"):
        return True

    if low.startswith("www."):
        return True

    if " › " in cleaned:
        return True

    if re.match(
        r"^[a-z0-9][a-z0-9.-]*\.[a-z]{2,}(?:\s|$)",
        low,
    ):
        return True

    return False


def extract_search_results(badge):
    content = latest_content_block(badge)
    results = []
    index = 0

    ignored_labels = {
        "google",
        "wiby",
        "settings",
        "search",
        "submit",
        "afbeeldingen",
        "video's",
        "maps",
        "nieuws",
        "boeken",
        "zoektools",
        "alles bekijken",
        "volgende >",
        "meer informatie",
        "inloggen",
        "privacy",
        "voorwaarden",
        "donker thema: uit",
        "find more...",
    }

    while index < len(content):
        cleaned = content[index].strip()

        match = re.match(
            r"^\[(\d+)\]\s*(.*)$",
            cleaned,
        )

        if not match:
            index += 1
            continue

        number = int(match.group(1))
        title = match.group(2).strip()
        title_index = index

        if not title:
            probe = index + 1

            while probe < len(content):
                candidate = content[probe].strip()

                if not candidate:
                    probe += 1
                    continue

                if re.match(r"^\[\d+\]", candidate):
                    break

                title = candidate
                title_index = probe
                break

            if not title:
                index += 1
                continue

        low = title.lower()

        if (
            low in ignored_labels
            or "[submit]" in low
            or low.startswith("q:")
            or low.startswith("query:")
            or low.startswith("search:")
        ):
            index += 1
            continue

        result_url = None

        for probe in range(
            title_index + 1,
            min(len(content), title_index + 7),
        ):
            candidate = content[probe].strip()

            if not candidate:
                continue

            if re.match(r"^\[\d+\]", candidate):
                break

            if looks_like_result_url(candidate):
                result_url = candidate
                break

        if result_url is not None:
            results.append(
                {
                    "number": number,
                    "title": title,
                    "url": result_url,
                }
            )

        index += 1

    return results


def extract_numbered_result_actions(
    badge,
    start_number,
    stop_number,
):
    content = latest_content_block(badge)
    numbers = []

    for line in content:
        for match in re.finditer(r"\[(\d+)\]", line):
            number = int(match.group(1))

            if start_number <= number <= stop_number:
                if number not in numbers:
                    numbers.append(number)

    return numbers


# ---------------------------------------------------------------------------
# BROWSER OPERATIONS
# ---------------------------------------------------------------------------

def go_home(badge):
    badge.clear_log()
    badge.why("H")
    badge.wait_for(
        CONFIG["home"]["url_pattern"],
        20,
    )
    badge.settle(1.0)


def activate_home_link(
    badge,
    action,
    expected_pattern,
):
    go_home(badge)
    badge.clear_log()

    badge.type_text(str(action))
    badge.settle(0.2)
    badge.enter()
    badge.settle(0.4)

    badge.wait_for(
        rf"activating link {action}",
        10,
    )

    badge.wait_for(
        expected_pattern,
        DEFAULT_TIMEOUT,
    )

    badge.settle(1.0)


def open_direct_url(
    badge,
    url,
    expected_pattern,
):
    go_home(badge)
    badge.clear_log()

    badge.why("E")
    badge.settle(0.5)

    # WHY+E already seeds "https://".
    if url.startswith("https://"):
        url = url[len("https://"):]

    badge.type_text(url)
    badge.settle(0.2)
    badge.enter()
    badge.settle(0.4)

    badge.wait_for(
        expected_pattern,
        DEFAULT_TIMEOUT,
    )

    badge.settle(1.0)


def perform_search(badge, config):
    if config["source"] == "home":
        go_home(badge)

    elif config["source"] == "home_link":
        activate_home_link(
            badge,
            config["home_action"],
            config["open_expect"],
        )

    else:
        raise ValueError(
            f"Unknown search source: {config['source']}"
        )

    content = latest_content_block(badge)

    field_number, submit_number = find_search_controls(
        content,
        config["submit_label"],
    )

    print(
        (
            f"\n[SEARCH FORM] field=[{field_number}] "
            f"submit=[{submit_number}]"
        ),
        file=sys.stderr,
    )

    badge.clear_log()

    badge.type_text(str(field_number))
    badge.settle(0.2)
    badge.enter()
    badge.settle(0.4)

    badge.type_text(config["query"])
    badge.settle(0.3)
    badge.enter()
    badge.settle(0.4)

    badge.type_text(str(submit_number))
    badge.settle(0.2)
    badge.enter()
    badge.settle(0.5)

    badge.wait_for(
        config["result_expect"],
        30,
    )

    badge.settle(2.0)

    query = config["query"]

    if not any(
        query.lower() in line.lower()
        for line in badge.get_lines()
    ):
        raise RuntimeError(
            f"HTTP succeeded, but query {query!r} "
            "was not visible in the result page"
        )

    results = extract_search_results(badge)

    details = [
        (
            f"Query: {query} "
            f"(field [{field_number}], submit [{submit_number}])"
        )
    ]

    if results:
        for result in results:
            details.append(
                (
                    f"[{result['number']}] "
                    f"{result['title']} -> "
                    f"{result['url']}"
                )
            )
        return details

    fallback = config.get("numbered_fallback")

    if fallback:
        numbers = extract_numbered_result_actions(
            badge,
            fallback["start"],
            fallback["stop"],
        )

        if len(numbers) >= fallback.get("minimum", 3):
            details.append(
                "Result actions: "
                + ", ".join(
                    f"[{number}]"
                    for number in numbers
                )
            )
            details.append(
                f"Detected {len(numbers)} numbered result actions"
            )
            return details

    raise RuntimeError(
        "Search HTTP request succeeded, but no usable "
        "search-result links/actions were detected"
    )


def command_setup(badge, setup):
    if setup is None:
        return

    if setup == "home":
        go_home(badge)
        return

    if setup == "home_then_wiby":
        activate_home_link(
            badge,
            5,
            r"HTTP 200.*https://wiby\.me",
        )
        return

    if setup == "home_wiby_back":
        activate_home_link(
            badge,
            5,
            r"HTTP 200.*https://wiby\.me",
        )

        badge.clear_log()
        badge.why("B")
        badge.wait_for(
            CONFIG["home"]["url_pattern"],
            20,
        )
        badge.settle(1.0)
        return

    raise ValueError(
        f"Unknown command setup: {setup}"
    )


# ---------------------------------------------------------------------------
# TEST FRAMEWORK
# ---------------------------------------------------------------------------

def run_test(
    results,
    number,
    description,
    test_func,
    badge=None,
    screenshot=False,
):
    print(
        f"\n\n========== TEST {number}: {description} ==========",
        file=sys.stderr,
    )

    details = []
    passed = False
    error = ""

    try:
        returned = test_func()

        if returned is None:
            returned = []

        elif isinstance(returned, str):
            returned = [returned]

        details.extend(
            list(returned)
        )
        passed = True

    except Exception as exc:
        error = str(exc)

    screenshot_config = CONFIG["screenshots"]

    screenshot_wanted = (
        screenshot_config["enabled"]
        and badge is not None
        and (
            (passed and screenshot)
            or (
                not passed
                and screenshot_config[
                    "screenshot_on_failure"
                ]
            )
        )
    )

    if screenshot_wanted:
        prefix = "" if passed else "FAILED-"

        output_path = (
            Path(
                screenshot_config["directory"]
            )
            / (
                f"{prefix}{number:02d}-"
                f"{safe_filename(description)}.png"
            )
        )

        try:
            shot = badge.capture_screenshot(
                output_path,
                screenshot_config["timeout"],
            )

            details.append(
                (
                    f"Screenshot: {shot['path']} "
                    f"({shot['width']}x{shot['height']}, "
                    f"CRC32 {shot['crc32']})"
                )
            )

        except Exception as shot_exc:
            details.append(
                f"Screenshot FAILED: {shot_exc}"
            )

    results.append(
        {
            "number": number,
            "description": description,
            "passed": passed,
            "error": error,
            "details": details,
        }
    )

    if passed:
        print(
            f"\nPASS: {description}",
            file=sys.stderr,
        )
    else:
        print(
            f"\nNOT PASSED: {description}",
            file=sys.stderr,
        )
        print(
            f"Reason: {error}",
            file=sys.stderr,
        )


def print_summary(results):
    print(
        "\n\n============================================================",
        file=sys.stderr,
    )
    print(
        "CONFIGURABLE MINI BROWSER TEST SUMMARY",
        file=sys.stderr,
    )
    print(
        "============================================================",
        file=sys.stderr,
    )

    for result in results:
        status = (
            "PASS"
            if result["passed"]
            else "NOT PASSED"
        )

        print(
            (
                f"{result['number']:>2}. "
                f"{status:<10} "
                f"{result['description']}"
            ),
            file=sys.stderr,
        )

        if result["error"]:
            print(
                f"    {result['error']}",
                file=sys.stderr,
            )

        for detail in result["details"]:
            print(
                f"    > {detail}",
                file=sys.stderr,
            )

    passed = sum(
        1
        for result in results
        if result["passed"]
    )

    total = len(results)

    print(
        "------------------------------------------------------------",
        file=sys.stderr,
    )
    print(
        f"Passed:     {passed}/{total}",
        file=sys.stderr,
    )
    print(
        f"Not passed: {total - passed}/{total}",
        file=sys.stderr,
    )
    print(
        "============================================================\n",
        file=sys.stderr,
    )


# ---------------------------------------------------------------------------
# MAIN
# ---------------------------------------------------------------------------

def main():
    badge = Badge()
    results = []
    number = 1

    print(
        "\nConfigurable Mini Browser regression test",
        file=sys.stderr,
    )
    print(
        f"Serial device: {DEVICE}",
        file=sys.stderr,
    )

    try:
        time.sleep(
            CONFIG["serial"]["startup_delay"]
        )

        # First verify that Mini Browser itself is alive.
        run_test(
            results,
            number,
            "Load Mini Browser home page",
            lambda: go_home(badge),
            badge=badge,
            screenshot=CONFIG["screenshots"]["after_home"],
        )
        number += 1

        # Configured sites.
        for site in CONFIG["sites"]:
            def site_test(site=site):
                mode = site["mode"]

                if mode == "url":
                    open_direct_url(
                        badge,
                        site["url"],
                        site["expect"],
                    )
                    return [
                        f"URL: {site['url']}"
                    ]

                if mode == "home_link":
                    activate_home_link(
                        badge,
                        site["action"],
                        site["expect"],
                    )
                    return [
                        f"Home action: [{site['action']}]"
                    ]

                raise ValueError(
                    f"Unknown site mode: {mode}"
                )

            run_test(
                results,
                number,
                f"Website: {site['name']}",
                site_test,
                badge=badge,
                screenshot=CONFIG["screenshots"]["after_sites"],
            )
            number += 1

        # Configured searches.
        for search_config in CONFIG["searches"]:
            run_test(
                results,
                number,
                f"Search: {search_config['name']}",
                lambda search_config=search_config: perform_search(
                    badge,
                    search_config,
                ),
                badge=badge,
                screenshot=CONFIG["screenshots"]["after_searches"],
            )
            number += 1

        # Configured badge/browser commands.
        for command_config in CONFIG["badge_commands"]:
            def command_test(
                command_config=command_config
            ):
                command_setup(
                    badge,
                    command_config.get("setup"),
                )

                badge.clear_log()
                badge.why(
                    command_config["command"]
                )

                badge.wait_for(
                    command_config["expect"],
                    DEFAULT_TIMEOUT,
                )

                badge.settle(0.8)

                return [
                    (
                        f"WHY+"
                        f"{command_config['command'].upper()}"
                    )
                ]

            run_test(
                results,
                number,
                (
                    "Badge command: "
                    f"{command_config['name']}"
                ),
                command_test,
                badge=badge,
                screenshot=CONFIG["screenshots"]["after_commands"],
            )
            number += 1

        print_summary(results)

        return (
            0
            if all(
                result["passed"]
                for result in results
            )
            else 1
        )

    finally:
        badge.close()


if __name__ == "__main__":
    sys.exit(main())
