
#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <badgevms/misc_funcs.h>
#include <badgevms/wifi.h>

#include "../why2025_ota/font.h"

#define SCREEN_WIDTH  720
#define SCREEN_HEIGHT 720

static inline Uint16 rgb888_to_rgb565(Uint32 rgb888) {
    Uint8 r = (rgb888 >> 16) & 0xFF;
    Uint8 g = (rgb888 >> 8) & 0xFF;
    Uint8 b = rgb888 & 0xFF;
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

static void draw_rect(Uint16 *pixels, int x, int y, int w, int h, Uint32 color) {
    Uint16 rgb565 = rgb888_to_rgb565(color);
    int    x2     = x + w;
    int    y2     = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x2 > SCREEN_WIDTH) x2 = SCREEN_WIDTH;
    if (y2 > SCREEN_HEIGHT) y2 = SCREEN_HEIGHT;
    for (int py = y; py < y2; py++) {
        Uint16 *row = &pixels[py * SCREEN_WIDTH + x];
        memset(row, rgb565 & 0xFF, 0);
        for (int i = 0; i < (x2 - x); i++) row[i] = rgb565;
    }
}

static void draw_char(Uint16 *pixels, int x, int y, char c, Uint32 color) {
    if (c < FONT_FIRST_CHAR || c > FONT_LAST_CHAR) return;
    int             char_index = c - FONT_FIRST_CHAR;
    uint16_t const *char_data  = pixel_font[char_index];
    Uint16          rgb565     = rgb888_to_rgb565(color);
    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint16_t row_data = char_data[row];
        int      py       = y + row;
        if (py < 0 || py >= SCREEN_HEIGHT) continue;
        for (int col = 0; col < FONT_WIDTH; col++) {
            if (row_data & (0x800 >> col)) {
                int px = x + col;
                if (px >= 0 && px < SCREEN_WIDTH) pixels[py * SCREEN_WIDTH + px] = rgb565;
            }
        }
    }
}

static void draw_text(Uint16 *pixels, int x, int y, const char *text, Uint32 color) {
    int cx = x;
    for (; *text; ++text) {
        draw_char(pixels, cx, y, *text, color);
        cx += FONT_WIDTH;
    }
}

int main(void) {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        printf("SDL init failed: %s\n", SDL_GetError());
        return 0;
    }

    SDL_Window   *win = SDL_CreateWindow("Device Status", SCREEN_WIDTH, SCREEN_HEIGHT, SDL_WINDOW_FULLSCREEN);
    SDL_Renderer *ren = SDL_CreateRenderer(win, NULL);
    SDL_Texture  *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, SCREEN_WIDTH, SCREEN_HEIGHT);
    Uint16       *fb  = malloc(SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(Uint16));
    if (!win || !ren || !tex || !fb) {
        printf("Failed to init UI\n");
        goto quit;
    }

    int quit = 0;
    while (!quit) {
        memset(fb, 0, SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(Uint16));
        draw_rect(fb, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0xAEB2B2);

        int y = 20;
        draw_text(fb, 20, y, "Device Status", 0x000000); y += 30;

        // MAC and Unique ID
        const char *mac = get_mac_address();
        char        line[256];
        if (mac && mac[0]) {
            snprintf(line, sizeof(line), "MAC: %s", mac);
            draw_text(fb, 20, y, line, 0x000000); y += 24;
        }
        uint64_t uid = get_unique_id();
        if (uid) {
            snprintf(line, sizeof(line), "ID: %08lX%08lX", (uint32_t)(uid >> 32), (uint32_t)uid);
            draw_text(fb, 20, y, line, 0x000000); y += 24;
        }

        // Memory & framebuffer/PSRAM
        size_t free_heap = get_free_heap_bytes();
        snprintf(line, sizeof(line), "Heap free: %u", (unsigned)free_heap);
        draw_text(fb, 20, y, line, 0x000000); y += 24;
        snprintf(line, sizeof(line), "PSRAM pages: %u/%u", (unsigned)get_psram_pages_free(), (unsigned)get_psram_pages_total());
        draw_text(fb, 20, y, line, 0x000000); y += 24;
        snprintf(line, sizeof(line), "FB pages: %u/%u", (unsigned)get_framebuffer_pages_free(), (unsigned)get_framebuffer_pages_total());
        draw_text(fb, 20, y, line, 0x000000); y += 24;

        // Wi‑Fi
        wifi_connection_status_t wcs = wifi_get_connection_status();
        snprintf(line, sizeof(line), "WiFi: conn=%d", (int)wcs);
        draw_text(fb, 20, y, line, 0x000000); y += 24;

        int ap_count = wifi_scan_get_num_results();
        if (ap_count > 0) {
            snprintf(line, sizeof(line), "APs: %d", ap_count);
            draw_text(fb, 20, y, line, 0x000000); y += 24;
            for (int i = 0; i < ap_count; i++) {
                wifi_station_handle s = wifi_scan_get_result(i);
                if (!s) continue;
                snprintf(line, sizeof(line), "[%d] %s rssi=%d ch=%d",
                         i, wifi_station_get_ssid(s), wifi_station_get_rssi(s), wifi_station_get_primary_channel(s));
                draw_text(fb, 30, y, line, 0x000000); y += 20;
                wifi_scan_free_station(s);
            }
        }

        // TODO: If SD card mount info becomes available via an exported API, add here.
        // TODO: If Bluetooth devices API is available, list summary here.

        SDL_UpdateTexture(tex, NULL, fb, SCREEN_WIDTH * sizeof(Uint16));
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_RenderTexture(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) quit = 1;
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_ESCAPE) quit = 1;
        }
        SDL_Delay(250);
    }

quit:
    if (fb) free(fb);
    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}


