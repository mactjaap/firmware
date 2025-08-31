cd ~/firmware
rm -rf build
. ~/esp-idf/export.sh
idf.py build


idf.py -p /dev/ttyUSB0 erase_flash
idf.py -p /dev/ttyUSB0 flash
idf.py -p /dev/ttyUSB0 monitor
