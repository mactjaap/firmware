WHY2025 BADGE FIRMWARE - BUILD AND FLASH ON macOS
=================================================

Tested:
- WHY2025 badge
- ESP32-P4 revision v1.0
- ESP-IDF v5.5
- esptool.py v4.12.0
- macOS / Apple Silicon
- Mini Browser 1.4


1. INSTALL PREREQUISITES
========================

Install Apple's command-line development tools:

xcode-select --install

Install Homebrew if it is not already installed.

Check:

brew --version

Install the required packages:

brew install cmake ninja dfu-util ccache python3


2. INSTALL ESP-IDF 5.5
======================

ESP-IDF only needs to be installed once.

Go to your home directory:

cd ~

Clone ESP-IDF version 5.5:

git clone -b v5.5 --recursive https://github.com/espressif/esp-idf.git

This creates:

~/esp-idf

Install the ESP-IDF tools:

cd ~/esp-idf
./install.sh


3. ACTIVATE ESP-IDF
===================

This must be done in every new Terminal session before using idf.py:

. ~/esp-idf/export.sh

Check:

idf.py --version

Expected:

ESP-IDF v5.5

Also useful:

esptool.py version


4. CLONE THE WHY2025 FIRMWARE
=============================

Go to your home directory:

cd ~

Clone the firmware:

git clone https://gitlab.com/tjabring/firmware.git

Enter the repository:

cd firmware

The customized/tested firmware is on branch UI-FINAL:

git checkout UI-FINAL

Check:

git status -sb
git log -5 --oneline


5. IMPORTANT: ESP32-P4 TARGET
=============================

DO NOT run:

idf.py set-target esp32p4

The project already selects the ESP32-P4 in CMakeLists.txt:

set(ENV{IDF_TARGET} esp32p4)

The firmware build also builds the ESP32-C6 connectivity firmware separately.

Do not manually change the target when using the known-good firmware configuration.


6. BUILD
========

Activate ESP-IDF first if this is a new Terminal:

. ~/esp-idf/export.sh

Then:

cd ~/firmware
idf.py build

A successful build creates, among other files:

build/bootloader/bootloader.bin
build/partition_table/partition-table.bin
build/ota_data_initial.bin
build/badgevms.bin
build/storage.bin


7. CONNECT THE BADGE
====================

Connect the SIDE USB-C port of the WHY2025 badge to the Mac.

Find the serial device:

ls /dev/cu.* | grep -E 'usb|wch|modem'

On the tested Mac it appeared as:

/dev/cu.wchusbserial110

The number can change, so always check.


8. VERIFY THAT IT IS THE ESP32-P4
=================================

Before flashing, verify the serial port:

esptool.py --port /dev/cu.wchusbserial110 chip_id

Replace /dev/cu.wchusbserial110 with the device found on your Mac.

Expected output includes:

Detecting chip type... ESP32-P4
Chip is ESP32-P4 (revision v1.0)

Tested badge MAC:

30:ed:a0:e1:a9:9b

DO NOT flash a port that identifies as ESP32-C6.


9. FLASH THE BADGE
==================

From the firmware directory:

cd ~/firmware

Make sure ESP-IDF is active:

. ~/esp-idf/export.sh

Then flash:

idf.py -p /dev/cu.wchusbserial110 flash

Again, replace the serial device with the actual ESP32-P4 device.


10. BUILD + FLASH IN ONE COMMAND
================================

Once the correct serial port is known:

idf.py -p /dev/cu.wchusbserial110 build flash


11. SERIAL MONITOR
==================

To watch the badge boot and application logs:

idf.py -p /dev/cu.wchusbserial110 monitor

Exit the monitor with:

Ctrl+]


12. BUILD + FLASH + MONITOR
===========================

For normal development:

idf.py -p /dev/cu.wchusbserial110 build flash monitor


13. MINI BROWSER SOURCE
=======================

Mini Browser source inside the complete firmware repository:

sdk_apps/mini_browser/mini_browser.c

Manifest:

sdk_apps/mini_browser/manifest.json

The standalone/public Mini Browser repository is:

https://github.com/mactjaap/mini_browser

Development and physical badge testing should first be done in:

~/firmware/sdk_apps/mini_browser/

After the change has been successfully built and tested on the physical
badge, copy the known-good Mini Browser source to the standalone GitHub
repository.


14. CURRENT KNOWN-GOOD VERSION
==============================

Firmware branch:

UI-FINAL

Mini Browser:

1.4

Mini Browser 1.4 includes URL editing with a visible cursor:

WHY+E    Start entering a new URL
WHY+C    Edit the current URL
LEFT     Move cursor left
RIGHT    Move cursor right
BACKSPACE
          Delete character before cursor
ENTER     Load URL

WHY+B    Go back
WHY+R    Reload
WHY+H    Home
WHY+Q    Quit


15. QUICK START - AFTER EVERYTHING IS INSTALLED
===============================================

For future sessions, normally only these commands are necessary:

. ~/esp-idf/export.sh
cd ~/firmware
git pull
git checkout UI-FINAL
idf.py build

Find/verify the badge if necessary:

esptool.py --port /dev/cu.wchusbserial110 chip_id

Then:

idf.py -p /dev/cu.wchusbserial110 flash


16. IMPORTANT NOTES
===================

- The main badge processor is the ESP32-P4.
- The WHY2025 badge also contains an ESP32-C6 connectivity processor.
- Use the SIDE USB-C connection for flashing the ESP32-P4 firmware.
- Always verify an unfamiliar serial device with esptool.py chip_id before
  flashing.
- Do not blindly use /dev/cu.wchusbserial110; the device number can change.
- Do not run idf.py set-target esp32p4 on this known-good firmware checkout.
- Do not copy an old build/ directory between computers.
- A clean clone should generate its own build/ directory.
- ESP-IDF v5.5 is the known-good version for this firmware.


17. VERIFIED CLEAN BUILD TEST
=============================

This procedure was successfully tested on 3 September 2026:

Fresh GitLab clone
    ->
UI-FINAL branch
    ->
ESP-IDF v5.5
    ->
clean macOS build
    ->
ESP32-P4 detected through side USB-C
    ->
firmware flashed successfully
    ->
badge booted successfully
    ->
Mini Browser 1.4 works

This confirms that the Git repository can reproduce the working badge
firmware without requiring the original Raspberry Pi build environment.
