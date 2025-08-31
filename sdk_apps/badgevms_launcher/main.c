#include "font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <badgevms/application.h>
#include <badgevms/compositor.h>
#include <badgevms/event.h>
#include <badgevms/keyboard.h>

/* ===========================================================
   Display
   =========================================================== */
#define SCREEN_WIDTH  720
#define SCREEN_HEIGHT 720

/* ===========================================================
   Color theme (RGB888) — tweak to match WHY2025 brand
   =========================================================== */
#define COL_BG           0x0A0E14  /* deep slate */
#define COL_PANEL        0x111826  /* card back */
#define COL_ACCENT       0xFF4976  /* hot pink (primary action) */
#define COL_ACCENT_2     0x13C4A3  /* teal (secondary action) */
#define COL_ACCENT_3     0x7C5CFC  /* purple (highlights) */
#define COL_TITLE_BG     0x0F1220
#define COL_TITLE_TEXT   0xFFFFFF
#define COL_TEXT         0xE5E7EB  /* light gray text */
#define COL_TEXT_MID     0x9CA3AF
#define COL_DIVIDER      0x2A3446
#define COL_BTN          0x1C2433
#define COL_BTN_TEXT     0xE5E7EB
#define COL_FOCUS_BG     0x20304A
#define COL_FOCUS_OUT    0x3B82F6  /* bright blue outline */
#define COL_ICON_MAIN    0xF97316  /* orange for some icons */

/* Selected list row */
#define COL_ROW_SELECTED_BG   0x18324C
#define COL_ROW_SELECTED_TEXT 0xFFFFFF

/* ===========================================================
   Launcher context
   =========================================================== */
typedef struct {
    window_handle_t window;
    framebuffer_t  *framebuffer;
    uint16_t       *pixels;
    application_t **applications;
    int             scroll_offset;
    int             selected_item;
    int             total_items;
    int             items_per_page;
    bool            show_about;
    bool            quit;
    int             about_index;     /* which app the About sheet shows (-1 if none) */
} Launcher_Context;

/* ===========================================================
   Helpers
   =========================================================== */

/* Convert 0xRRGGBB -> RGB565 using BadgeVMS' built-in rgb888_to_rgb565(r,g,b) */
static inline uint16_t HEX(uint32_t c) {
    return rgb888_to_rgb565((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

static void draw_rect(Launcher_Context *ctx, int x, int y, int w, int h, uint32_t rgb) {
    if (w <= 0 || h <= 0) return;
    uint16_t c = HEX(rgb);

    int x2 = x + w;
    int y2 = y + h;

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x2 > SCREEN_WIDTH)  x2 = SCREEN_WIDTH;
    if (y2 > SCREEN_HEIGHT) y2 = SCREEN_HEIGHT;

    for (int py = y; py < y2; ++py) {
        uint16_t *row = &ctx->pixels[py * SCREEN_WIDTH + x];
        for (int px = x; px < x2; ++px) {
            *row++ = c;
        }
    }
}

/* Thin lines */
static void draw_hline(Launcher_Context *ctx, int x, int y, int w, uint32_t rgb) {
    draw_rect(ctx, x, y, w, 1, rgb);
}
static void draw_vline(Launcher_Context *ctx, int x, int y, int h, uint32_t rgb) {
    draw_rect(ctx, x, y, 1, h, rgb);
}

/* Pixel-safe */
static inline void put_px(Launcher_Context *ctx, int x, int y, uint32_t rgb) {
    if (x < 0 || y < 0 || x >= SCREEN_WIDTH || y >= SCREEN_HEIGHT) return;
    ctx->pixels[y * SCREEN_WIDTH + x] = HEX(rgb);
}

/* Integer Bresenham line */
static void draw_line(Launcher_Context *ctx, int x0, int y0, int x1, int y1, uint32_t rgb) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    for (;;) {
        put_px(ctx, x0, y0, rgb);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Integer circle outline (full octants) */
static void draw_circle_outline(Launcher_Context *ctx, int cx, int cy, int r, uint32_t rgb) {
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        put_px(ctx, cx + x, cy + y, rgb);
        put_px(ctx, cx + y, cy + x, rgb);
        put_px(ctx, cx - y, cy + x, rgb);
        put_px(ctx, cx - x, cy + y, rgb);
        put_px(ctx, cx - x, cy - y, rgb);
        put_px(ctx, cx - y, cy - x, rgb);
        put_px(ctx, cx + y, cy - x, rgb);
        put_px(ctx, cx + x, cy - y, rgb);
        y++;
        if (err < 0) err += 2*y + 1;
        else { x--; err += 2*(y - x + 1); }
    }
}

/* Integer filled circle by horizontal spans */
static void draw_circle_filled(Launcher_Context *ctx, int cx, int cy, int r, uint32_t rgb) {
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        draw_hline(ctx, cx - x, cy + y, 2*x + 1, rgb);
        draw_hline(ctx, cx - x, cy - y, 2*x + 1, rgb);
        draw_hline(ctx, cx - y, cy + x, 2*y + 1, rgb);
        draw_hline(ctx, cx - y, cy - x, 2*y + 1, rgb);
        y++;
        if (err < 0) err += 2*y + 1;
        else { x--; err += 2*(y - x + 1); }
    }
}

/* Draw only the upper half of a circle outline (for Wi-Fi arcs) */
static void draw_semicircle_top_outline(Launcher_Context *ctx, int cx, int cy, int r, uint32_t rgb) {
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        /* only plot points with y <= cy (upper half) */
        put_px(ctx, cx + x, cy - y, rgb);
        put_px(ctx, cx - x, cy - y, rgb);
        put_px(ctx, cx + y, cy - x, rgb);
        put_px(ctx, cx - y, cy - x, rgb);
        y++;
        if (err < 0) err += 2*y + 1;
        else { x--; err += 2*(y - x + 1); }
    }
}

/* ===========================================================
   Font helpers (from font.h)
   =========================================================== */
static void draw_char(Launcher_Context *ctx, int x, int y, char c, uint32_t color) {
    if (c < FONT_FIRST_CHAR || c > FONT_LAST_CHAR) return;
    int idx = c - FONT_FIRST_CHAR;
    const uint16_t *rows = pixel_font[idx];
    uint16_t c565 = HEX(color);
    for (int ry = 0; ry < FONT_HEIGHT; ++ry) {
        uint16_t bits = rows[ry];
        int py = y + ry;
        if (py < 0 || py >= SCREEN_HEIGHT) continue;
        for (int col = 0; col < FONT_WIDTH; ++col) {
            if (bits & (0x800 >> col)) {
                int px = x + col;
                if (px >= 0 && px < SCREEN_WIDTH) {
                    ctx->pixels[py * SCREEN_WIDTH + px] = c565;
                }
            }
        }
    }
}
static void draw_text(Launcher_Context *ctx, int x, int y, const char *s, uint32_t color) {
    int cx = x;
    while (*s) { draw_char(ctx, cx, y, *s++, color); cx += FONT_WIDTH; }
}
static void draw_text_bold(Launcher_Context *ctx, int x, int y, const char *s, uint32_t color) {
    draw_text(ctx, x, y, s, color); draw_text(ctx, x+1, y, s, color);
}
static int text_w(const char *s) { return (int)strlen(s) * FONT_WIDTH; }
static void draw_text_center(Launcher_Context *ctx, int x, int y, int w, const char *s, uint32_t color) {
    int tw = text_w(s); draw_text(ctx, x + (w - tw)/2, y, s, color);
}

/* ===========================================================
   Simple “neo” card borders
   =========================================================== */
static void draw_card(Launcher_Context *ctx, int x, int y, int w, int h) {
    draw_rect(ctx, x, y, w, h, COL_PANEL);
    draw_hline(ctx, x, y, w, COL_DIVIDER);
    draw_hline(ctx, x, y + h - 1, w, COL_DIVIDER);
    draw_vline(ctx, x, y, h, COL_DIVIDER);
    draw_vline(ctx, x + w - 1, y, h, COL_DIVIDER);
}

/* Focus glow (1px outline) */
static void draw_focus(Launcher_Context *ctx, int x, int y, int w, int h) {
    draw_hline(ctx, x-2, y-2, w+4, COL_FOCUS_OUT);
    draw_hline(ctx, x-2, y + h + 1, w+4, COL_FOCUS_OUT);
    draw_vline(ctx, x-2, y-2, h+4, COL_FOCUS_OUT);
    draw_vline(ctx, x + w + 1, y-2, h+4, COL_FOCUS_OUT);
}

/* ===========================================================
   Vector-ish icons (integer-only)
   =========================================================== */
static void icon_browser(Launcher_Context *ctx, int x, int y, int sz, bool selected) {
    int r = sz/3;
    int cx = x + sz/2 - 2;
    int cy = y + sz/2 - 2;
    uint32_t a = selected ? COL_ACCENT_2 : COL_ACCENT;
    uint32_t b = COL_TEXT;

    draw_circle_filled(ctx, cx, cy, r, a);
    /* simple magnifier handle */
    draw_line(ctx, cx + r/2, cy + r/2, x + sz - 3, y + sz - 3, b);
    draw_line(ctx, cx + r/2 + 1, cy + r/2, x + sz - 2, y + sz - 3, b);
}

static void icon_settings(Launcher_Context *ctx, int x, int y, int sz, bool selected) {
    int r = sz/3;
    int cx = x + sz/2;
    int cy = y + sz/2;
    uint32_t c1 = selected ? COL_ACCENT_3 : COL_ACCENT_2;
    uint32_t c2 = COL_TEXT;

    /* 8 spokes at fixed integer directions (no trig) */
    const int dirs[8][2] = {
        { 1, 0}, { 1, 1}, { 0, 1}, {-1, 1},
        {-1, 0}, {-1,-1}, { 0,-1}, { 1,-1}
    };
    for (int i = 0; i < 8; ++i) {
        int dx = dirs[i][0], dy = dirs[i][1];
        int x1 = cx + dx * (r + 2);
        int y1 = cy + dy * (r + 2);
        int x2 = cx + dx * (r + 6);
        int y2 = cy + dy * (r + 6);
        draw_line(ctx, x1, y1, x2, y2, c2);
    }
    draw_circle_filled(ctx, cx, cy, r, c1);
    draw_circle_filled(ctx, cx, cy, r/2, COL_PANEL);
}

static void icon_settings2(Launcher_Context *ctx, int x, int y, int sz, bool selected) {
    int r = sz/3;
    int cx = x + sz/2;
    int cy = y + sz/2;
    uint32_t c1 = selected ? COL_ACCENT_2 : COL_ACCENT;
    uint32_t c2 = COL_TEXT;

    const int dirs[8][2] = {
        { 1, 0}, { 1, 1}, { 0, 1}, {-1, 1},
        {-1, 0}, {-1,-1}, { 0,-1}, { 1,-1}
    };
    for (int i = 0; i < 8; ++i) {
        int dx = dirs[i][0], dy = dirs[i][1];
        int x1 = cx + dx * (r + 2);
        int y1 = cy + dy * (r + 2);
        int x2 = cx + dx * (r + 6);
        int y2 = cy + dy * (r + 6);
        draw_line(ctx, x1, y1, x2, y2, c2);
    }
    draw_circle_filled(ctx, cx, cy, r, c1);
    draw_circle_filled(ctx, cx, cy, r/2, COL_PANEL);
}

static void icon_wifi(Launcher_Context *ctx, int x, int y, int sz, bool selected) {
    uint32_t c = selected ? COL_ACCENT : COL_ACCENT_2;
    int cx = x + sz/2;
    int base = y + sz/2 + 6;
    /* three top semi-circles, increasing radius */
    draw_semicircle_top_outline(ctx, cx, base, sz/6, c);
    draw_semicircle_top_outline(ctx, cx, base, sz/4, c);
    draw_semicircle_top_outline(ctx, cx, base, sz/3, c);
    draw_circle_filled(ctx, cx, base + 6, 3, c);
}

static void icon_snake(Launcher_Context *ctx, int x, int y, int sz, bool selected) {
    uint32_t c = selected ? COL_ACCENT : COL_ICON_MAIN;
    int left = x + sz/6;
    int right = x + sz - sz/6;
    int midy = y + sz/2;
    draw_line(ctx, left, midy, right, midy, c);
    draw_line(ctx, right, midy, right-6, midy-6, c);
    draw_line(ctx, right, midy, right-6, midy+6, c);
}

static void icon_chip(Launcher_Context *ctx, int x, int y, int sz, bool selected) {
    uint32_t c1 = selected ? COL_ACCENT_2 : COL_ACCENT_3;
    uint32_t c2 = COL_TEXT;
    draw_rect(ctx, x + 4, y + 4, sz - 8, sz - 8, c1);
    for (int k = 2; k < sz-2; k += 6) {
        draw_rect(ctx, x + k, y + 1, 2, 3, c2);
        draw_rect(ctx, x + k, y + sz - 4, 2, 3, c2);
        draw_rect(ctx, x + 1, y + k, 3, 2, c2);
        draw_rect(ctx, x + sz - 4, y + k, 3, 2, c2);
    }
}

static void icon_hello(Launcher_Context *ctx, int x, int y, int sz, bool selected) {
    uint32_t c = selected ? COL_ACCENT : COL_ACCENT_2;
    draw_rect(ctx, x + 4, y + 6, sz - 8, sz/2, c);
    draw_rect(ctx, x + 8, y + sz/2 + 2, 8, 6, c);
}

static void icon_plug(Launcher_Context *ctx, int x, int y, int sz, bool selected) {
    uint32_t c = selected ? COL_ACCENT : COL_TEXT;
    draw_rect(ctx, x + sz/4, y + sz/3, sz/2, sz/3, c);
    draw_rect(ctx, x + sz/3, y + sz/6, 3, sz/6, c);
    draw_rect(ctx, x + sz/2, y + sz/6, 3, sz/6, c);
    draw_rect(ctx, x + sz/3 + 1, y + sz - 6, sz/3, 3, c);
}

static void icon_star(Launcher_Context *ctx, int x, int y, int sz, bool selected) {
    uint32_t c = selected ? COL_ACCENT_3 : COL_ACCENT;
    int cx = x + sz/2, cy = y + sz/2, r = sz/3;
    int px[5] = { cx, cx + r, cx + r/3, cx - r/3, cx - r };
    int py[5] = { cy - r, cy - r/3, cy + r, cy + r, cy - r/3 };
    for (int i = 0; i < 5; ++i) {
        int j = (i + 2) % 5;
        draw_line(ctx, px[i], py[i], px[j], py[j], c);
    }
}

/* UID->icon mapping */
static void draw_app_icon(Launcher_Context *ctx, const char *uid, int x, int y, int sz, bool selected) {
    if (!uid) uid = "";
    if (strstr(uid, "mini_browser"))      { icon_browser(ctx, x, y, sz, selected); return; }
    if (strstr(uid, "settings"))          { icon_settings(ctx, x, y, sz, selected); return; }
    if (strstr(uid, "why2025_namebadge")) { icon_star(ctx,     x, y, sz, selected); return; }
    if (strstr(uid, "wifi"))              { icon_wifi(ctx,     x, y, sz, selected); return; }
    if (strstr(uid, "sdl_test"))          { icon_snake(ctx,    x, y, sz, selected); return; }
    if (strstr(uid, "doom"))              { icon_chip(ctx,     x, y, sz, selected); return; }
    if (strstr(uid, "curl"))              { icon_plug(ctx,     x, y, sz, selected); return; }
    if (strstr(uid, "serial"))            { icon_plug(ctx,     x, y, sz, selected); return; }
    if (strstr(uid, "hardware"))          { icon_chip(ctx,     x, y, sz, selected); return; }
    if (strstr(uid, "hello"))             { icon_hello(ctx,    x, y, sz, selected); return; }
    if (strstr(uid, "sponsors"))          { icon_star(ctx,     x, y, sz, selected); return; }
    if (strstr(uid, "ota"))               { icon_settings2(ctx, x, y, sz, selected); return; }
    icon_chip(ctx, x, y, sz, selected);
}

/* ===========================================================
   UI drawing
   =========================================================== */
static void draw_title_bar(Launcher_Context *ctx, int x, int y, int w, int h, const char *title) {
    draw_rect(ctx, x, y, w, h, COL_TITLE_BG);
    draw_text_bold(ctx, x + 16, y + (h - FONT_HEIGHT)/2, title, COL_TITLE_TEXT);
}

static void draw_window(Launcher_Context *ctx) {
    const int wx = 24, wy = 24, ww = SCREEN_WIDTH - 48, wh = SCREEN_HEIGHT - 48;
    draw_rect(ctx, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COL_BG);

    draw_card(ctx, wx, wy, ww, wh);

    const int th = 48;
    draw_title_bar(ctx, wx + 2, wy + 2, ww - 4, th, "WHY Launcher");

    char info[64];
    snprintf(info, sizeof(info), "Use \x18/\x19 to navigate  Enter: Launch  A: About app  ESC: Exit");
    draw_text(ctx, wx + 16, wy + th + 16, info, COL_TEXT_MID);

    const int list_y = wy + th + 44;
    const int list_h = wh - th - 92;
    const int item_h = 78;
    const int list_x = wx + 12;
    const int list_w = ww - 24;

    draw_rect(ctx, list_x, list_y, list_w, list_h, COL_PANEL);
    draw_rect(ctx, list_x, list_y, list_w, 1, COL_DIVIDER);
    draw_rect(ctx, list_x, list_y + list_h - 1, list_w, 1, COL_DIVIDER);
    draw_rect(ctx, list_x, list_y, 1, list_h, COL_DIVIDER);
    draw_rect(ctx, list_x + list_w - 1, list_y, 1, list_h, COL_DIVIDER);

    draw_rect(ctx, wx + 2, wy + wh - 40, ww - 4, 38, COL_BTN);
    draw_text(ctx, wx + 16, wy + wh - 32, "WHY2025 • BadgeVMS", COL_BTN_TEXT);

    ctx->items_per_page = (list_h - 6) / item_h;
    int start = ctx->scroll_offset;
    int end   = start + ctx->items_per_page;
    if (end > ctx->total_items) end = ctx->total_items;

    for (int i = start; i < end; ++i) {
        int iy = list_y + 3 + (i - start) * item_h;
        int ix = list_x + 6;
        int iw = list_w - 12;

        bool sel = (i == ctx->selected_item);
        uint32_t row_bg   = sel ? COL_ROW_SELECTED_BG   : COL_PANEL;
        uint32_t row_text = sel ? COL_ROW_SELECTED_TEXT : COL_TEXT;

        draw_rect(ctx, ix, iy, iw, item_h - 6, row_bg);
        draw_hline(ctx, ix, iy + item_h - 6, iw, COL_DIVIDER);

        if (sel) draw_focus(ctx, ix, iy, iw, item_h - 6);

        int icon_sz = 48;
        int icon_x  = ix + 12;
        int icon_y  = iy + (item_h - 6 - icon_sz)/2;
        draw_rect(ctx, icon_x - 3, icon_y - 3, icon_sz + 6, icon_sz + 6, COL_FOCUS_BG);
        draw_app_icon(ctx, ctx->applications[i]->unique_identifier, icon_x, icon_y, icon_sz, sel);

        const char *name = ctx->applications[i]->name ? ctx->applications[i]->name : "(unnamed)";
        draw_text_bold(ctx, icon_x + icon_sz + 16, iy + 10, name, row_text);

        if (ctx->applications[i]->version) {
            char ver[32];
            snprintf(ver, sizeof(ver), "v%s", ctx->applications[i]->version);
            draw_text(ctx, icon_x + icon_sz + 16, iy + 10 + 22, ver, COL_TEXT_MID);
        }
    }

    /* scrollbar */
    if (ctx->total_items > ctx->items_per_page) {
        int sbx = list_x + list_w - 20;
        int sby = list_y + 4;
        int sbh = list_h - 8;
        draw_rect(ctx, sbx, sby, 12, sbh, COL_BTN);
        draw_rect(ctx, sbx, sby, 12, 1, COL_DIVIDER);
        draw_rect(ctx, sbx, sby + sbh - 1, 12, 1, COL_DIVIDER);

        int thumb_h = (sbh * ctx->items_per_page) / ctx->total_items;
        if (thumb_h < 24) thumb_h = 24;
        int thumb_y = sby;
        if (ctx->total_items > ctx->items_per_page) {
            thumb_y += ((sbh - thumb_h) * ctx->scroll_offset) / (ctx->total_items - ctx->items_per_page);
        }
        draw_rect(ctx, sbx + 2, thumb_y, 8, thumb_h, COL_ACCENT_3);
    }
}

/* ===========================================================
   About dialog (per selected app)
   =========================================================== */
static void draw_about(Launcher_Context *ctx, application_t *app) {
    int w = 520, h = 320;
    int x = (SCREEN_WIDTH  - w)/2;
    int y = (SCREEN_HEIGHT - h)/2;

    draw_rect(ctx, x-6, y-6, w+12, h+12, COL_BG);
    draw_card(ctx, x, y, w, h);
    draw_title_bar(ctx, x + 2, y + 2, w - 4, 42, "About");

    const char *name = (app && app->name) ? app->name : "(unknown)";
    const char *ver  = (app && app->version && app->version[0]) ? app->version : "-";
    const char *uid  = (app && app->unique_identifier) ? app->unique_identifier : "-";
    const char *bin  = (app && app->binary_path && app->binary_path[0]) ? app->binary_path : "-";

    char line[256];
    draw_text_center(ctx, x, y + 78,  w, name, COL_TEXT);

    snprintf(line, sizeof(line), "Version: %s", ver);
    draw_text_center(ctx, x, y + 110, w, line, COL_TEXT_MID);

    snprintf(line, sizeof(line), "UID: %s", uid);
    draw_text_center(ctx, x, y + 138, w, line, COL_TEXT_MID);

    snprintf(line, sizeof(line), "Binary: %s", bin);
    draw_text_center(ctx, x, y + 166, w, line, COL_TEXT_MID);

    draw_text_center(ctx, x, y + 220, w, "ENTER or ESC to close", COL_TEXT_MID);
}

/* ===========================================================
   Input handling
   =========================================================== */
static void handle_keyboard(Launcher_Context *ctx, keyboard_scancode_t code) {
    if (ctx->show_about) {
        if (code == KEY_SCANCODE_ESCAPE || code == KEY_SCANCODE_RETURN || code == KEY_SCANCODE_SPACE) {
            ctx->show_about  = false;
            ctx->about_index = -1;
        }
        return;
    }

    switch (code) {
        case KEY_SCANCODE_UP:
            if (ctx->selected_item > 0) {
                ctx->selected_item--;
                if (ctx->selected_item < ctx->scroll_offset) {
                    ctx->scroll_offset = ctx->selected_item;
                }
            }
            break;

        case KEY_SCANCODE_DOWN:
            if (ctx->selected_item < ctx->total_items - 1) {
                ctx->selected_item++;
                if (ctx->selected_item >= ctx->scroll_offset + ctx->items_per_page) {
                    ctx->scroll_offset = ctx->selected_item - ctx->items_per_page + 1;
                }
            }
            break;

        case KEY_SCANCODE_RETURN:
        case KEY_SCANCODE_SPACE:
            printf("Launching: %s\n", ctx->applications[ctx->selected_item]->name);
            application_launch(ctx->applications[ctx->selected_item]->unique_identifier);
            break;

        case KEY_SCANCODE_A:
            ctx->about_index = ctx->selected_item;  /* show About for selected app */
            ctx->show_about  = true;
            break;

        case KEY_SCANCODE_ESCAPE:
            ctx->quit = true;
            break;

        default: break;
    }
}

/* ===========================================================
   Priority bucketing (optional)
   =========================================================== */
static bool is_priority_uid(const char *uid) {
    if (!uid) return false;
    return
        (strcmp(uid, "mini_browser") == 0) ||
        (strcmp(uid, "badgevms_settings") == 0) ||
        (strcmp(uid, "why2025_namebadge") == 0);
}

/* ===========================================================
   Main loop
   =========================================================== */
static bool run_launcher(application_t **apps, size_t num) {
    Launcher_Context ctx = (Launcher_Context){0};
    ctx.applications  = apps;
    ctx.total_items   = (int)num;
    ctx.about_index   = -1;

    ctx.window = window_create(
        "Application Launcher",
        (window_size_t){SCREEN_WIDTH, SCREEN_HEIGHT},
        WINDOW_FLAG_DOUBLE_BUFFERED | WINDOW_FLAG_FULLSCREEN | WINDOW_FLAG_LOW_PRIORITY
    );
    if (!ctx.window) { printf("Window could not be created\n"); return false; }

    ctx.framebuffer = window_framebuffer_create(
        ctx.window, (window_size_t){SCREEN_WIDTH, SCREEN_HEIGHT}, BADGEVMS_PIXELFORMAT_RGB565
    );
    if (!ctx.framebuffer) { printf("Framebuffer texture could not be created\n"); return false; }

    ctx.pixels = ctx.framebuffer->pixels;

    while (!ctx.quit) {
        memset(ctx.pixels, 0, SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint16_t));

        draw_window(&ctx);
        if (ctx.show_about) {
            application_t *a = NULL;
            if (ctx.about_index >= 0 && ctx.about_index < ctx.total_items) {
                a = ctx.applications[ctx.about_index];
            }
            draw_about(&ctx, a);
        }

        window_present(ctx.window, true, NULL, 0);

        event_t e = window_event_poll(ctx.window, true, 0);
        if (e.type == EVENT_QUIT) {
            ctx.quit = true;
        } else if (e.type == EVENT_KEY_DOWN) {
            handle_keyboard(&ctx, e.keyboard.scancode);
        }
    }

    return true;
}

/* ===========================================================
   Entry point
   =========================================================== */
int main(int argc, char *argv[]) {
    application_t          *app;
    application_list_handle list = application_list(&app);

    application_t **prio = NULL, **rest = NULL;
    size_t n_prio = 0, n_rest = 0;

    printf("Currently installed applications:\n");
    while (app) {
        printf("Name: %s\n", app->name);
        printf("  UID: %s\n", app->unique_identifier);
        printf("  Version: %s\n", app->version);
        printf("  Binary : %s\n", app->binary_path);

        if (app->binary_path && strlen(app->binary_path) &&
            app->unique_identifier &&
            (strcmp(app->unique_identifier, "badgevms_launcher") != 0) &&
            (strcmp(app->unique_identifier, "why2025_firmware_ota_c6") != 0)) {

            if (is_priority_uid(app->unique_identifier)) {
                prio = (application_t**)realloc(prio, sizeof(*prio) * (n_prio + 1));
                prio[n_prio++] = app;
                printf("  -> BUCKET: PRIORITY\n");
            } else {
                rest = (application_t**)realloc(rest, sizeof(*rest) * (n_rest + 1));
                rest[n_rest++] = app;
                printf("  -> BUCKET: REST\n");
            }
        }
        app = application_list_get_next(list);
    }

    size_t num = n_prio + n_rest;
    application_t **apps = (application_t**)malloc(sizeof(*apps) * num);
    size_t idx = 0;
    for (size_t i = 0; i < n_prio; ++i) apps[idx++] = prio[i];
    for (size_t i = 0; i < n_rest; ++i) apps[idx++] = rest[i];

    for (size_t i = 0; i < num; ++i) {
        printf("FINAL_ORDER[%zu]: %s (%s)\n", i, apps[i]->name, apps[i]->unique_identifier);
    }

    bool ok = run_launcher(apps, num);

    free(prio);
    free(rest);
    free(apps);

    return ok ? 0 : 1;
}
