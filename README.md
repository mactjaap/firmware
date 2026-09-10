# MacTjaap WHY2025 BadgeVMS Firmware

This repository contains my customized build of **BadgeVMS** for the
ESP32-P4 based WHY2025 badge.

It is based on the original WHY2025 BadgeVMS firmware, but includes my
own launcher/UI changes, application selection and ordering, and the
latest **Mini Browser 2.3** with extended Unicode support.

![BadgeVMS Logo](misc/BadgeVMS.png)

## What is different in this build?

The goal of this fork is to provide a polished, useful everyday BadgeVMS
installation with a cleaner application launcher and a strong set of
preinstalled applications.

### Customized launcher and UI

The launcher has been reworked from the original BadgeVMS presentation.
Changes include:

-   A customized dark launcher/interface.
-   Custom application icons.
-   A full-screen application menu designed for the badge's 720x720
    display.
-   Improved application ordering, with frequently used applications
    placed prominently.
-   **Mini Browser** is presented as one of the primary applications.
-   Launcher/internal support applications that do not need to be
    started manually are hidden from the normal application list.
-   Updated About/information presentation and launcher navigation.

### Mini Browser 2.3

This firmware includes **Mini Browser 2.3**, my lightweight browser
designed specifically for BadgeVMS.

Mini Browser 2.3 includes:

-   HTTP and HTTPS browsing using libcurl.
-   HTML text rendering.
-   Link discovery and numbered link navigation.
-   Back and Forward history.
-   Persistent bookmarks.
-   HTML GET forms, including text, search, URL, hidden and submit
    fields.
-   UTF-8-safe text processing.
-   Pixel-aware line wrapping for mixed-width text.
-   Broad Unicode support using an external GNU Unifont-derived bitmap
    font.
-   27,696 Unicode glyphs in the generated font.
-   Japanese, Chinese, Greek, Cyrillic, punctuation, mathematical
    symbols and many other Unicode ranges.
-   Monochrome single-codepoint Plane-1 emoji.
-   Real rendered bold text for HTML `<b>` and `<strong>`.
-   Compatibility-friendly ASCII list markers when the external Unicode
    font is unavailable.

The Unicode font is included in the firmware storage so a normal
build/flash of this firmware provides the extended Mini Browser
character set.

Complex color emoji, ZWJ emoji sequences, JavaScript, CSS layout and
graphical web pages are intentionally outside the scope of Mini Browser.
It is designed as a fast, text-oriented browser for the badge.

Mini Browser development is also maintained separately at:

https://github.com/mactjaap/mini_browser

## BadgeVMS

BadgeVMS is an operating environment for the WHY2025 badge.

Some of its main features are:

-   Multiple programs can run at once.
-   Every program gets its own linear address space.
-   Programs are isolated from each other, although not from the
    operating system itself.
-   VMS-style paths and search lists.
-   Applications are loaded as position-independent RISC-V ELF binaries.
-   SDL2 and SDL3 are available to applications through the BadgeVMS
    SDK.

## Supported hardware

-   WHY2025 badge (ESP32-P4 based)

## Building the firmware

This firmware is built using **ESP-IDF 5.5**.

Activate your ESP-IDF environment first. For example:

``` sh
. ~/esp-idf/export.sh
```

Then build the firmware:

``` sh
idf.py build
```

To build, flash and open the serial monitor:

``` sh
idf.py build flash monitor
```

Or specify the serial device explicitly, for example:

``` sh
idf.py -p /dev/ttyUSB0 flash monitor
```

> **Important:** this firmware has been developed and tested with the
> WHY2025 ESP32-P4 badge configuration. Do not casually regenerate the
> project configuration or run `idf.py set-target esp32p4` on an
> existing working checkout. The WHY2025 badge hardware/configuration
> must be preserved.

When pulling firmware changes that modify `sdkconfig.defaults`, a clean
rebuild may be necessary:

``` sh
idf.py fullclean
idf.py build
```

## Firmware storage and applications

BadgeVMS uses a storage image containing the preinstalled applications
and their assets.

Mini Browser's Unicode font is installed as an application asset and is
available inside BadgeVMS as:

``` text
APPS:[mini_browser]unifont_cjk.bin
```

BadgeVMS paths are **not UNIX paths**. They use VMS-style syntax:

``` text
DEVICE:[directory.subdirectory]filename.ext
```

## Example applications

The [`sdk_apps`](sdk_apps) directory contains many BadgeVMS applications
and examples.

Some useful SDK examples include:

-   [`framebuffer_test`](sdk_apps/framebuffer_test) - direct interaction
    with the windowing system and keyboard input.
-   [`sdl_test`](sdk_apps/sdl_test) - similar functionality using SDL3.
-   [`sdl2_test`](sdk_apps/sdl2_test) - SDL2 example.
-   [`curl_test`](sdk_apps/curl_test) - HTTP(S) requests.
-   [`thread_test`](sdk_apps/thread_test) - thread creation and worker
    interaction.
-   [`doomgeneric`](sdk_apps/doomgeneric) - a complete Doom port and a
    useful reference for framebuffers, scaling, window handling and
    input.

The customized firmware also contains a broader selection of practical,
system and demonstration applications for the badge.

## Building BadgeVMS applications

BadgeVMS provides an SDK containing the BadgeVMS headers and libraries,
including SDL3 and SDL2.

Build the SDK with:

``` sh
idf.py sdk
```

This generates the `sdk_dist` directory containing the headers and
libraries.

Applications are RISC-V position-independent ELF shared objects. You can
use the `riscv32-esp-elf-*` toolchain supplied with ESP-IDF.

Example:

``` sh
riscv32-esp-elf-gcc -O2 -fPIC -fdata-sections -ffunction-sections -flto \
   -fno-builtin -fno-builtin-function -fno-jump-tables -fno-tree-switch-conversion \
   -fstrict-volatile-bitfields -fvisibility=hidden -g3 -mabi=ilp32f \
   -march=rv32imafc_zicsr_zifencei -nostartfiles -nostdlib -shared \
   -Wl,--strip-debug -Wl,--gc-sections -e main --sysroot sdk_dist -isystem sdk_dist/include \
   hello.c -o hello.elf
```

A suitable `riscv64-linux-gnu-gcc` can also be used; GCC is the tested
compiler family.

## Linking with SDK libraries

Because of limitations in the BadgeVMS ELF loader, applications should
not expose symbols other than `main`.

The standard build flags use:

``` text
-fvisibility=hidden
```

When linking an additional static `.a` library, use:

``` text
-Wl,--exclude-libs,libmylib.a
```

All dependencies not supplied by the BadgeVMS SDK must be statically
linked. BadgeVMS does not provide conventional shared-library loading or
`dlopen()`.

## Important BadgeVMS details

-   UNIX paths do not work inside BadgeVMS.
-   Paths use `DEVICE:[directory.subdirectory]filename.ext`.
-   BadgeVMS applications are position-independent ELF shared objects.
-   The RISC-V compiler and linker flags used by the SDK are important;
    ordinary executables will not load.
-   Dependencies outside the SDK must be statically linked.
-   Firmware and storage-image changes should be tested on the actual
    WHY2025 badge.

## Upstream project

This repository is a customized firmware build based on the original
WHY2025 BadgeVMS project.

Original WHY2025 firmware:

https://gitlab.com/why2025/team-badge/firmware

My customized firmware:

https://github.com/mactjaap/firmware

Mini Browser:

https://github.com/mactjaap/mini_browser

## Credits

Many thanks to the WHY2025 badge team and the BadgeVMS developers for
creating the badge firmware, operating environment, SDK and application
ecosystem.

Mini Browser and the firmware customizations in this repository are
maintained by **MacTjaap**.
