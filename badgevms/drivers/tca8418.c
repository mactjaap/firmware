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
#include "freertos/semphr.h"
#include "tty.h"

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

// TCA8418 key event register: bit 7 = pressed, bits 0-6 = key number (1-80 = matrix)
#define TCA8418_KEY_PRESSED 0x80
#define TCA8418_KEY_MASK    0x7F
#define TCA8418_MAX_KEY     80
// Depth of the TCA8418 key event FIFO, bounds the reads per call
#define TCA8418_FIFO_DEPTH  10

#if BADGEVMS_SERIAL_KBD

#define SERIAL_KBD_LINE_MAX 64
#define SERIAL_KBD_QUEUE_MAX 16
// Cap per read so a flood of serial events cannot starve the real keyboard
#define SERIAL_KBD_EVENTS_PER_READ 4
// Bound the time spent draining the UART per poll
#define SERIAL_KBD_POLL_BUDGET 256

/*
 * This poller is the only reader of the UART0 RX FIFO. Lines of the form
 * "E <scancode hex> <down> <text hex>" become keyboard events, every other
 * byte is forwarded unchanged to the tty RX buffer (stdin).
 */
typedef enum {
    SERIAL_KBD_LINE_START, // nothing of the current line seen yet
    SERIAL_KBD_CANDIDATE,  // line starts with 'E', buffering
    SERIAL_KBD_PASSTHRU,   // not a keyboard line, forward until '\n'
} serial_kbd_state_t;

static serial_kbd_state_t serial_kbd_state = SERIAL_KBD_LINE_START;
static char serial_kbd_line[SERIAL_KBD_LINE_MAX];
static size_t serial_kbd_line_len = 0;

static event_t serial_kbd_queue[SERIAL_KBD_QUEUE_MAX];
static unsigned serial_kbd_head = 0;
static unsigned serial_kbd_tail = 0;

// Protects the line parser, the UART FIFO and the event queue. Taken by the
// compositor (keyboard read) and by tty_read (stdin).
static SemaphoreHandle_t serial_kbd_get_lock(void)
{
    static portMUX_TYPE       init_mux = portMUX_INITIALIZER_UNLOCKED;
    static StaticSemaphore_t  lock_buf;
    static SemaphoreHandle_t  lock;

    if (!lock) {
        taskENTER_CRITICAL(&init_mux);
        if (!lock) {
            lock = xSemaphoreCreateMutexStatic(&lock_buf);
        }
        taskEXIT_CRITICAL(&init_mux);
    }

    return lock;
}

static bool serial_kbd_queue_empty(void)
{
    return serial_kbd_head == serial_kbd_tail;
}

static bool serial_kbd_queue_full(void)
{
    return (serial_kbd_head + 1) % SERIAL_KBD_QUEUE_MAX == serial_kbd_tail;
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
    // mod is filled in from the shared modifier state when delivered
    event->keyboard.mod = BADGEVMS_KMOD_NONE;
    event->keyboard.down = down;

    /*
     * SDL_badgevmsevents.c turns this into SDL_TEXT_INPUT
     * on key-down. Only printable ASCII is accepted.
     */
    event->keyboard.text =
        (down && text >= 0x20 && text <= 0x7e) ? text : 0;
}

/*
 * Returns true if the line is a keyboard command (consumed, even if its
 * values are rejected), false if it must be forwarded to the tty.
 */
static bool serial_kbd_process_line(char const *line)
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
        return false;
    }

    if (scancode > 0x1ff) {
        return true;
    }

    if (down > 1) {
        return true;
    }

    if (text > 0xff) {
        return true;
    }

    // The compositor's system hotkeys (FN + arrows/cross, LALT + TAB) must
    // only be reachable from the physical keyboard.
    if (scancode == KEY_SCANCODE_FN || scancode == KEY_SCANCODE_LALT) {
        ESP_LOGW(TAG, "Serial keyboard: rejecting system key 0x%02x", scancode);
        return true;
    }

    event_t event;

    serial_kbd_make_event(
        &event,
        (keyboard_scancode_t)scancode,
        down != 0,
        (unsigned char)text
    );

    // serial_kbd_poll() only parses a line when the queue has room, so this
    // cannot fail; never silently lose a key-up if it ever does.
    if (!serial_kbd_queue_push(&event)) {
        ESP_LOGE(TAG, "Serial keyboard queue full, dropped key %s 0x%02x", down ? "down" : "up", scancode);
    }

    ESP_LOGV(
        TAG,
        "Serial keyboard scancode=0x%02x down=%u text=0x%02x",
        scancode,
        down,
        text
    );

    return true;
}

static void serial_kbd_forward(uint8_t const *buf, size_t len)
{
    if (len && tty_rx_push(buf, len) != len) {
        ESP_LOGV(TAG, "tty RX buffer full, dropped %u bytes", (unsigned)len);
    }
}

static void serial_kbd_poll_uart(void)
{
    uart_dev_t *uart =
        UART_LL_GET_HW(UART_NUM_0);

    uint8_t out[32];
    size_t  out_len = 0;

    for (int budget = SERIAL_KBD_POLL_BUDGET; budget > 0; budget--) {
        // A keyboard line can only be completed when there is room for its
        // event; otherwise leave the bytes in the UART FIFO for later.
        if (serial_kbd_state != SERIAL_KBD_PASSTHRU &&
            serial_kbd_queue_full()) {
            break;
        }

        if (!uart_ll_get_rxfifo_len(uart)) {
            break;
        }

        uint8_t c;
        uart_ll_read_rxfifo(uart, &c, 1);

        if (out_len == sizeof(out)) {
            serial_kbd_forward(out, out_len);
            out_len = 0;
        }

        switch (serial_kbd_state) {
            case SERIAL_KBD_LINE_START:
                if (c == 'E') {
                    serial_kbd_line[0]  = (char)c;
                    serial_kbd_line_len = 1;
                    serial_kbd_state    = SERIAL_KBD_CANDIDATE;
                } else {
                    out[out_len++] = c;
                    if (c != '\n') {
                        serial_kbd_state = SERIAL_KBD_PASSTHRU;
                    }
                }
                break;

            case SERIAL_KBD_CANDIDATE:
                if (c == '\n') {
                    serial_kbd_line[serial_kbd_line_len] = '\0';
                    if (!serial_kbd_process_line(serial_kbd_line)) {
                        // Not a keyboard command: hand it to stdin unchanged
                        serial_kbd_forward(out, out_len);
                        out_len = 0;
                        serial_kbd_forward((uint8_t const *)serial_kbd_line, serial_kbd_line_len);
                        out[out_len++] = c;
                    }
                    serial_kbd_line_len = 0;
                    serial_kbd_state    = SERIAL_KBD_LINE_START;
                } else if ((serial_kbd_line_len == 1 && c != ' ') ||
                           serial_kbd_line_len >= SERIAL_KBD_LINE_MAX - 1) {
                    // Not "E ", or too long to be a keyboard command: discard
                    // it as far as the keyboard is concerned and forward it
                    // up to and including the next '\n'.
                    serial_kbd_forward(out, out_len);
                    out_len = 0;
                    serial_kbd_forward((uint8_t const *)serial_kbd_line, serial_kbd_line_len);
                    serial_kbd_line_len = 0;
                    out[out_len++]      = c;
                    serial_kbd_state    = SERIAL_KBD_PASSTHRU;
                } else {
                    serial_kbd_line[serial_kbd_line_len++] = (char)c;
                }
                break;

            case SERIAL_KBD_PASSTHRU:
                out[out_len++] = c;
                if (c == '\n') {
                    serial_kbd_state = SERIAL_KBD_LINE_START;
                }
                break;
        }
    }

    serial_kbd_forward(out, out_len);
}

void serial_kbd_poll(void)
{
    SemaphoreHandle_t lock = serial_kbd_get_lock();

    xSemaphoreTake(lock, portMAX_DELAY);
    serial_kbd_poll_uart();
    xSemaphoreGive(lock);
}

#endif // BADGEVMS_SERIAL_KBD

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

static key_mod_t scancode_to_mod(keyboard_scancode_t s) {
    switch (s) {
        case KEY_SCANCODE_LSHIFT: return BADGEVMS_KMOD_LSHIFT;
        case KEY_SCANCODE_RSHIFT: return BADGEVMS_KMOD_RSHIFT;
        case KEY_SCANCODE_LCTRL: return BADGEVMS_KMOD_LCTRL;
        case KEY_SCANCODE_RCTRL: return BADGEVMS_KMOD_RCTRL;
        case KEY_SCANCODE_LALT: return BADGEVMS_KMOD_LALT;
        case KEY_SCANCODE_RALT: return BADGEVMS_KMOD_RALT;
        case KEY_SCANCODE_LGUI: return BADGEVMS_KMOD_LGUI;
        default: return BADGEVMS_KMOD_NONE;
    }
}

// Shared by the physical and the serial keyboard so both report the same mods
static void update_mod_state(tca8418_device_t *device, keyboard_scancode_t s, bool down) {
    key_mod_t mod = scancode_to_mod(s);

    if (mod != BADGEVMS_KMOD_NONE) {
        if (down) {
            device->mod_state |= mod;
        } else {
            device->mod_state &= ~mod;
        }
    }
}

static event_t scancode_to_event(tca8418_device_t *device, uint8_t scancode) {
    struct timeval tv_now;
    gettimeofday(&tv_now, NULL);

    uint8_t             key = scancode & TCA8418_KEY_MASK;
    keyboard_scancode_t s   = KEY_SCANCODE_UNKNOWN;

    if (key >= 1 && key <= TCA8418_MAX_KEY) {
        s = keymap[key - 1];
    }

    event_t event        = {0};
    event.keyboard.down  = (scancode & TCA8418_KEY_PRESSED) != 0;

    update_mod_state(device, s, event.keyboard.down);

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

static ssize_t tca8418_read(void *dev, int fd, void *buf, size_t count)
{
    tca8418_device_t *device = dev;

    if (fd) {
        return -1;
    }

    size_t written = 0;

#if BADGEVMS_SERIAL_KBD
    /*
     * First consume any keyboard commands arriving from the Mac.
     * Synthetic serial keyboard events have priority, but are capped so the
     * TCA8418 is still read every frame. Never block the compositor: if
     * tty_read currently holds the lock, try again next frame.
     */
    SemaphoreHandle_t lock = serial_kbd_get_lock();

    if (xSemaphoreTake(lock, 0) == pdTRUE) {
        serial_kbd_poll_uart();

        for (int n = 0; n < SERIAL_KBD_EVENTS_PER_READ && written + sizeof(event_t) <= count; n++) {
            event_t event;

            if (!serial_kbd_queue_pop(&event)) {
                break;
            }

            update_mod_state(device, event.keyboard.scancode, event.keyboard.down);
            event.keyboard.mod = device->mod_state;

            memcpy(
                (uint8_t *)buf + written,
                &event,
                sizeof(event)
            );

            written += sizeof(event);
        }

        xSemaphoreGive(lock);
    }
#endif

    /*
     * Then continue processing the real TCA8418 exactly as before.
     * An I2C error reads as 0 (no events / no key), skipping this frame.
     */
    for (int n = 0; n < TCA8418_FIFO_DEPTH && written + sizeof(event_t) <= count; n++) {
        if (!tca8418_get_event_count(device->keyboard)) {
            break;
        }

        uint8_t c = tca8418_get_key(
            device->keyboard
        );

        uint8_t key = c & TCA8418_KEY_MASK;

        if (key == 0 || key > TCA8418_MAX_KEY) {
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

        ESP_LOGV(
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
    ESP_LOGI(TAG, "Connect to keyboard, this message and the follow error are cosmetic. There is no error");
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

    ESP_LOGI(TAG, "keyboard initialization success");

    return (device_t *)dev;
}
