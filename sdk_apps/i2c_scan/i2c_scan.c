/*
 * i2c_scan - list every device that answers on the badge's I2C bus.
 *
 * Purpose: find out whether the IP5306 power chip is reachable from the
 * ESP32-P4.  Only the I2C variant of the IP5306 has a bus interface; it
 * answers at address 0x75.  If 0x75 does not show up, the battery LEDs
 * are not reachable from software at all.
 *
 * Output goes to the screen and to the serial console (idf.py monitor).
 * Keys: R = scan again, Esc = quit.
 */
#include <SDL3/SDL.h>
#include <badgevms/device.h>
#include <stdio.h>
#include <string.h>

#define BUS_NAME     "I2CBUS0"
#define MAX_RESULTS  127
#define IP5306_ADDR  0x75
#define LINE_MAX     64
#define LINES_MAX    40

static char lines[LINES_MAX][LINE_MAX];
static int  line_count;
static bool ip5306_found;

static void add_line(const char *text) {
    printf("[i2c_scan] %s\n", text);
    if (line_count < LINES_MAX)
        snprintf(lines[line_count++], LINE_MAX, "%s", text);
}

/* Devices the badge is known to have on this bus (see HARDWARE.md). */
static const char *known_device(uint8_t addr) {
    switch (addr) {
        case 0x34: return "TCA8418 keypad controller";
        case 0x69: return "BMI270 motion sensor";
        case 0x68: return "BMI270 motion sensor (alt addr)";
        case 0x76: return "BME690 environment sensor";
        case 0x77: return "BME690 environment sensor (alt addr)";
        case 0x6A: return "BQ25895 charger (older board revision)";
        case 0x75: return "IP5306 power chip (I2C variant) !";
        default:   return "unknown device";
    }
}

static void run_scan(void) {
    char text[LINE_MAX];

    line_count   = 0;
    ip5306_found = false;
    add_line("I2C scan of " BUS_NAME " (SDA GPIO18, SCL GPIO20)");
    add_line("");

    i2c_bus_device_t *bus = (i2c_bus_device_t *)device_get(BUS_NAME);
    if (!bus || bus->device.type != DEVICE_TYPE_BUS || !bus->_scan) {
        add_line("ERROR: I2C bus device " BUS_NAME " not found");
        return;
    }

    i2c_scanresult_t results[MAX_RESULTS];
    memset(results, 0, sizeof(results));
    int count = bus->_scan(bus, results, MAX_RESULTS);
    if (count < 0) count = 0;
    if (count > MAX_RESULTS) count = MAX_RESULTS;

    snprintf(text, sizeof(text), "%d device(s) answered:", count);
    add_line(text);

    for (int i = 0; i < count; i++) {
        uint8_t addr = results[i].address;
        if (addr == IP5306_ADDR) ip5306_found = true;
        snprintf(text, sizeof(text), "  0x%02X  %s", addr, known_device(addr));
        add_line(text);
    }

    add_line("");
    if (ip5306_found) {
        add_line("RESULT: 0x75 answers -> IP5306 I2C variant present.");
        add_line("Its registers can be read; LED control still unlikely.");
    } else {
        add_line("RESULT: nothing at 0x75 -> no IP5306 on this bus.");
        add_line("The battery LEDs cannot be reached from software.");
    }
    add_line("");
    add_line("R = scan again   Esc = quit");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    run_scan();

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        printf("[i2c_scan] SDL_Init failed: %s (results above)\n", SDL_GetError());
        return 0;
    }

    /*
     * Try the Mini Browser's setup first (716x716 fullscreen); a plain
     * 720x720 window does not fit the compositor's client area and the
     * renderer then fails with "Couldn't find matching render driver".
     * Fall back to the window size the SDL Snake demo uses.
     */
    static const struct { int w, h; SDL_WindowFlags flags; float scale; } modes[] = {
        {716, 716, SDL_WINDOW_FULLSCREEN, 1.5f},   /* 60 columns of 12x12 text */
        {576, 432, 0,                     1.2f},   /* 60 columns of ~10x10 text */
    };
    SDL_Window *win = NULL;
    SDL_Renderer *ren = NULL;
    float scale = 1.0f;
    for (size_t m = 0; m < sizeof(modes) / sizeof(modes[0]) && !ren; m++) {
        win = SDL_CreateWindow("I2C Scan", modes[m].w, modes[m].h, modes[m].flags);
        ren = win ? SDL_CreateRenderer(win, NULL) : NULL;
        if (ren) {
            scale = modes[m].scale;
            printf("[i2c_scan] window %dx%d opened\n", modes[m].w, modes[m].h);
        } else {
            printf("[i2c_scan] window %dx%d failed: %s\n", modes[m].w, modes[m].h, SDL_GetError());
            if (win) SDL_DestroyWindow(win);
            win = NULL;
        }
    }
    if (!ren) {
        printf("[i2c_scan] no window/renderer (results above)\n");
        SDL_Quit();
        return 0;
    }
    SDL_SetRenderScale(ren, scale, scale);

    bool running = true, dirty = true;
    while (running) {
        SDL_Event ev;
        if (SDL_WaitEventTimeout(&ev, 500)) {
            do {
                if (ev.type == SDL_EVENT_QUIT) running = false;
                else if (ev.type == SDL_EVENT_KEY_DOWN) {
                    if (ev.key.key == SDLK_ESCAPE) running = false;
                    else if (ev.key.key == SDLK_R) { run_scan(); dirty = true; }
                }
            } while (SDL_PollEvent(&ev));
        }
        if (!dirty || !running) continue;
        dirty = false;

        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        for (int i = 0; i < line_count; i++) {
            bool result_line = strncmp(lines[i], "RESULT", 6) == 0;
            if (result_line && ip5306_found)
                SDL_SetRenderDrawColor(ren, 255, 200, 0, 255);
            else if (result_line)
                SDL_SetRenderDrawColor(ren, 0, 220, 0, 255);
            else
                SDL_SetRenderDrawColor(ren, 220, 220, 220, 255);
            SDL_RenderDebugText(ren, 8.0f, 8.0f + 12.0f * i, lines[i]);
        }
        SDL_RenderPresent(ren);
    }

    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
