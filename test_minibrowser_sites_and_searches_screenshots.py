#!/usr/bin/env python3

import base64
import re
import struct
import sys
import threading
import time
import zlib
from pathlib import Path

import serial


DEVICE = "/dev/cu.wchusbserial10"
BAUD = 115200

DEFAULT_TIMEOUT = 20.0

SCREENSHOTS_ENABLED = True
SCREENSHOT_DIRECTORY = Path("screenshots")
SCREENSHOT_TIMEOUT = 150.0
# True = WHY+Z complete page; False = WHY+S visible viewport only.
SCREENSHOT_FULL_PAGE = True
SCREENSHOT_ON_PASS = True
SCREENSHOT_ON_FAILURE = True


def safe_filename(text):
    value = re.sub(
        r"[^A-Za-z0-9._-]+",
        "-",
        text.strip().lower()
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
        rows.append(0)  # PNG filter type 0
        start = y * stride
        rows.extend(rgb[start:start + stride])

    png = bytearray(b"\x89PNG\r\n\x1a\n")

    png.extend(
        png_chunk(
            b"IHDR",
            struct.pack(
                ">IIBBBBB",
                width,
                height,
                8,   # bit depth
                2,   # truecolour RGB
                0,
                0,
                0,
            )
        )
    )

    png.extend(
        png_chunk(
            b"IDAT",
            zlib.compress(bytes(rows), 9)
        )
    )

    png.extend(
        png_chunk(
            b"IEND",
            b""
        )
    )

    path.parent.mkdir(
        parents=True,
        exist_ok=True
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
                        # Full-page screenshots can produce tens of thousands
                        # of IMG records. Keep a large host-side history so
                        # the beginning of a long transfer is not discarded.
                        if len(self.lines) > 250000:
                            self.lines = self.lines[-225000:]

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


    def capture_screenshot(
        self,
        output_path,
        timeout=SCREENSHOT_TIMEOUT,
        retries=2,
        full_page=SCREENSHOT_FULL_PAGE,
    ):
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
                    timeout,
                    full_page,
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


    def _capture_screenshot_once(
        self,
        output_path,
        timeout=SCREENSHOT_TIMEOUT,
        full_page=SCREENSHOT_FULL_PAGE,
    ):
        output_path = Path(output_path)

        self.clear_log()

        mode = "full page (WHY+Z)" if full_page else "viewport (WHY+S)"
        print(
            f"\n[SCREENSHOT] Requesting {output_path} [{mode}]",
            file=sys.stderr
        )

        self.why("Z" if full_page else "S")

        # Treat timeout as inactivity once data starts arriving.
        deadline = time.monotonic() + timeout
        last_line_count = 0
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

            if len(lines) != last_line_count:
                last_line_count = len(lines)
                deadline = time.monotonic() + timeout

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

        rgb = decode_rle5(
            compressed,
            width,
            height
        )

        write_png_rgb(
            output_path,
            width,
            height,
            rgb
        )

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



SCREENSHOT_BADGE = None


def run_test(results, number, description, test_func):
    step(number, description)

    details = []
    functional_passed = False
    error = ""

    try:
        returned_details = test_func()

        if returned_details is None:
            returned_details = []

        elif isinstance(returned_details, str):
            returned_details = [returned_details]

        details.extend(
            list(returned_details)
        )

        functional_passed = True

    except Exception as exc:
        error = str(exc)

    screenshot_wanted = (
        SCREENSHOTS_ENABLED
        and SCREENSHOT_BADGE is not None
        and (
            (functional_passed and SCREENSHOT_ON_PASS)
            or (
                not functional_passed
                and SCREENSHOT_ON_FAILURE
            )
        )
    )

    if screenshot_wanted:
        prefix = (
            ""
            if functional_passed
            else "FAILED-"
        )

        filename = (
            f"{prefix}{number:02d}-"
            f"{safe_filename(description)}.png"
        )

        output_path = (
            SCREENSHOT_DIRECTORY
            / filename
        )

        try:
            shot = SCREENSHOT_BADGE.capture_screenshot(
                output_path,
                full_page=SCREENSHOT_FULL_PAGE,
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
            "passed": functional_passed,
            "error": error,
            "details": details,
        }
    )

    if functional_passed:
        print(
            f"\nPASS: {description}",
            file=sys.stderr
        )

    else:
        print(
            f"\nNOT PASSED: {description}",
            file=sys.stderr
        )

        print(
            f"Reason: {error}",
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
        match = re.match(r"^\[(\d+)\]\s*(.*)$", cleaned)

        if not match:
            continue

        actions.append(
            (
                int(match.group(1)),
                match.group(2).strip(),
                cleaned,
            )
        )

    return actions


def find_search_controls(content, submit_label):
    actions = numbered_actions(content)

    wanted = submit_label.lower()

    submit_index = None

    for index, (number, label, raw) in enumerate(actions):
        low = label.lower()

        if wanted in low:
            submit_index = index
            break

    if submit_index is None:
        rendered = " | ".join(
            raw
            for _, _, raw in actions[:30]
        )

        raise RuntimeError(
            f"Could not find submit control containing {submit_label!r}. "
            f"Visible actions: {rendered}"
        )

    submit_number = actions[submit_index][0]

    field_number = None

    # Bind the submit button to the nearest preceding text field.
    for index in range(submit_index - 1, -1, -1):
        number, label, raw = actions[index]
        low = label.lower()

        if (
            low.startswith("q:")
            or low.startswith("query:")
            or low.startswith("search:")
            or low == "q"
            or low == "query"
        ):
            field_number = number
            break

    if field_number is None:
        rendered = " | ".join(
            raw
            for _, _, raw in actions[:30]
        )

        raise RuntimeError(
            f"Found submit [{submit_number}] {actions[submit_index][1]!r}, "
            f"but no preceding search field. Visible actions: {rendered}"
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

    # Google often renders a domain/breadcrumb rather than a full URL.
    if " › " in cleaned:
        return True

    # Bare host/domain such as esp32.com or esp32.net.
    if re.match(
        r"^[a-z0-9][a-z0-9.-]*\.[a-z]{2,}(?:\s|$)",
        low
    ):
        return True

    return False


def extract_search_results(
    badge,
    query="esp32"
):
    content = latest_content_block(badge)
    results = []

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

    index = 0

    while index < len(content):
        cleaned = content[index].strip()

        match = re.match(
            r"^\[(\d+)\]\s*(.*)$",
            cleaned
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

        if low in ignored_labels:
            index += 1
            continue

        if "[submit]" in low:
            index += 1
            continue

        if low.startswith("q:"):
            index += 1
            continue

        if low.startswith("query:"):
            index += 1
            continue

        if low.startswith("search:"):
            index += 1
            continue

        result_url = None

        for probe in range(
            title_index + 1,
            min(len(content), title_index + 7)
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
    start_number=18,
    stop_number=46
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



def perform_search(
    badge,
    query,
    expected_http_pattern,
    submit_label,
    allow_numbered_fallback=False,
    fallback_start=18,
    fallback_stop=46
):
    content = latest_content_block(badge)

    field_number, submit_number = find_search_controls(
        content,
        submit_label
    )

    print(
        (
            f"\n[SEARCH FORM] field=[{field_number}] "
            f"submit=[{submit_number}]"
        ),
        file=sys.stderr
    )

    badge.clear_log()

    badge.type_text(str(field_number))
    badge.settle(0.2)
    badge.enter()
    badge.settle(0.4)

    badge.type_text(query)
    badge.settle(0.3)
    badge.enter()
    badge.settle(0.4)

    badge.type_text(str(submit_number))
    badge.settle(0.2)
    badge.enter()
    badge.settle(0.5)

    badge.wait_for(
        expected_http_pattern,
        30
    )

    badge.settle(2.0)

    all_lines = badge.get_lines()

    if not any(
        query.lower() in line.lower()
        for line in all_lines
    ):
        raise RuntimeError(
            f"Search returned HTTP 200, but no {query!r} text was found"
        )

    search_results = extract_search_results(
        badge,
        query=query
    )

    fallback_numbers = []

    if not search_results and allow_numbered_fallback:
        fallback_numbers = extract_numbered_result_actions(
            badge,
            start_number=fallback_start,
            stop_number=fallback_stop
        )

        # Require several result actions so a stray navigation link cannot
        # accidentally make the search pass.
        if len(fallback_numbers) < 3:
            fallback_numbers = []

    if not search_results and not fallback_numbers:
        content = latest_content_block(badge)

        visible = " | ".join(
            line.strip()
            for line in content[:60]
            if line.strip()
        )

        raise RuntimeError(
            (
                "Search returned HTTP 200, but no usable search-result "
                f"actions were captured. Page starts with: {visible}"
            )
        )

    return (
        field_number,
        submit_number,
        search_results,
        fallback_numbers,
    )




def format_search_result_lines(search_results):
    lines = []

    for result in search_results:
        lines.append(
            (
                f"[{result['number']}] "
                f"{result['title']} -> "
                f"{result['url']}"
            )
        )

    return lines



def main():
    global SCREENSHOT_BADGE

    badge = Badge()
    SCREENSHOT_BADGE = badge
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

            (
                field_number,
                submit_number,
                search_results,
                fallback_numbers,
            ) = perform_search(
                badge,
                "esp32",
                r"HTTP 200.*wiby\.me",
                submit_label="[search]"
            )

            return [
                (
                    f"Wiby query: esp32 "
                    f"(field [{field_number}], submit [{submit_number}])"
                ),
                *format_search_result_lines(search_results),
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

            (
                field_number,
                submit_number,
                search_results,
                fallback_numbers,
            ) = perform_search(
                badge,
                "esp32",
                r"HTTP 200.*google",
                submit_label="[google search]",
                allow_numbered_fallback=True,
                fallback_start=18,
                fallback_stop=46
            )

            details = [
                (
                    f"Google query: esp32 "
                    f"(field [{field_number}], submit [{submit_number}])"
                )
            ]

            if search_results:
                details.extend(
                    format_search_result_lines(search_results)
                )
            else:
                details.append(
                    (
                        "Google result actions: "
                        + ", ".join(
                            f"[{number}]"
                            for number in fallback_numbers
                        )
                    )
                )

                details.append(
                    (
                        f"Detected {len(fallback_numbers)} numbered "
                        "result actions in the Google result area"
                    )
                )

            return details

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
        SCREENSHOT_BADGE = None
        badge.close()


if __name__ == "__main__":
    sys.exit(main())
