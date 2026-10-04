#include "font.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <badgevms/wifi.h>
#include <SDL3/SDL.h>
#include <string.h>
#include <time.h>

#define SCREEN_WIDTH  720
#define SCREEN_HEIGHT 720

/* ===========================================================
   Color theme (RGB888) - WHY2025 palette, same as the launcher
   =========================================================== */
#define COL_BG                0x0A0E14  /* deep slate */
#define COL_PANEL             0x111826  /* card back */
#define COL_ACCENT_3          0xF25E95  /* pink/rose */
#define COL_ACCENT_6          0xF24436  /* red */
#define COL_PURPLE            0x8B5CFF
#define COL_YELLOW            0xFFFB96
#define COL_TITLE_BG          0x0F1220
#define COL_TITLE_TEXT        0xFFFFFF
#define COL_TEXT              0xE5E7EB
#define COL_TEXT_MID          0x9CA3AF
#define COL_DIVIDER           0x2A3446
#define COL_BTN               0x1C2433
#define COL_BTN_TEXT          0xE5E7EB
#define COL_FOCUS_BG          0x1A2232
#define COL_FOCUS_OUT         0x64EFFE  /* cyan outline */
#define COL_ROW_SELECTED_BG   0x18283C
#define COL_ROW_SELECTED_TEXT 0xFFFFFF
#define COL_SUCCESS           0x34D399  /* green */
#define COL_ERROR             COL_ACCENT_6

/* Window geometry shared by all screens (matches the launcher). */
#define WIN_X     24
#define WIN_Y     24
#define WIN_W     (SCREEN_WIDTH - 48)
#define WIN_H     (SCREEN_HEIGHT - 48)
#define TITLE_H   48
#define FOOTER_H  38

typedef enum { SCREEN_MAIN, SCREEN_WIFI, SCREEN_DISPLAY, SCREEN_SYSTEM, SCREEN_ABOUT } ScreenState;

typedef struct {
    char ssid[64];
    int  signal_strength; // 0-100, strongest access point
    int  ap_count;        // access points broadcasting this name
    bool secured;
    bool connected;
} wifi_network;

typedef struct {
    SDL_Window   *window;
    SDL_Renderer *renderer;
    SDL_Texture  *texture;
    uint16_t     *pixels;

    ScreenState current_screen;
    int         selected_item;
    int         scroll_offset;
    int         total_items;
    int         items_per_page;

    wifi_network *networks;
    int           network_count;
    bool          show_password_dialog;
    bool          show_connecting_dialog;
    int           selected_network;
    char          password_buffer[128];
    int           password_cursor;
    bool          show_password;

    char     status_message[256];
    uint32_t status_color;
    uint32_t status_timer;

    char connecting_ssid[64];
    char connection_status_text[128];
    bool connecting_secured;

    /* Current connection, refreshed every CONN_REFRESH_MS. */
    bool     is_connected;
    char     connected_ssid[64];
    int      connected_rssi;
    uint32_t conn_refresh_at;
} app_context;

#define CONN_REFRESH_MS 2000

static void render_screen(app_context *ctx);

static inline uint16_t rgb888_to_rgb565_color(uint32_t rgb888) {
    uint8_t r = (rgb888 >> 16) & 0xFF;
    uint8_t g = (rgb888 >> 8) & 0xFF;
    uint8_t b = rgb888 & 0xFF;
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

static void put_px(app_context *ctx, int x, int y, uint32_t color) {
    if ((unsigned)x >= SCREEN_WIDTH || (unsigned)y >= SCREEN_HEIGHT)
        return;
    ctx->pixels[y * SCREEN_WIDTH + x] = rgb888_to_rgb565_color(color);
}

static void draw_rect(app_context *ctx, int x, int y, int w, int h, uint32_t color) {
    uint16_t rgb565 = rgb888_to_rgb565_color(color);
    int      x2     = x + w;
    int      y2     = y + h;

    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x2 > SCREEN_WIDTH)
        x2 = SCREEN_WIDTH;
    if (y2 > SCREEN_HEIGHT)
        y2 = SCREEN_HEIGHT;

    for (int py = y; py < y2; py++) {
        uint16_t *row   = &ctx->pixels[py * SCREEN_WIDTH + x];
        int       width = x2 - x;
        for (int i = 0; i < width; i++) {
            row[i] = rgb565;
        }
    }
}

static void draw_char(app_context *ctx, int x, int y, char c, uint32_t color) {
    if (c < FONT_FIRST_CHAR || c > FONT_LAST_CHAR)
        return;

    int             char_index = c - FONT_FIRST_CHAR;
    uint16_t const *char_data  = pixel_font[char_index];
    uint16_t        rgb565     = rgb888_to_rgb565_color(color);

    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint16_t row_data = char_data[row];
        int      py       = y + row;

        if (py < 0 || py >= SCREEN_HEIGHT)
            continue;

        for (int col = 0; col < FONT_WIDTH; col++) {
            if (row_data & (0x800 >> col)) {
                int px = x + col;
                if (px >= 0 && px < SCREEN_WIDTH) {
                    ctx->pixels[py * SCREEN_WIDTH + px] = rgb565;
                }
            }
        }
    }
}

static void draw_text(app_context *ctx, int x, int y, char const *text, uint32_t color) {
    int current_x = x;
    while (*text) {
        draw_char(ctx, current_x, y, *text, color);
        current_x += FONT_WIDTH;
        text++;
    }
}

static void draw_text_bold(app_context *ctx, int x, int y, char const *text, uint32_t color) {
    draw_text(ctx, x, y, text, color);
    draw_text(ctx, x + 1, y, text, color);
}

static int get_text_width(char const *text) {
    return strlen(text) * FONT_WIDTH;
}

static void draw_text_centered(app_context *ctx, int x, int y, int width, char const *text, uint32_t color) {
    int text_w = get_text_width(text);
    int text_x = x + (width - text_w) / 2;
    draw_text(ctx, text_x, y, text, color);
}

/* Draw at most max_w pixels of text; longer text ends in "..". */
static void draw_text_clipped(app_context *ctx, int x, int y, int max_w, char const *text, uint32_t color, bool bold) {
    char buf[96];
    int  max_chars = max_w / FONT_WIDTH;
    if (max_chars < 0)
        max_chars = 0;
    if (max_chars > (int)sizeof(buf) - 1)
        max_chars = (int)sizeof(buf) - 1;
    snprintf(buf, sizeof(buf), "%s", text);
    if ((int)strlen(buf) > max_chars) {
        buf[max_chars] = '\0';
        if (max_chars >= 2) {
            buf[max_chars - 1] = '.';
            buf[max_chars - 2] = '.';
        }
    }
    if (bold)
        draw_text_bold(ctx, x, y, buf, color);
    else
        draw_text(ctx, x, y, buf, color);
}

/* ===========================================================
   Cards, title bars and focus - same style as the launcher
   =========================================================== */
static void draw_outline(app_context *ctx, int x, int y, int w, int h, uint32_t color) {
    draw_rect(ctx, x, y, w, 1, color);
    draw_rect(ctx, x, y + h - 1, w, 1, color);
    draw_rect(ctx, x, y, 1, h, color);
    draw_rect(ctx, x + w - 1, y, 1, h, color);
}

static void draw_card(app_context *ctx, int x, int y, int w, int h) {
    draw_rect(ctx, x, y, w, h, COL_PANEL);
    draw_outline(ctx, x, y, w, h, COL_DIVIDER);
}

static void draw_focus(app_context *ctx, int x, int y, int w, int h) {
    draw_outline(ctx, x - 2, y - 2, w + 4, h + 4, COL_FOCUS_OUT);
}

static void draw_title_bar(app_context *ctx, int x, int y, int w, int h, char const *title) {
    draw_rect(ctx, x, y, w, h, COL_TITLE_BG);
    draw_text_bold(ctx, x + 16, y + (h - FONT_HEIGHT) / 2, title, COL_TITLE_TEXT);
}

/* A dialog: dark margin around a card with its own title bar. */
static void draw_dialog(app_context *ctx, int x, int y, int w, int h, char const *title) {
    draw_rect(ctx, x - 6, y - 6, w + 12, h + 12, COL_BG);
    draw_card(ctx, x, y, w, h);
    draw_title_bar(ctx, x + 2, y + 2, w - 4, 42, title);
    draw_rect(ctx, x + 2, y + 44, w - 4, 2, COL_ACCENT_3);
}

/* Full-screen frame: background, card, title, hint line and footer.
   Returns nothing; the content area starts at WIN_Y + TITLE_H + 36. */
static void draw_frame(app_context *ctx, char const *title, char const *hint) {
    draw_rect(ctx, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COL_BG);
    draw_card(ctx, WIN_X, WIN_Y, WIN_W, WIN_H);
    draw_title_bar(ctx, WIN_X + 2, WIN_Y + 2, WIN_W - 4, TITLE_H, title);
    draw_text(ctx, WIN_X + 16, WIN_Y + TITLE_H + 12, hint, COL_TEXT_MID);
    draw_rect(ctx, WIN_X + 2, WIN_Y + WIN_H - FOOTER_H - 2, WIN_W - 4, FOOTER_H, COL_BTN);
}

/* Footer: "WHY2025 - BadgeVMS" left, an optional message right. */
static void draw_footer(app_context *ctx, char const *message, uint32_t color) {
    static char const footer[] = "WHY2025 - BadgeVMS";
    int               fy       = WIN_Y + WIN_H - FOOTER_H - 2 + (FOOTER_H - FONT_HEIGHT) / 2;
    draw_text(ctx, WIN_X + 16, fy, footer, COL_BTN_TEXT);
    if (message && message[0]) {
        int max_w = WIN_W - 48 - get_text_width(footer);
        int w     = get_text_width(message);
        if (w > max_w)
            w = max_w;
        draw_text_clipped(ctx, WIN_X + WIN_W - 16 - w, fy, max_w, message, color, false);
    }
}

/* List panel with a divider outline. */
static void draw_list_panel(app_context *ctx, int x, int y, int w, int h) {
    draw_rect(ctx, x, y, w, h, COL_PANEL);
    draw_outline(ctx, x, y, w, h, COL_DIVIDER);
}

/* ===========================================================
   Vector icons
   =========================================================== */
static void draw_circle_filled(app_context *ctx, int cx, int cy, int r, uint32_t color) {
    for (int dy = -r; dy <= r; dy++) {
        int dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= r * r)
            dx++;
        draw_rect(ctx, cx - dx, cy + dy, 2 * dx + 1, 1, color);
    }
}

/* Upper arc of a ring (WiFi waves), thickness t, between ~45 and ~135 deg. */
static void draw_wave(app_context *ctx, int cx, int cy, int r, int t, uint32_t color) {
    for (int dy = -r; dy <= 0; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 > r * r || d2 < (r - t) * (r - t))
                continue;
            if (-dy * 10 < (dx < 0 ? -dx : dx) * 10)   /* keep |dx| <= -dy */
                continue;
            put_px(ctx, cx + dx, cy + dy, color);
        }
    }
}

static void icon_wifi(app_context *ctx, int x, int y, int sz, bool sel) {
    int cx = x + sz / 2, cy = y + sz * 78 / 100;
    draw_wave(ctx, cx, cy, sz * 44 / 100, 4, sel ? COL_FOCUS_OUT : COL_PURPLE);
    draw_wave(ctx, cx, cy, sz * 31 / 100, 4, COL_FOCUS_OUT);
    draw_wave(ctx, cx, cy, sz * 18 / 100, 4, sel ? COL_YELLOW : COL_ACCENT_3);
    draw_circle_filled(ctx, cx, cy - 1, 3, sel ? COL_YELLOW : COL_ACCENT_3);
}

static void icon_about(app_context *ctx, int x, int y, int sz, bool sel) {
    int cx = x + sz / 2, cy = y + sz / 2, r = sz * 40 / 100;
    draw_circle_filled(ctx, cx, cy, r, sel ? COL_FOCUS_OUT : COL_PURPLE);
    draw_circle_filled(ctx, cx, cy, r - 3, COL_BG);
    draw_circle_filled(ctx, cx, cy - r / 2, 3, sel ? COL_YELLOW : COL_ACCENT_3);
    draw_rect(ctx, cx - 2, cy - r / 4, 5, r * 3 / 4 + 2, sel ? COL_YELLOW : COL_FOCUS_OUT);
}

static void draw_signal_strength(app_context *ctx, int x, int y, int strength) {
    int bar_width   = 5;
    int bar_spacing = 3;
    int max_height  = 22;

    for (int i = 0; i < 4; i++) {
        int bar_x      = x + i * (bar_width + bar_spacing);
        int bar_height = (max_height * (i + 1)) / 4;
        int bar_y      = y + max_height - bar_height;

        uint32_t color = COL_DIVIDER;
        if (strength >= (i + 1) * 25) {
            color = (strength > 75) ? COL_FOCUS_OUT : (strength > 50) ? COL_TEXT : COL_ACCENT_3;
        }

        draw_rect(ctx, bar_x, bar_y, bar_width, bar_height, color);
    }
}

/* RSSI (dBm) -> 0..100: -100 dBm or worse is 0, -50 dBm or better is 100. */
static int signal_quality(int rssi) {
    int q = 2 * (rssi + 100);
    return q < 0 ? 0 : q > 100 ? 100 : q;
}

static void update_connection_info(app_context *ctx, bool force) {
    uint32_t now = SDL_GetTicks();
    if (!force && now < ctx->conn_refresh_at)
        return;
    ctx->conn_refresh_at = now + CONN_REFRESH_MS;

    wifi_station_handle st = wifi_get_connection_station(); /* NULL unless connected */
    ctx->is_connected      = st != NULL;
    ctx->connected_ssid[0] = '\0';
    ctx->connected_rssi    = 0;
    if (st) {
        snprintf(ctx->connected_ssid, sizeof(ctx->connected_ssid), "%s", wifi_station_get_ssid(st));
        ctx->connected_rssi = wifi_station_get_rssi(st);
        wifi_scan_free_station(st);
    }
}

/* Connected network first, then strongest signal, then name. */
static int network_compare(void const *pa, void const *pb) {
    wifi_network const *a = pa, *b = pb;
    if (a->connected != b->connected)
        return a->connected ? -1 : 1;
    if (a->signal_strength != b->signal_strength)
        return b->signal_strength - a->signal_strength;
    return strcmp(a->ssid, b->ssid);
}

/*
 * One entry per network name.  Mesh systems and dual-band routers show up
 * as several access points with the same SSID; they are merged into one
 * row that keeps the strongest signal.  Hidden networks (empty SSID) are
 * left out because they cannot be selected by name.
 */
static void populate_wifi_networks(app_context *ctx) {
    int results = wifi_scan_get_num_results();
    update_connection_info(ctx, true);

    free(ctx->networks);
    ctx->networks      = calloc(results > 0 ? results : 1, sizeof(wifi_network));
    ctx->network_count = 0;
    if (!ctx->networks)
        return;

    for (int i = 0; i < results; i++) {
        wifi_station_handle station = wifi_scan_get_result(i);
        if (!station)
            continue;
        char const *ssid    = wifi_station_get_ssid(station);
        int         quality = signal_quality(wifi_station_get_rssi(station));
        bool        secured = wifi_station_get_mode(station) != WIFI_AUTH_OPEN;

        if (ssid && ssid[0]) {
            int found = -1;
            for (int n = 0; n < ctx->network_count; n++) {
                if (strcmp(ctx->networks[n].ssid, ssid) == 0) {
                    found = n;
                    break;
                }
            }
            if (found >= 0) {
                wifi_network *net = &ctx->networks[found];
                net->ap_count++;
                if (quality > net->signal_strength) {
                    net->signal_strength = quality;
                    net->secured         = secured;
                }
            } else {
                wifi_network *net = &ctx->networks[ctx->network_count++];
                snprintf(net->ssid, sizeof(net->ssid), "%s", ssid);
                net->signal_strength = quality;
                net->secured         = secured;
                net->ap_count        = 1;
                net->connected       = ctx->is_connected && strcmp(ssid, ctx->connected_ssid) == 0;
            }
        }
        wifi_scan_free_station(station);
    }

    if (ctx->network_count > 1)
        qsort(ctx->networks, ctx->network_count, sizeof(wifi_network), network_compare);

    if (ctx->selected_item >= ctx->network_count)
        ctx->selected_item = ctx->network_count > 0 ? ctx->network_count - 1 : 0;
    if (ctx->scroll_offset > ctx->selected_item)
        ctx->scroll_offset = ctx->selected_item;
}

static void draw_main_settings(app_context *ctx) {
    update_connection_info(ctx, false);
    draw_frame(ctx, "System Settings", "Up/Down: Select  Enter: Open  Esc: Exit");

    // char const *categories[]   = {"WiFi Settings", "Display Settings", "System Information", "About"};
    char const *categories[]   = {"WiFi Settings", "About"};
    char const *descriptions[] = {
        "Configure wireless network connection",
        // "Adjust screen brightness and timeout",
        // "View system status and diagnostics",
        "Application version and credits"
    };
    ctx->total_items = 2;

    int list_x      = WIN_X + 12;
    int list_y      = WIN_Y + TITLE_H + 36;
    int list_w      = WIN_W - 24;
    int list_h      = WIN_H - TITLE_H - 84;
    int item_height = 84;

    draw_list_panel(ctx, list_x, list_y, list_w, list_h);

    for (int i = 0; i < ctx->total_items; i++) {
        int  item_y = list_y + 6 + i * item_height;
        int  item_x = list_x + 6;
        int  item_w = list_w - 12;
        int  row_h  = item_height - 6;
        bool sel    = (i == ctx->selected_item);

        draw_rect(ctx, item_x, item_y, item_w, row_h, sel ? COL_ROW_SELECTED_BG : COL_PANEL);
        draw_rect(ctx, item_x, item_y + row_h, item_w, 1, COL_DIVIDER);
        if (sel)
            draw_focus(ctx, item_x, item_y, item_w, row_h);

        int icon_size  = 48;
        int icon_x     = item_x + 12;
        int icon_y_pos = item_y + (row_h - icon_size) / 2;
        draw_rect(ctx, icon_x - 3, icon_y_pos - 3, icon_size + 6, icon_size + 6, COL_FOCUS_BG);
        if (i == 0)
            icon_wifi(ctx, icon_x, icon_y_pos, icon_size, sel);
        else
            icon_about(ctx, icon_x, icon_y_pos, icon_size, sel);

        int text_x = icon_x + icon_size + 16;
        draw_text_bold(ctx, text_x, item_y + 16, categories[i], sel ? COL_ROW_SELECTED_TEXT : COL_TEXT);
        if (i == 0) {
            /* WiFi row: show the current connection instead of the description. */
            char line[96];
            if (ctx->is_connected)
                snprintf(line, sizeof(line), "Connected to %s", ctx->connected_ssid);
            else
                snprintf(line, sizeof(line), "Not connected");
            draw_text_clipped(
                ctx,
                text_x,
                item_y + 16 + 24,
                item_x + item_w - 12 - text_x,
                line,
                ctx->is_connected ? COL_SUCCESS : COL_ACCENT_3,
                false
            );
        } else {
            draw_text(ctx, text_x, item_y + 16 + 24, descriptions[i], COL_TEXT_MID);
        }
    }

    draw_footer(ctx, NULL, 0);
}

static void draw_wifi_settings(app_context *ctx) {
    draw_frame(ctx, "WiFi Settings", "Up/Down: Select  Enter: Connect  S: Scan  Esc: Back");

    update_connection_info(ctx, false);

    char status_line[160];
    if (ctx->is_connected) {
        snprintf(
            status_line,
            sizeof(status_line),
            "Connected to %s (%d dBm)",
            ctx->connected_ssid,
            ctx->connected_rssi
        );
    } else if (ctx->connection_status_text[0]) {
        snprintf(status_line, sizeof(status_line), "%s", ctx->connection_status_text);
    } else {
        snprintf(status_line, sizeof(status_line), "Not connected");
    }

    int content_y = WIN_Y + TITLE_H + 40;
    draw_text(ctx, WIN_X + 16, content_y, "Status:", COL_TEXT_MID);
    draw_text_clipped(
        ctx,
        WIN_X + 16 + get_text_width("Status: "),
        content_y,
        WIN_W - 32 - get_text_width("Status: "),
        status_line,
        ctx->is_connected ? COL_SUCCESS : COL_ACCENT_3,
        true
    );

    char avail[64];
    if (ctx->network_count)
        snprintf(avail, sizeof(avail), "Available networks (%d)", ctx->network_count);
    else
        snprintf(avail, sizeof(avail), "Available networks");
    draw_text(ctx, WIN_X + 16, content_y + 28, avail, COL_TEXT);

    int list_x      = WIN_X + 12;
    int list_y      = content_y + 54;
    int list_w      = WIN_W - 24;
    int list_h      = WIN_Y + WIN_H - FOOTER_H - 10 - list_y;
    int item_height = 60;

    draw_list_panel(ctx, list_x, list_y, list_w, list_h);

    ctx->items_per_page = (list_h - 6) / item_height;
    int visible_start   = ctx->scroll_offset;
    int visible_end     = visible_start + ctx->items_per_page;
    if (visible_end > ctx->network_count)
        visible_end = ctx->network_count;

    bool scrollbar = ctx->network_count > ctx->items_per_page;

    for (int i = visible_start; i < visible_end; i++) {
        int  item_y = list_y + 6 + (i - visible_start) * item_height;
        int  item_x = list_x + 6;
        int  item_w = list_w - 12 - (scrollbar ? 22 : 0);
        int  row_h  = item_height - 6;
        bool sel    = (i == ctx->selected_item);

        draw_rect(ctx, item_x, item_y, item_w, row_h, sel ? COL_ROW_SELECTED_BG : COL_PANEL);
        draw_rect(ctx, item_x, item_y + row_h, item_w, 1, COL_DIVIDER);
        if (sel)
            draw_focus(ctx, item_x, item_y, item_w, row_h);

        draw_text_clipped(
            ctx,
            item_x + 12,
            item_y + 9,
            item_w - 90,
            ctx->networks[i].ssid,
            sel ? COL_ROW_SELECTED_TEXT : COL_TEXT,
            true
        );

        char info[64];
        char const *kind = ctx->networks[i].secured ? "Secured" : "Open";
        if (ctx->networks[i].ap_count > 1)
            snprintf(info, sizeof(info), "%s, %d access points", kind, ctx->networks[i].ap_count);
        else
            snprintf(info, sizeof(info), "%s", kind);
        if (ctx->networks[i].connected) {
            draw_text(ctx, item_x + 12, item_y + 31, "Connected", COL_SUCCESS);
            draw_text(ctx, item_x + 12 + get_text_width("Connected  "), item_y + 31, info, COL_TEXT_MID);
        } else {
            draw_text(ctx, item_x + 12, item_y + 31, info, COL_TEXT_MID);
        }

        draw_signal_strength(ctx, item_x + item_w - 48, item_y + (row_h - 22) / 2, ctx->networks[i].signal_strength);
    }

    if (scrollbar) {
        int sbx = list_x + list_w - 20;
        int sby = list_y + 4;
        int sbh = list_h - 8;
        draw_rect(ctx, sbx, sby, 12, sbh, COL_BTN);
        draw_outline(ctx, sbx, sby, 12, sbh, COL_DIVIDER);

        int thumb_h = (sbh * ctx->items_per_page) / ctx->network_count;
        if (thumb_h < 24)
            thumb_h = 24;
        int thumb_y = sby + ((sbh - thumb_h) * ctx->scroll_offset) / (ctx->network_count - ctx->items_per_page);
        draw_rect(ctx, sbx + 2, thumb_y, 8, thumb_h, COL_ACCENT_3);
    }

    if (!ctx->network_count && (SDL_GetTicks() >= ctx->status_timer)) {
        ctx->status_timer = SDL_GetTicks() + 3000;
        ctx->status_color = COL_FOCUS_OUT;
        strcpy(ctx->status_message, "Scanning for networks...");
    }

    if (!ctx->network_count) {
        draw_text_centered(ctx, list_x, list_y + list_h / 2 - FONT_HEIGHT / 2, list_w, "Scanning...", COL_TEXT_MID);
    }

    if (ctx->status_message[0] && SDL_GetTicks() < ctx->status_timer) {
        draw_footer(ctx, ctx->status_message, ctx->status_color);
    } else {
        draw_footer(ctx, NULL, 0);
    }

    if (!ctx->network_count) {
        populate_wifi_networks(ctx);
    }
}

static void draw_connecting_screen(app_context *ctx) {
    int w = 520, h = 220;
    int x = (SCREEN_WIDTH - w) / 2;
    int y = (SCREEN_HEIGHT - h) / 2;

    draw_dialog(ctx, x, y, w, h, "Connecting to WiFi");

    int content_y = y + 72;

    char network_text[128];
    snprintf(network_text, sizeof(network_text), "Network: %s", ctx->connecting_ssid);
    draw_text_centered(ctx, x, content_y, w, network_text, COL_TEXT);

    content_y += 32;
    char security_text[64];
    snprintf(security_text, sizeof(security_text), "Security: %s", ctx->connecting_secured ? "WPA/WPA2" : "Open");
    draw_text_centered(ctx, x, content_y, w, security_text, COL_TEXT_MID);

    content_y += 48;
    draw_text_centered(ctx, x, content_y, w, "Connecting...", COL_FOCUS_OUT);
}

static void draw_password_dialog(app_context *ctx) {
    int dialog_w = 600;
    int dialog_h = 260;
    int dialog_x = (SCREEN_WIDTH - dialog_w) / 2;
    int dialog_y = (SCREEN_HEIGHT - dialog_h) / 2;

    draw_dialog(ctx, dialog_x, dialog_y, dialog_w, dialog_h, "Enter WiFi Password");

    char ssid_text[128];
    snprintf(ssid_text, sizeof(ssid_text), "Network: %s", ctx->networks[ctx->selected_network].ssid);
    draw_text_clipped(ctx, dialog_x + 20, dialog_y + 66, dialog_w - 40, ssid_text, COL_TEXT, false);

    int field_x = dialog_x + 20;
    int field_y = dialog_y + 102;
    int field_w = dialog_w - 40;
    int field_h = 38;

    draw_rect(ctx, field_x, field_y, field_w, field_h, COL_FOCUS_BG);
    draw_outline(ctx, field_x, field_y, field_w, field_h, COL_FOCUS_OUT);

    char display_buffer[128];
    if (ctx->show_password) {
        strcpy(display_buffer, ctx->password_buffer);
    } else {
        int len = strlen(ctx->password_buffer);
        for (int i = 0; i < len; i++) {
            display_buffer[i] = '*';
        }
        display_buffer[len] = '\0';
    }

    /* Keep the end of a long password (and the cursor) visible. */
    int         max_chars = (field_w - 20) / FONT_WIDTH;
    char const *shown     = display_buffer;
    if ((int)strlen(shown) > max_chars)
        shown += strlen(shown) - max_chars;

    int text_y = field_y + (field_h - FONT_HEIGHT) / 2;
    draw_text(ctx, field_x + 8, text_y, shown, COL_TEXT);

    int cursor_x = field_x + 8 + get_text_width(shown);
    if (SDL_GetTicks() % 1000 < 500) { // Blinking cursor
        draw_rect(ctx, cursor_x, text_y, 2, FONT_HEIGHT, COL_ACCENT_3);
    }

    draw_text_centered(
        ctx,
        dialog_x,
        dialog_y + dialog_h - 50,
        dialog_w,
        "Tab: Show/hide  Enter: Connect  Esc: Cancel",
        COL_TEXT_MID
    );
}

static void draw_about_dialog(app_context *ctx) {
    int dialog_w = 520;
    int dialog_h = 280;
    int dialog_x = (SCREEN_WIDTH - dialog_w) / 2;
    int dialog_y = (SCREEN_HEIGHT - dialog_h) / 2;

    draw_dialog(ctx, dialog_x, dialog_y, dialog_w, dialog_h, "About");

    int content_y = dialog_y + 78;
    draw_text_centered(ctx, dialog_x, content_y, dialog_w, "System Settings", COL_TEXT);
    draw_text_centered(ctx, dialog_x, content_y + 32, dialog_w, "Version 1.0", COL_TEXT_MID);
    draw_text_centered(ctx, dialog_x, content_y + 60, dialog_w, "BadgeVMS system settings", COL_TEXT_MID);
    draw_text_centered(ctx, dialog_x, content_y + 88, dialog_w, "WHY2025", COL_ACCENT_3);

    draw_text_centered(ctx, dialog_x, dialog_y + dialog_h - 44, dialog_w, "Enter or Esc to close", COL_TEXT_MID);
}

static void attempt_wifi_connection(app_context *ctx) {
    strcpy(ctx->connecting_ssid, ctx->networks[ctx->selected_network].ssid);
    ctx->connecting_secured = ctx->networks[ctx->selected_network].secured;

    ctx->show_password_dialog   = false;
    ctx->show_connecting_dialog = true;
    render_screen(ctx);

    wifi_disconnect();
    wifi_set_connection_parameters(
        ctx->networks[ctx->selected_network].ssid,
        ctx->networks[ctx->selected_network].secured ? ctx->password_buffer : ""
    );
    wifi_connection_status_t result = wifi_connect();

    switch (result) {
        case WIFI_CONNECTED:
            for (int i = 0; i < ctx->network_count; i++) {
                ctx->networks[i].connected = (i == ctx->selected_network);
            }
            ctx->connection_status_text[0] = '\0';
            break;

        case WIFI_ERROR_WRONG_CREDENTIALS:
            snprintf(ctx->connection_status_text, sizeof(ctx->connection_status_text), "Not connected, wrong password");
            break;

        case WIFI_ERROR:
        case WIFI_DISCONNECTED:
        default: snprintf(ctx->connection_status_text, sizeof(ctx->connection_status_text), "Connection failed"); break;
    }

    memset(ctx->password_buffer, 0, sizeof(ctx->password_buffer));
    ctx->password_cursor        = 0;
    ctx->show_connecting_dialog = false;

    ctx->current_screen = SCREEN_WIFI;

    update_connection_info(ctx, true);
    ctx->network_count = 0;
}

static void handle_key_event(app_context *ctx, SDL_Event *event) {
    SDL_Keycode key = event->key.key;

    if (ctx->show_password_dialog) {
        if (key == SDLK_ESCAPE) {
            ctx->show_password_dialog = false;
            memset(ctx->password_buffer, 0, sizeof(ctx->password_buffer));
            ctx->password_cursor = 0;
        } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            attempt_wifi_connection(ctx);
        } else if (key == SDLK_TAB) {
            ctx->show_password = !ctx->show_password;
        } else if (key == SDLK_BACKSPACE) {
            if (ctx->password_cursor > 0) {
                ctx->password_cursor--;
                ctx->password_buffer[ctx->password_cursor] = '\0';
            }
        } else if (event->type == SDL_EVENT_TEXT_INPUT) {
            if (ctx->password_cursor < 127) {
                strcat(ctx->password_buffer, event->text.text);
                ctx->password_cursor = strlen(ctx->password_buffer);
            }
        }
        return;
    }

    switch (ctx->current_screen) {
        case SCREEN_MAIN:
            if (key == SDLK_UP) {
                if (ctx->selected_item > 0)
                    ctx->selected_item--;
            } else if (key == SDLK_DOWN) {
                if (ctx->selected_item < ctx->total_items - 1)
                    ctx->selected_item++;
            } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
                switch (ctx->selected_item) {
                    case 0: ctx->current_screen = SCREEN_WIFI; break;
                    case 1:
                        ctx->current_screen = SCREEN_ABOUT;
                        break;
                        // case 1: ctx->current_screen = SCREEN_DISPLAY; break;
                        // case 2: ctx->current_screen = SCREEN_SYSTEM; break;
                }
                ctx->selected_item = 0;
                ctx->scroll_offset = 0;
            } else if (key == SDLK_ESCAPE) {
                SDL_Event quit_event;
                quit_event.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&quit_event);
            }
            break;

        case SCREEN_WIFI:
            if (key == SDLK_UP) {
                if (ctx->selected_item > 0) {
                    ctx->selected_item--;
                    if (ctx->selected_item < ctx->scroll_offset) {
                        ctx->scroll_offset = ctx->selected_item;
                    }
                }
            } else if (key == SDLK_DOWN) {
                if (ctx->selected_item < ctx->network_count - 1) {
                    ctx->selected_item++;
                    if (ctx->selected_item >= ctx->scroll_offset + ctx->items_per_page) {
                        ctx->scroll_offset = ctx->selected_item - ctx->items_per_page + 1;
                    }
                }
            } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && ctx->selected_item < ctx->network_count) {
                ctx->selected_network = ctx->selected_item;
                if (ctx->networks[ctx->selected_item].secured) {
                    ctx->show_password_dialog = true;
                    memset(ctx->password_buffer, 0, sizeof(ctx->password_buffer));
                    ctx->password_cursor = 0;
                    ctx->show_password   = false;
                } else {
                    attempt_wifi_connection(ctx);
                }
            } else if (key == SDLK_S) {
                ctx->network_count = 0;
            } else if (key == SDLK_ESCAPE) {
                ctx->current_screen = SCREEN_MAIN;
                ctx->selected_item  = 0;
                ctx->scroll_offset  = 0;
            }
            break;

        case SCREEN_ABOUT:
            if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_ESCAPE) {
                ctx->current_screen = SCREEN_MAIN;
                ctx->selected_item  = 1;
            }
            break;

        default:
            if (key == SDLK_ESCAPE) {
                ctx->current_screen = SCREEN_MAIN;
                ctx->selected_item  = 0;
            }
            break;
    }
}

static void render_screen(app_context *ctx) {
    memset(ctx->pixels, 0, SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint16_t));

    switch (ctx->current_screen) {
        case SCREEN_MAIN: draw_main_settings(ctx); break;
        case SCREEN_WIFI:
            draw_wifi_settings(ctx);
            if (ctx->show_password_dialog) {
                draw_password_dialog(ctx);
            }
            if (ctx->show_connecting_dialog) {
                draw_connecting_screen(ctx);
            }
            break;
        case SCREEN_DISPLAY: draw_main_settings(ctx); break;
        case SCREEN_SYSTEM: draw_main_settings(ctx); break;
        case SCREEN_ABOUT:
            draw_main_settings(ctx);
            draw_about_dialog(ctx);
            break;
    }

    SDL_UpdateTexture(ctx->texture, NULL, ctx->pixels, SCREEN_WIDTH * sizeof(uint16_t));
    SDL_RenderClear(ctx->renderer);
    SDL_RenderTexture(ctx->renderer, ctx->texture, NULL, NULL);
    SDL_RenderPresent(ctx->renderer);
}

int main(int argc, char *argv[]) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    app_context ctx = {0};

    ctx.window = SDL_CreateWindow("System Settings", SCREEN_WIDTH, SCREEN_HEIGHT, SDL_WINDOW_FULLSCREEN);
    if (!ctx.window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    ctx.renderer = SDL_CreateRenderer(ctx.window, NULL);
    if (!ctx.renderer) {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(ctx.window);
        SDL_Quit();
        return 1;
    }

    ctx.texture = SDL_CreateTexture(
        ctx.renderer,
        SDL_PIXELFORMAT_RGB565,
        SDL_TEXTUREACCESS_STREAMING,
        SCREEN_WIDTH,
        SCREEN_HEIGHT
    );
    if (!ctx.texture) {
        SDL_Log("SDL_CreateTexture failed: %s", SDL_GetError());
        SDL_DestroyRenderer(ctx.renderer);
        SDL_DestroyWindow(ctx.window);
        SDL_Quit();
        return 1;
    }

    ctx.pixels = calloc(SCREEN_WIDTH * SCREEN_HEIGHT, sizeof(uint16_t));
    if (!ctx.pixels) {
        SDL_Log("Failed to allocate pixel buffer");
        SDL_DestroyTexture(ctx.texture);
        SDL_DestroyRenderer(ctx.renderer);
        SDL_DestroyWindow(ctx.window);
        SDL_Quit();
        return 1;
    }

    ctx.current_screen = SCREEN_MAIN;
    ctx.selected_item  = 0;
    ctx.scroll_offset  = 0;

    SDL_StartTextInput(ctx.window);

    bool      running = true;
    SDL_Event event;

    while (running) {
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_EVENT_QUIT: running = false; break;
                case SDL_EVENT_KEY_DOWN: handle_key_event(&ctx, &event); break;
                case SDL_EVENT_TEXT_INPUT:
                    if (ctx.show_password_dialog) {
                        handle_key_event(&ctx, &event);
                    }
                    break;
            }
        }

        render_screen(&ctx);

        SDL_Delay(16); // ~60 FPS
    }

    SDL_StopTextInput(ctx.window);
    free(ctx.pixels);
    free(ctx.networks);
    SDL_DestroyTexture(ctx.texture);
    SDL_DestroyRenderer(ctx.renderer);
    SDL_DestroyWindow(ctx.window);
    SDL_Quit();

    return 0;
}
