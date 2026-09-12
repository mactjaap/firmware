# Mini Browser 2.3

A compact, interactive, text-oriented web browser for the WHY2025 badge.

Mini Browser is written in C using SDL3 and libcurl. It retrieves HTML pages, converts them into readable text, extracts links and simple HTML forms, and provides a keyboard-driven browsing interface designed for the 720×720 WHY2025 badge display.

Version **2.3** adds broad Unicode rendering, monochrome Plane-1 emoji, UTF-8-safe pixel-aware wrapping, and genuine rendered bold text for HTML `<b>` and `<strong>`, while retaining the link navigation, bookmarks, Back/Forward history and GET forms from earlier releases.

Mini Browser deliberately does **not** try to be a modern graphical browser. There is no JavaScript engine, CSS layout engine, image renderer, or full DOM. The goal is a small, fast browser for text-oriented and lightweight websites.

## Highlights

- Text-oriented HTML browsing over HTTP and HTTPS
- Up to 64 KiB downloaded per page
- Up to 128 extracted links and 160 interactive actions
- Numbered navigation for links and form controls
- Back and Forward browsing history
- Persistent bookmarks
- Editable URL bar
- Hold Up/Down for fast scrolling
- Simple GET form support
- UTF-8-safe text processing and wrapping
- Pixel-aware wrapping for mixed ASCII and Unicode text
- Broad Unicode support using a generated GNU Unifont bitmap file
- 27,696 generated Unicode glyphs across selected Plane 0 and Plane 1 ranges
- CJK, Greek, Cyrillic, punctuation, symbols and many other scripts/blocks
- Monochrome single-codepoint emoji
- Real rendered bold for `<b>` and `<strong>`
- ASCII `*` markers for unordered lists, so list markers remain usable without the external Unicode font
- Small built-in 5×7 ASCII bitmap font
- No JavaScript, CSS layout or image rendering required

## Unicode and text rendering

ASCII uses the browser's built-in bitmap font. Additional Unicode characters are loaded from:

    APPS:[mini_browser]unifont_cjk.bin

The generated font is based on GNU Unifont 17.0.02 and combines selected glyphs from the Japanese Plane-0 font and the supplementary-plane font.

The current generated font contains **27,696 glyphs** and covers broad ranges including Latin extensions, Greek, Cyrillic, punctuation, currency symbols, arrows, mathematical symbols, box drawing, geometric shapes, CJK punctuation, Hiragana, Katakana, CJK ideographs, fullwidth forms and selected supplementary symbol/emoji blocks.

UTF-8 wrapping is code-point safe: Mini Browser does not intentionally split a multi-byte UTF-8 sequence when wrapping page text. Wrapping is also pixel-aware, accounting for the different rendered widths of the built-in ASCII font and 16×16 Unicode glyphs.

If the external Unicode font is not installed, the built-in ASCII renderer remains available. Unordered HTML list items intentionally use the ASCII `*` marker rather than requiring a Unicode bullet.

## Bold text

HTML `<b>` and `<strong>` are rendered as genuine bold text rather than as visible Markdown-style `**` markers.

Mini Browser implements this in the renderer by drawing bold glyphs with an additional one-pixel horizontal pass. This works with both the built-in ASCII glyphs and glyphs loaded from the Unicode font.

## Emoji

Mini Browser 2.3 can render many supplementary-plane, single-codepoint emoji as monochrome GNU Unifont bitmap glyphs.

Examples include:

    😀 😃 😂 😎 🤖
    👍 👋 🙏
    🐶 🐱 🐼
    🍎 🍕 ☕
    🚗 ✈ 🚀
    🌍 🌙 🔥
    💡 💻 🔑

This is bitmap Unicode rendering, not a color emoji engine.

Mini Browser does not currently compose complex emoji sequences such as ZWJ sequences, skin-tone combinations, gender sequences or regional-indicator flag pairs. Variation-selector handling is also limited.

## HTML forms

Mini Browser supports simple interactive HTML GET forms.

Supported form controls include:

- `<input type="text">`
- `<input type="search">`
- `<input type="url">`
- `<input type="hidden">`
- `<input type="submit">`
- `<button>`
- `<button type="submit">`

Links and visible form controls share the same numbered action system. Type the action number and press Enter to activate it.

GET submissions use URL encoding compatible with `application/x-www-form-urlencoded`. Hidden fields are included, disabled fields are ignored, and the activated named submit button is included where appropriate.

Up to 4 forms with up to 8 stored fields per form are supported.

POST forms are recognised but intentionally not submitted. Complex controls such as `textarea`, `select`, checkboxes, radio buttons, file uploads and JavaScript-driven forms are not currently supported.

## HTML rendering

Mini Browser converts useful HTML structure into a compact text representation.

Supported or specially handled elements include headings, paragraphs, line breaks, ordered and unordered lists, preformatted text, inline code, bold/strong text, emphasis/italic text, horizontal rules, simple table rows/cells, hyperlinks, simple forms, named HTML entities, and decimal/hexadecimal numeric entities.

Unordered list items use `* ` as their marker. This is intentional: the marker works with the built-in ASCII font even when the optional external Unicode font is not installed.

Comments, doctypes, scripts, styles and document head content are ignored for normal page rendering. The page `<title>` is extracted for the top bar.

## Navigation

Every usable link or visible form control receives an action number. Type the number and press Enter to activate it.

### Keyboard controls

| Key | Action |
| --- | --- |
| `0`–`9` + Enter | Activate a numbered link or form action |
| `Enter` | Activate / accept editing |
| `Up` / `Down` | Scroll one line; hold for continuous scrolling |
| `J` / `K` | Scroll down / up one line |
| `Left` / `Right` | Move cursor while editing |
| `Backspace` | Delete while editing |

The WHY2025 key acts as the browser accelerator:

| Shortcut | Action |
| --- | --- |
| `WHY+E` | Enter a new URL |
| `WHY+C` | Edit the current URL |
| `WHY+H` | Home |
| `WHY+R` | Reload |
| `WHY+B` | Back |
| `WHY+G` | Forward |
| `WHY+F` | Add/remove current bookmark |
| `WHY+M` | Open bookmarks |
| `WHY+Q` | Quit |

## Bookmarks and history

Mini Browser stores up to 32 bookmarks and keeps up to 32 HTTP/HTTPS history entries.

`WHY+B` moves backward and `WHY+G` moves forward. Navigating to a new page after going Back truncates the old forward branch. Reloading does not create a duplicate history entry, and GET form submissions participate in the same history.

Bookmark data is stored at:

    APPS:[mini_browser]bookmarks.txt

## Networking

Mini Browser uses libcurl and requests HTTP/1.1 where available. It requests uncompressed transfer data with:

    Accept-Encoding: identity

Redirect following is bounded. Network failures and HTTP errors are shown as readable browser pages.

## Limits

| Resource | Limit |
| --- | ---: |
| Downloaded page data | 64 KiB |
| URL length | 256 bytes |
| Links | 128 |
| Interactive actions | 160 |
| Forms per page | 4 |
| Fields per form | 8 |
| Editable form value | 127 characters |
| Bookmarks | 32 |
| History entries | 32 |

## What Mini Browser does not support

Mini Browser does not currently provide JavaScript execution, CSS layout/styling, images, video/audio, POST form submission, file uploads, complex HTML form controls, a complete HTML5 DOM/parser, color emoji, or complex emoji composition.

Simple server-rendered websites and text-oriented sites work best.

## Architecture

    URL
      |
      v
    libcurl HTTP fetch
      |
      v
    bounded HTML-to-text parser
      |
      +--> links
      +--> forms
      +--> title
      +--> formatting markers
      |
      v
    UTF-8-safe, pixel-aware wrapping
      |
      v
    numbered action model
      |
      v
    SDL3 renderer
      |
      +--> built-in ASCII glyphs
      +--> external Unicode glyphs
      +--> real bold rendering

## Unicode font generation

The external font asset is generated from GNU Unifont 17.0.02 sources. The tested 2.3 font combines:

    unifont_jp-17.0.02.hex
    unifont_upper-17.0.02.hex

into:

    unifont_cjk.bin

The generated binary contains 27,696 usable glyphs in the selected ranges.

GNU Unifont is dual-licensed under the SIL Open Font License 1.1 and GNU GPL version 2 or later with the GNU Font Embedding Exception. When redistributing the generated font asset, include the applicable GNU Unifont licensing and attribution material.

## Testing and macOS keyboard bridge

The repository includes three Python utilities for driving and testing Mini Browser through the badge's serial keyboard bridge:

- `badge_keyboard.py` — interactive macOS keyboard control
- `test_minibrowser_sites_and_searches.py` — fixed automated regression suite
- `test_minibrowser_configurable_test.py` — configurable automated test runner

The scripts use the serial keyboard event protocol implemented by the WHY2025 BadgeVMS keyboard driver. They send synthetic keyboard events over the badge's serial connection; BadgeVMS then delivers those events through the normal keyboard/event path to SDL3 and Mini Browser. The physical badge keyboard remains usable.

The scripts require Python 3 and pyserial:

    python3 -m pip install pyserial

By default the supplied scripts use:

    /dev/cu.wchusbserial10
    115200 baud

Change the serial device in the script if the badge appears under another device name. On macOS, available serial devices can be checked with:

    ls /dev/cu.*

Only one program should have the serial device open at a time. Stop a serial monitor or another test script before starting one of these tools.

### badge_keyboard.py

`badge_keyboard.py` turns the Mac keyboard into an interactive keyboard for the WHY2025 badge.

Run:

    ./badge_keyboard.py

or:

    python3 badge_keyboard.py

Normal printable characters are translated to BadgeVMS/HID-style scancodes and sent to the badge. Enter, Tab, Backspace, Delete, Home, End and the arrow keys are also translated.

Control-key combinations are used to generate Mini Browser WHY-key shortcuts:

| Mac key | Badge action |
| --- | --- |
| `Ctrl-E` | `WHY+E` — enter a new URL |
| `Ctrl-H` | `WHY+H` — home |
| `Ctrl-R` | `WHY+R` — reload |
| `Ctrl-B` | `WHY+B` — back |
| `Ctrl-G` | `WHY+G` — forward |
| `Ctrl-F` | `WHY+F` — add/remove bookmark |
| `Ctrl-Q` | `WHY+Q` — quit |
| `Ctrl-]` | Exit `badge_keyboard.py` |

The script also displays serial output from the badge, making it useful for interactive testing and debugging.

The serial protocol uses lines in this form:

    E <scancode-hex> <down> <text-hex>

For example, an Enter key press is sent as:

    E 28 1 00
    E 28 0 00

The first event is key-down and the second is key-up.

### test_minibrowser_sites_and_searches.py

`test_minibrowser_sites_and_searches.py` is the fixed Mini Browser regression/smoke-test suite. It is intended to be run after browser, BadgeVMS, SDL3, networking or keyboard changes to quickly verify that the known working feature set still behaves correctly.

Run:

    ./test_minibrowser_sites_and_searches.py

or:

    python3 test_minibrowser_sites_and_searches.py

The suite automatically drives Mini Browser through the same serial keyboard event path as a real user. It tests:

- Loading the Mini Browser home page
- Recommended home-page links:
  - Wiby
  - Marginalia Search
  - FrogFind
  - Hacker News
  - NPR Text
  - TEXTFILES.COM
  - curl
  - ifconfig.co
- Back history with `WHY+B`
- Forward history with `WHY+G`
- Reload with `WHY+R`
- Opening an arbitrary URL with `WHY+E`
- Editing the current URL with `WHY+C`
- A real Wiby search for `esp32`
- A real Google search for `esp32`
- Returning home with `WHY+H`

The search tests discover the numbered form field and submit controls, enter the query and submit the GET form. When result titles and destination URLs can be extracted they are printed in the summary. Google can return a more compact result representation, so the Google test also accepts a sufficiently large numbered result-action area as evidence that the search succeeded.

Each test is isolated as much as practical and failures do not immediately stop the complete suite. At the end a summary shows every test as `PASS` or `NOT PASSED`, together with search-result details where available.

The process exits with status `0` when every test passes and status `1` when one or more tests fail. This makes the script suitable for repeatable post-update regression testing.

### test_minibrowser_configurable_test.py

`test_minibrowser_configurable_test.py` uses the same serial keyboard automation but is designed for experimenting with additional websites, searches and browser commands without rewriting the test engine.

Run:

    ./test_minibrowser_configurable_test.py

or:

    python3 test_minibrowser_configurable_test.py

Normal changes are made only in the `CONFIG` dictionary near the top of the script.

Serial settings are configurable:

    "serial": {
        "device": "/dev/cu.wchusbserial10",
        "baudrate": 115200,
        "startup_delay": 2.0,
        "default_timeout": 25,
    }

Website tests can either open a URL directly with `WHY+E`:

    {
        "name": "Example.com",
        "mode": "url",
        "url": "example.com",
        "expect": r"HTTP 200.*https://example\.com",
    }

or activate a numbered link from the Mini Browser home page:

    {
        "name": "Wiby",
        "mode": "home_link",
        "action": 5,
        "expect": r"HTTP 200.*https://wiby\.me",
    }

Search tests describe where the form is located, the query to enter, the submit-control label and the expected HTTP result. For example:

    {
        "name": "Wiby search",
        "source": "home_link",
        "home_action": 5,
        "open_expect": r"HTTP 200.*https://wiby\.me",
        "query": "esp32",
        "submit_label": "[search]",
        "result_expect": r"HTTP 200.*wiby\.me",
        "numbered_fallback": None,
    }

The configurable Google test demonstrates `numbered_fallback`. This is useful when Google returns valid numbered result actions but the serial text representation does not contain enough title/URL text for the richer result parser.

Badge command tests are also configured as data:

    {
        "name": "Reload",
        "command": "R",
        "setup": "home",
        "expect": r"HTTP 200.*https://minibrowser\.macip\.net",
    }

The current built-in setup helpers are:

- `home`
- `home_then_wiby`
- `home_wiby_back`

These make it possible to establish a known browser state before testing commands such as Back and Forward.

`WHY+Q` is intentionally not enabled in the default command list. If a Quit test is added, keep it as the final test because it terminates Mini Browser.

Like the fixed regression suite, the configurable runner prints badge serial output while running, records individual failures, continues with subsequent tests where possible, and finishes with a `PASS` / `NOT PASSED` summary.

### Which test script should I use?

Use `test_minibrowser_sites_and_searches.py` as the stable regression test after an update. Because its test set is fixed, a change from an all-PASS run is easy to spot.

Use `test_minibrowser_configurable_test.py` when testing new sites, different search queries, new search engines or additional WHY-key commands.

Use `badge_keyboard.py` when you want to operate Mini Browser manually from the Mac keyboard while watching the badge's serial/debug output.

## Building

Mini Browser is part of the WHY2025 BadgeVMS firmware tree. Build it with the existing WHY2025 ESP-IDF project configuration.

Do not casually regenerate the ESP32-P4 target configuration on early P4 badge hardware.

## Project

Source repository:

    https://github.com/mactjaap/mini_browser/

Home page:

    https://minibrowser.macip.net/

## Version

**Mini Browser 2.3**

Version 2.3 combines the stable interactive browser foundation with broad Unicode/emoji rendering, UTF-8-safe pixel-aware wrapping and genuine bold HTML text rendering.
