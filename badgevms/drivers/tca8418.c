/* This file is part of BadgeVMS
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "tca8418.h"

#include "badgevms/event.h"
#include "esp_log.h"
#include "esp_tca8418.h"
#include "freertos/FreeRTOS.h"

#include "hal/uart_ll.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#include <sys/time.h>

#define SDA_PIN           18
#define SCL_PIN           20
#define I2C_MASTER_SDA_IO SDA_PIN
#define I2C_MASTER_SCL_IO SCL_PIN

#define TAG "TCA8418"

#define SERIAL_KBD_LINE_MAX 64
#define SERIAL_KBD_QUEUE_MAX 16

static char serial_kbd_line[SERIAL_KBD_LINE_MAX];
static size_t serial_kbd_line_len = 0;

static event_t serial_kbd_queue[SERIAL_KBD_QUEUE_MAX];
static unsigned serial_kbd_head = 0;
static unsigned serial_kbd_tail = 0;

static bool serial_kbd_queue_empty(void)
{
    return serial_kbd_head == serial_kbd_tail;
}

static bool serial_kbd_queue_push(event_t const *event)
{
    unsigned next = (serial_kbd_head + 1) % SERIAL_KBD_QUEUE_MAX;

    if (next == serial_kbd_tail) {
        return false;
    }

    serial_kbd_queue[serial_kbd_head] = *event;
    serial_kbd_head = next;

    return true;
}

static bool serial_kbd_queue_pop(event_t *event)
{
    if (serial_kbd_queue_empty()) {
        return false;
    }

    *event = serial_kbd_queue[serial_kbd_tail];
    serial_kbd_tail =
        (serial_kbd_tail + 1) % SERIAL_KBD_QUEUE_MAX;

    return true;
}

static void serial_kbd_make_event(
    event_t *event,
    keyboard_scancode_t scancode,
    bool down,
    key_mod_t mod,
    unsigned char text
)
{
    struct timeval tv_now;

    memset(event, 0, sizeof(*event));

    gettimeofday(&tv_now, NULL);

    event->type = down
        ? EVENT_KEY_DOWN
        : EVENT_KEY_UP;

    event->keyboard.timestamp =
        (int64_t)tv_now.tv_sec * 1000000L +
        (int64_t)tv_now.tv_usec;

    event->keyboard.scancode = scancode;
    event->keyboard.key =
        BADGEVMS_SCANCODE_TO_KEYCODE(scancode);

    event->keyboard.repeat = false;
    event->keyboard.mod = mod;
    event->keyboard.down = down;

    /*
     * SDL_badgevmsevents.c turns this into SDL_TEXT_INPUT
     * on key-down.
     */
    event->keyboard.text =
        down ? text : 0;
}

static void serial_kbd_process_line(char const *line)
{
    unsigned scancode;
    unsigned down;
    unsigned text;

    /*
     * Protocol:
     *
     * E <scancode hex> <down 0/1> <text hex>
     *
     * Examples:
     *
     * E 04 1 61    A key down, text 'a'
     * E 04 0 00    A key up
     * E 28 1 00    Return down
     * E 28 0 00    Return up
     */

    if (sscanf(
            line,
            "E %x %u %x",
            &scancode,
            &down,
            &text
        ) != 3) {
        return;
    }

    if (scancode > 0x1ff) {
        return;
    }

    if (down > 1) {
        return;
    }

    if (text > 0xff) {
        return;
    }

    event_t event;

    serial_kbd_make_event(
        &event,
        (keyboard_scancode_t)scancode,
        down != 0,
        BADGEVMS_KMOD_NONE,
        (unsigned char)text
    );

    serial_kbd_queue_push(&event);

    ESP_LOGI(
        TAG,
        "Serial keyboard scancode=0x%02x down=%u text=0x%02x",
        scancode,
        down,
        text
    );
}

static void serial_kbd_poll_uart(void)
{
    uart_dev_t *uart =
        UART_LL_GET_HW(UART_NUM_0);

    uint32_t available =
        uart_ll_get_rxfifo_len(uart);

    while (available > 0) {
        uint8_t buffer[64];

        uint32_t count = available;

        if (count > sizeof(buffer)) {
            count = sizeof(buffer);
        }

        uart_ll_read_rxfifo(
            uart,
            buffer,
            count
        );

        for (uint32_t i = 0; i < count; i++) {
            unsigned char c = buffer[i];

            if (c == '\r') {
                continue;
            }

            if (c == '\n') {
                serial_kbd_line[
                    serial_kbd_line_len
                ] = '\0';

                serial_kbd_process_line(
                    serial_kbd_line
                );

                serial_kbd_line_len = 0;
                continue;
            }

            if (serial_kbd_line_len <
                SERIAL_KBD_LINE_MAX - 1) {

                serial_kbd_line[
                    serial_kbd_line_len++
                ] = (char)c;

            } else {
                serial_kbd_line_len = 0;
            }
        }

        available =
            uart_ll_get_rxfifo_len(uart);
    }
}



typedef struct {
    device_t       device;
    key_mod_t      mod_state;
    tca8418_dev_t *keyboard;
} tca8418_device_t;

static keyboard_scancode_t const keymap[80] = {
    KEY_SCANCODE_ESCAPE,    // 0x1
    KEY_SCANCODE_SQUARE,    // 0x2
    KEY_SCANCODE_TRIANGLE,  // 0x3
    KEY_SCANCODE_CROSS,     // 0x4
    KEY_SCANCODE_CIRCLE,    // 0x5
    KEY_SCANCODE_CLOUD,     // 0x6
    KEY_SCANCODE_DIAMOND,   // 0x7
    KEY_SCANCODE_BACKSPACE, // 0x8
    KEY_SCANCODE_0,         // 0x9
    KEY_SCANCODE_MINUS,     // 0xa
    KEY_SCANCODE_GRAVE,     // 0xb
    KEY_SCANCODE_1,         // 0xc
    KEY_SCANCODE_2,         // 0xd
    KEY_SCANCODE_3,         // 0xe
    KEY_SCANCODE_4,         // 0xf

    KEY_SCANCODE_5,   // 0x10
    KEY_SCANCODE_6,   // 0x11
    KEY_SCANCODE_7,   // 0x12
    KEY_SCANCODE_8,   // 0x13
    KEY_SCANCODE_9,   // 0x14
    KEY_SCANCODE_TAB, // 0x15
    KEY_SCANCODE_Q,   // 0x16
    KEY_SCANCODE_W,   // 0x17
    KEY_SCANCODE_E,   // 0x18
    KEY_SCANCODE_R,   // 0x19
    KEY_SCANCODE_T,   // 0x1a
    KEY_SCANCODE_Y,   // 0x1b
    KEY_SCANCODE_U,   // 0x1c
    KEY_SCANCODE_I,   // 0x1d
    KEY_SCANCODE_O,   // 0x1e
    KEY_SCANCODE_FN,  // 0x1f

    KEY_SCANCODE_A,      // 0x20
    KEY_SCANCODE_S,      // 0x21
    KEY_SCANCODE_D,      // 0x22
    KEY_SCANCODE_F,      // 0x23
    KEY_SCANCODE_G,      // 0x24
    KEY_SCANCODE_H,      // 0x25
    KEY_SCANCODE_J,      // 0x26
    KEY_SCANCODE_K,      // 0x27
    KEY_SCANCODE_L,      // 0x28
    KEY_SCANCODE_LSHIFT, // 0x29
    KEY_SCANCODE_Z,      // 0x2a
    KEY_SCANCODE_X,      // 0x2b
    KEY_SCANCODE_C,      // 0x2c
    KEY_SCANCODE_V,      // 0x2d
    KEY_SCANCODE_B,      // 0x2e
    KEY_SCANCODE_N,      // 0x2f

    KEY_SCANCODE_M,          // 0x30
    KEY_SCANCODE_COMMA,      // 0x31
    KEY_SCANCODE_PERIOD,     // 0x32
    KEY_SCANCODE_LEFT,       // 0x33
    KEY_SCANCODE_DOWN,       // 0x34
    KEY_SCANCODE_RIGHT,      // 0x35
    KEY_SCANCODE_SLASH,      // 0x36
    KEY_SCANCODE_UP,         // 0x37
    KEY_SCANCODE_RSHIFT,     // 0x38
    KEY_SCANCODE_SEMICOLON,  // 0x39
    KEY_SCANCODE_APOSTROPHE, // 0x3a
    KEY_SCANCODE_RETURN,     // 0x3b
    KEY_SCANCODE_EQUALS,     // 0x3c
    KEY_SCANCODE_LCTRL,      // 0x3d
    KEY_SCANCODE_LGUI,       // 0x3e
    KEY_SCANCODE_LALT,       // 0x3f

    KEY_SCANCODE_BACKSLASH,   // 0x40
    KEY_SCANCODE_SPACE,       // 0x41
    KEY_SCANCODE_SPACE,       // 0x42
    KEY_SCANCODE_SPACE,       // 0x43
    KEY_SCANCODE_RALT,        // 0x44
    KEY_SCANCODE_P,           // 0x45
    KEY_SCANCODE_LEFTBRACKET, // 0x46
    KEY_SCANCODE_UNKNOWN,     // 0x47
    KEY_SCANCODE_UNKNOWN,     // 0x48
    KEY_SCANCODE_UNKNOWN,     // 0x49
    KEY_SCANCODE_UNKNOWN,     // 0x4a
    KEY_SCANCODE_UNKNOWN,     // 0x4b
    KEY_SCANCODE_UNKNOWN,     // 0x4c
    KEY_SCANCODE_UNKNOWN,     // 0x4d
    KEY_SCANCODE_UNKNOWN,     // 0x4e
    KEY_SCANCODE_UNKNOWN,     // 0x4f

    KEY_SCANCODE_RIGHTBRACKET, // 0x50
};

event_t scancode_to_event(tca8418_device_t *device, uint8_t scancode) {
    struct timeval tv_now;
    gettimeofday(&tv_now, NULL);

    uint8_t             key = scancode & 0x7F;
    keyboard_scancode_t s   = KEY_SCANCODE_UNKNOWN;

    if (key <= 0x50) {
        s = keymap[key - 1];
    }

    event_t event;
    event.keyboard.down = scancode >> 7; // high bit set means pressed

    key_mod_t mod;
    switch (s) {
        case KEY_SCANCODE_LSHIFT: mod = BADGEVMS_KMOD_LSHIFT; break;
        case KEY_SCANCODE_RSHIFT: mod = BADGEVMS_KMOD_RSHIFT; break;
        case KEY_SCANCODE_LCTRL: mod = BADGEVMS_KMOD_LCTRL; break;
        case KEY_SCANCODE_RCTRL: mod = BADGEVMS_KMOD_RCTRL; break;
        case KEY_SCANCODE_LALT: mod = BADGEVMS_KMOD_LALT; break;
        case KEY_SCANCODE_RALT: mod = BADGEVMS_KMOD_RALT; break;
        case KEY_SCANCODE_LGUI: mod = BADGEVMS_KMOD_LGUI; break;
        default: mod = BADGEVMS_KMOD_NONE;
    }

    if (mod != BADGEVMS_KMOD_NONE) {
        if (event.keyboard.down) {
            device->mod_state |= mod;
        } else {
            device->mod_state &= ~mod;
        }
    }

    if (event.keyboard.down) {
        event.type = EVENT_KEY_DOWN;
    } else {
        event.type = EVENT_KEY_UP;
    }

    event.keyboard.timestamp = (int64_t)tv_now.tv_sec * 1000000L + (int64_t)tv_now.tv_usec;
    event.keyboard.scancode  = s;
    event.keyboard.key       = BADGEVMS_SCANCODE_TO_KEYCODE(s);
    event.keyboard.repeat    = false;
    event.keyboard.mod       = device->mod_state;
    event.keyboard.text      = keyboard_get_ascii(s, device->mod_state);

    return event;
}

static int tca8418_open(void *dev, path_t *path, int flags, mode_t mode) {
    return 0;
}

static int tca8418_close(void *dev, int fd) {
    if (fd)
        return -1;
    return 0;
}

static ssize_t tca8418_write(void *dev, int fd, void const *buf, size_t count) {
    return -1;
}

static void tca8418_keyboard_task(void *pvParameters) {
    tca8418_device_t *device = pvParameters;
    // poll keyboard for keypresses
    while (true) {
        while (tca8418_get_event_count(device->keyboard) > 0) {
            sKeyAndChar test = tca8418_get_char(device->keyboard);
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
    // If ever exits, cleanup
    vTaskDelete(NULL);
}

static ssize_t tca8418_read(void *dev, int fd, void *buf, size_t count)
{
    tca8418_device_t *device = dev;

    if (fd) {
        return -1;
    }

    /*
     * First consume any keyboard commands arriving from the Mac.
     */
    serial_kbd_poll_uart();

    size_t written = 0;

    /*
     * Synthetic serial keyboard events have priority.
     */
    while (written + sizeof(event_t) <= count) {
        event_t event;

        if (!serial_kbd_queue_pop(&event)) {
            break;
        }

        memcpy(
            (uint8_t *)buf + written,
            &event,
            sizeof(event)
        );

        written += sizeof(event);
    }

    /*
     * Then continue processing the real TCA8418 exactly as before.
     */
    while (written + sizeof(event_t) <= count) {
        if (!tca8418_get_event_count(device->keyboard)) {
            break;
        }

        char c = tca8418_get_key(
            device->keyboard
        );

        uint8_t key = c & 0x7F;

        if (key > 0x50) {
            ESP_LOGD(
                TAG,
                "Illegal scancode 0x%02x, skipping",
                c
            );

            continue;
        }

        event_t event =
            scancode_to_event(
                device,
                c
            );

        ESP_LOGW(
            TAG,
            "Got keyboard event raw 0x%02x scancode 0x%02x",
            c,
            event.keyboard.scancode
        );

        memcpy(
            (uint8_t *)buf + written,
            &event,
            sizeof(event)
        );

        written += sizeof(event);
    }

    return written;
}

static ssize_t tca8418_lseek(void *dev, int fd, off_t offset, int whence) {
    return -1;
}

device_t *tca8418_keyboard_create() {
    tca8418_device_t *dev      = calloc(1, sizeof(tca8418_device_t));
    device_t         *base_dev = (device_t *)dev;

    base_dev->type   = DEVICE_TYPE_KEYBOARD;
    base_dev->_open  = tca8418_open;
    base_dev->_close = tca8418_close;
    base_dev->_write = tca8418_write;
    base_dev->_read  = tca8418_read;
    base_dev->_lseek = tca8418_lseek;

    // https://github.com/espressif/esp-iot-solution/discussions/494
    ESP_LOGE(TAG, "Connect to keyboard, this message and the follow error are cosmetic. There is no error");
    dev->keyboard = tca8418_create(
        I2C_MASTER_SCL_IO,
        I2C_MASTER_SDA_IO,
        0,           // Default address
        GPIO_NUM_NC, // Notify pin is not connected
        0,           // Max
        0            // Max
    );

    if (!dev->keyboard) {
        ESP_LOGE(TAG, "keyboard initialization failed");
        free(dev);
        return NULL;
    }

    tca8418_flush(dev->keyboard);

    ESP_LOGE(TAG, "keyboard initialization success");
    // xTaskCreate(tca8418_keyboard_task, "tca8418_keyboard_task", 4096, dev, tskIDLE_PRIORITY, NULL);

    return (device_t *)dev;
}
