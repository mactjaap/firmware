/* ===========================================================
   Badge VMS with icons
   =========================================================== */

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
   Color theme (RGB888) — WHY2025 palette
   =========================================================== */
#define COL_BG           0x0A0E14  /* deep slate */
#define COL_PANEL        0x111826  /* card back */

/* WHY2025 Accent Colours (0xRRGGBB) */
#define COL_ACCENT_1   0xFFFB96  /* yellow */
#define COL_ACCENT_2   0x64EFFE  /* cyan */
#define COL_ACCENT_3   0xF25E95  /* pink/rose */
#define COL_ACCENT_4   0x5233BF  /* purple */
#define COL_ACCENT_5   0x2E1A64  /* dark purple */
#define COL_ACCENT_6   0xF24436  /* red */

#define COL_TITLE_BG     0x0F1220
#define COL_TITLE_TEXT   0xFFFFFF
#define COL_TEXT         0xE5E7EB
#define COL_TEXT_MID     0x9CA3AF
#define COL_DIVIDER      0x2A3446
#define COL_BTN          0x1C2433
#define COL_BTN_TEXT     0xE5E7EB
#define COL_FOCUS_BG     0x1A2232
#define COL_FOCUS_OUT    0x64EFFE  /* cyan outline */

/* Selected list row */
#define COL_ROW_SELECTED_BG   0x18283C
#define COL_ROW_SELECTED_TEXT 0xFFFFFF

/* ===========================================================
   Minimal framebuffer view for helpers
   =========================================================== */
typedef struct { uint16_t *pixels; } fbview_t;

/* Convert 0xRRGGBB -> RGB565 using BadgeVMS converter */
static inline uint16_t HEX(uint32_t c) {
    return rgb888_to_rgb565((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

static inline void put_px(fbview_t *ctx, int x, int y, uint32_t rgb) {
    if ((unsigned)x >= SCREEN_WIDTH || (unsigned)y >= SCREEN_HEIGHT) return;
    ctx->pixels[y * SCREEN_WIDTH + x] = HEX(rgb);
}

static void draw_rect(fbview_t *ctx, int x, int y, int w, int h, uint32_t rgb) {
    if (w <= 0 || h <= 0) return;
    uint16_t c = HEX(rgb);
    int x2 = x + w, y2 = y + h;
    if (x < 0) x = 0; if (y < 0) y = 0;
    if (x2 > SCREEN_WIDTH)  x2 = SCREEN_WIDTH;
    if (y2 > SCREEN_HEIGHT) y2 = SCREEN_HEIGHT;
    for (int py = y; py < y2; ++py) {
        uint16_t *row = &ctx->pixels[py * SCREEN_WIDTH + x];
        for (int px = x; px < x2; ++px) *row++ = c;
    }
}
static void draw_hline(fbview_t *ctx, int x, int y, int w, uint32_t rgb){ draw_rect(ctx,x,y,w,1,rgb); }
static void draw_vline(fbview_t *ctx, int x, int y, int h, uint32_t rgb){ draw_rect(ctx,x,y,1,h,rgb); }

/* Bresenham line (1px) */
static void draw_line(fbview_t *ctx, int x0,int y0,int x1,int y1,uint32_t rgb){
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    for (;;){
        put_px(ctx,x0,y0,rgb);
        if (x0==x1 && y0==y1) break;
        e2 = 2*err;
        if (e2 >= dy){ err += dy; x0 += sx; }
        if (e2 <= dx){ err += dx; y0 += sy; }
    }
}

/* Thick line by stamping a small square around each plotted pixel */
static void draw_thick_line(fbview_t *ctx, int x0,int y0,int x1,int y1,uint32_t rgb,int thick){
    if (thick <= 1){ draw_line(ctx,x0,y0,x1,y1,rgb); return; }
    int half = thick/2;
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    for (;;){
        draw_rect(ctx, x0 - half, y0 - half, thick, thick, rgb);
        if (x0==x1 && y0==y1) break;
        e2 = 2*err;
        if (e2 >= dy){ err += dy; x0 += sx; }
        if (e2 <= dx){ err += dx; y0 += sy; }
    }
}

/* Circles */
static void draw_circle_filled(fbview_t *ctx, int cx,int cy,int r,uint32_t rgb){
    int x=r,y=0,err=1-r;
    while (x>=y){
        draw_hline(ctx,cx-x,cy+y,2*x+1,rgb);
        draw_hline(ctx,cx-x,cy-y,2*x+1,rgb);
        draw_hline(ctx,cx-y,cy+x,2*y+1,rgb);
        draw_hline(ctx,cx-y,cy-x,2*y+1,rgb);
        y++;
        if (err<0) err += 2*y + 1;
        else { x--; err += 2*(y - x + 1); }
    }
}
static void draw_semicircle_top_outline(fbview_t *ctx,int cx,int cy,int r,uint32_t rgb){
    int x=r,y=0,err=1-r;
    while (x>=y){
        put_px(ctx,cx+x,cy-y,rgb);
        put_px(ctx,cx-x,cy-y,rgb);
        put_px(ctx,cx+y,cy-x,rgb);
        put_px(ctx,cx-y,cy-x,rgb);
        y++;
        if (err<0) err += 2*y + 1;
        else { x--; err += 2*(y - x + 1); }
    }
}

/* ===========================================================
   Font helpers (from font.h)
   =========================================================== */
static void draw_char(fbview_t *ctx, int x, int y, char c, uint32_t color) {
    if (c < FONT_FIRST_CHAR || c > FONT_LAST_CHAR) return;
    int idx = c - FONT_FIRST_CHAR;
    const uint16_t *rows = pixel_font[idx];
    uint16_t c565 = HEX(color);
    for (int ry = 0; ry < FONT_HEIGHT; ++ry) {
        uint16_t bits = rows[ry];
        int py = y + ry; if ((unsigned)py >= SCREEN_HEIGHT) continue;
        for (int col = 0; col < FONT_WIDTH; ++col) {
            if (bits & (0x800 >> col)) {
                int px = x + col; if ((unsigned)px >= SCREEN_WIDTH) continue;
                ctx->pixels[py * SCREEN_WIDTH + px] = c565;
            }
        }
    }
}
static void draw_text(fbview_t *ctx, int x, int y, const char *s, uint32_t color){
    for (int cx=x; *s; ++s, cx += FONT_WIDTH) draw_char(ctx, cx, y, *s, color);
}
static void draw_text_bold(fbview_t *ctx, int x, int y, const char *s, uint32_t color){
    draw_text(ctx,x,y,s,color); draw_text(ctx,x+1,y,s,color);
}
static int text_w(const char *s){ return (int)strlen(s) * FONT_WIDTH; }
static void draw_text_center(fbview_t *ctx,int x,int y,int w,const char *s,uint32_t color){
    int tw = text_w(s); draw_text(ctx, x + (w - tw)/2, y, s, color);
}

/* ===========================================================
   Cards & focus
   =========================================================== */
static void draw_card(fbview_t *ctx, int x, int y, int w, int h) {
    draw_rect(ctx, x, y, w, h, COL_PANEL);
    draw_hline(ctx, x, y, w, COL_DIVIDER);
    draw_hline(ctx, x, y + h - 1, w, COL_DIVIDER);
    draw_vline(ctx, x, y, h, COL_DIVIDER);
    draw_vline(ctx, x + w - 1, y, h, COL_DIVIDER);
}
static void draw_focus(fbview_t *ctx, int x, int y, int w, int h) {
    draw_hline(ctx, x-2, y-2, w+4, COL_FOCUS_OUT);
    draw_hline(ctx, x-2, y + h + 1, w+4, COL_FOCUS_OUT);
    draw_vline(ctx, x-2, y-2, h+4, COL_FOCUS_OUT);
    draw_vline(ctx, x + w + 1, y-2, h+4, COL_FOCUS_OUT);
}

/* ===========================================================
   Vector icons — Mini Browser 2.6 launcher makeover

   All icons are drawn from framebuffer primitives.  There are no image
   files, decoders or heap allocations.  The Mini Browser icon deliberately
   ignores the selected state so its normal and highlighted appearance are
   identical; selection is already communicated by the launcher row/focus.
   =========================================================== */
static void draw_circle_outline(fbview_t *ctx, int cx,int cy,int r,uint32_t rgb,int thick){
    if (r <= 0) return;
    if (thick < 1) thick = 1;
    for (int t=0; t<thick; ++t) {
        int rr = r - t;
        if (rr <= 0) break;
        int x=rr,y=0,err=1-rr;
        while (x>=y){
            put_px(ctx,cx+x,cy+y,rgb); put_px(ctx,cx+y,cy+x,rgb);
            put_px(ctx,cx-y,cy+x,rgb); put_px(ctx,cx-x,cy+y,rgb);
            put_px(ctx,cx-x,cy-y,rgb); put_px(ctx,cx-y,cy-x,rgb);
            put_px(ctx,cx+y,cy-x,rgb); put_px(ctx,cx+x,cy-y,rgb);
            y++;
            if (err<0) err += 2*y + 1;
            else { x--; err += 2*(y - x + 1); }
        }
    }
}

/* Mini Browser: same cyan/blue globe + purple/magenta orbit as 2.6. */
static void icon_browser(fbview_t *ctx, int x,int y,int sz,bool sel){
    (void)sel; /* intentionally identical in normal and selected rows */
    int cx = x + sz*46/100;
    int cy = y + sz*52/100;
    int r  = sz*31/100;
    int t  = (sz >= 40) ? 2 : 1;

    /* Dark tile blends into the launcher's icon well. */
    draw_rect(ctx, x, y, sz, sz, 0x080A0C);

    /* Globe shell and latitude grid. */
    draw_circle_outline(ctx,cx,cy,r,0x00DCFF,t);
    draw_thick_line(ctx,cx-r+3,cy-r/3,cx+r-3,cy-r/3,0x00BFEF,t);
    draw_thick_line(ctx,cx-r,  cy,    cx+r,  cy,    0x00DCFF,t);
    draw_thick_line(ctx,cx-r+3,cy+r/3,cx+r-3,cy+r/3,0x008CFF,t);

    /* Longitude curves, intentionally angular at this icon resolution. */
    draw_thick_line(ctx,cx,cy-r,cx,cy+r,0x00DCFF,t);
    draw_thick_line(ctx,cx-3,cy-r+2,cx-r/2,cy,0x00BFEF,t);
    draw_thick_line(ctx,cx-r/2,cy,cx-3,cy+r-2,0x008CFF,t);
    draw_thick_line(ctx,cx+3,cy-r+2,cx+r/2,cy,0x6E62FF,t);
    draw_thick_line(ctx,cx+r/2,cy,cx+3,cy+r-2,0x6E62FF,t);

    /* Orbital ring: cyan comes in from the left, purple exits top-right. */
    int ox0=x+1,      oy0=y+sz*72/100;
    int ox1=x+sz/5,   oy1=y+sz*70/100;
    int ox2=x+sz*2/5, oy2=y+sz*59/100;
    int ox3=x+sz*3/5, oy3=y+sz*47/100;
    int ox4=x+sz*4/5, oy4=y+sz*31/100;
    int ox5=x+sz-4,   oy5=y+sz*17/100;
    draw_thick_line(ctx,ox0,oy0,ox1,oy1,0x00E6FF,t+1);
    draw_thick_line(ctx,ox1,oy1,ox2,oy2,0x00D7FF,t+1);
    draw_thick_line(ctx,ox2,oy2,ox3,oy3,0x6D45FF,t+1);
    draw_thick_line(ctx,ox3,oy3,ox4,oy4,0xA92CFF,t+1);
    draw_thick_line(ctx,ox4,oy4,ox5,oy5,0xE327F4,t+1);

    /* Magenta planet and tiny highlight. */
    int pr = (sz >= 40) ? 5 : 3;
    draw_circle_filled(ctx,ox5,oy5,pr,0xE923F4);
    draw_circle_filled(ctx,ox5-1,oy5-1,(pr>=4)?2:1,0xFF9CFF);
}

/* Settings: cyan/purple gear with a bright hub. */
static void icon_settings(fbview_t *ctx, int x,int y,int sz,bool sel){
    int cx=x+sz/2, cy=y+sz/2, r=sz/4;
    uint32_t gear = sel ? 0x64EFFE : 0x8B5CFF;
    uint32_t tip  = sel ? 0xF25E95 : 0x64EFFE;
    const int d[8][2]={{1,0},{1,1},{0,1},{-1,1},{-1,0},{-1,-1},{0,-1},{1,-1}};
    for(int i=0;i<8;i++){
        int dx=d[i][0],dy=d[i][1];
        draw_thick_line(ctx,cx+dx*(r-1),cy+dy*(r-1),cx+dx*(r+7),cy+dy*(r+7),tip,3);
    }
    draw_circle_filled(ctx,cx,cy,r+2,gear);
    draw_circle_filled(ctx,cx,cy,r/2,0x0A0E14);
    draw_circle_outline(ctx,cx,cy,r/2+2,0xFFFFFF,1);
}

/* OTA/update: rocket instead of a second gear. */
static void icon_ota(fbview_t *ctx, int x,int y,int sz,bool sel){
    uint32_t body = sel ? 0xFFFFFF : 0x64EFFE;
    uint32_t nose = sel ? 0xF25E95 : 0xA92CFF;
    uint32_t fire = 0xFFFB96;
    int cx=x+sz/2;
    draw_circle_filled(ctx,cx,y+sz/3,sz/7,nose);
    draw_rect(ctx,cx-sz/7,y+sz/3,2*(sz/7)+1,sz/3,body);
    draw_thick_line(ctx,cx-sz/7,y+sz*3/5,cx-sz/4,y+sz*3/4,nose,3);
    draw_thick_line(ctx,cx+sz/7,y+sz*3/5,cx+sz/4,y+sz*3/4,nose,3);
    draw_thick_line(ctx,cx,y+sz*2/3,cx,y+sz-5,fire,4);
    draw_circle_filled(ctx,cx,y+sz/2,sz/14,0x0A0E14);
}

/* Wi-Fi: clean three-arc radio mark with pink status dot. */
static void icon_wifi(fbview_t *ctx, int x,int y,int sz,bool sel){
    uint32_t c = sel ? 0x64EFFE : 0xFFFFFF;
    int cx=x+sz/2, base=y+sz*3/4;
    draw_semicircle_top_outline(ctx,cx,base,sz/6,c);
    draw_semicircle_top_outline(ctx,cx,base,sz/4,c);
    draw_semicircle_top_outline(ctx,cx,base,sz/3,c);
    /* thicken the arcs with a second shifted pass */
    draw_semicircle_top_outline(ctx,cx,base+1,sz/6,c);
    draw_semicircle_top_outline(ctx,cx,base+1,sz/4,c);
    draw_semicircle_top_outline(ctx,cx,base+1,sz/3,c);
    draw_circle_filled(ctx,cx,base+2,4,sel?0xF25E95:0xFFFB96);
}

/* Snake: a real little pixel snake, rather than a straight arrow. */
static void icon_snake(fbview_t *ctx, int x,int y,int sz,bool sel){
    uint32_t c = sel ? 0x64EFFE : 0xF25E95;
    uint32_t head = sel ? 0xFFFB96 : 0x8B5CFF;
    int t=4;
    draw_thick_line(ctx,x+7,y+sz*2/3,x+sz/3,y+sz*2/3,c,t);
    draw_thick_line(ctx,x+sz/3,y+sz*2/3,x+sz/3,y+sz/3,c,t);
    draw_thick_line(ctx,x+sz/3,y+sz/3,x+sz*2/3,y+sz/3,c,t);
    draw_thick_line(ctx,x+sz*2/3,y+sz/3,x+sz*2/3,y+sz/2,c,t);
    draw_circle_filled(ctx,x+sz*2/3,y+sz/2,6,head);
    put_px(ctx,x+sz*2/3+2,y+sz/2-2,0x0A0E14);
    draw_thick_line(ctx,x+sz*2/3+5,y+sz/2+1,x+sz-3,y+sz/2+4,0xF24436,1);
}

/* Chip/hardware/DOOM: neon microchip with a dark silicon core. */
static void icon_chip(fbview_t *ctx, int x,int y,int sz,bool sel){
    uint32_t edge = sel ? 0xF25E95 : 0x64EFFE;
    uint32_t core = sel ? 0x5233BF : 0x2E1A64;
    int m=8;
    draw_rect(ctx,x+m,y+m,sz-2*m,sz-2*m,core);
    draw_hline(ctx,x+m,y+m,sz-2*m,edge);
    draw_hline(ctx,x+m,y+sz-m-1,sz-2*m,edge);
    draw_vline(ctx,x+m,y+m,sz-2*m,edge);
    draw_vline(ctx,x+sz-m-1,y+m,sz-2*m,edge);
    for(int k=12;k<sz-10;k+=8){
        draw_rect(ctx,x+k,y+3,2,6,edge); draw_rect(ctx,x+k,y+sz-9,2,6,edge);
        draw_rect(ctx,x+3,y+k,6,2,edge); draw_rect(ctx,x+sz-9,y+k,6,2,edge);
    }
    draw_rect(ctx,x+m+6,y+m+6,sz-2*m-12,sz-2*m-12,0x0A0E14);
    draw_rect(ctx,x+m+9,y+m+9,5,5,sel?0xFFFB96:0xF25E95);
}

/* Name badge: WHY palette ID card with avatar and two text strokes. */
static void icon_nametag(fbview_t *ctx, int x,int y,int sz,bool sel){
    int rx=x+4, ry=y+7, rw=sz-8, rh=sz-14;
    uint32_t body=sel?0x5233BF:0x2E1A64;
    draw_rect(ctx,rx,ry,rw,rh,body);
    draw_rect(ctx,rx,ry,rw,6,0x64EFFE);
    draw_circle_filled(ctx,rx+11,ry+17,6,sel?0xFFFB96:0xF25E95);
    draw_rect(ctx,rx+22,ry+13,rw-27,4,0xFFFFFF);
    draw_rect(ctx,rx+22,ry+21,rw-31,3,0xF25E95);
    draw_rect(ctx,rx+rw/3,ry+rh-5,rw/3,2,0x64EFFE);
}

/* Sponsors: luminous five-point star with cyan/pink centre. */
static void icon_star_bold(fbview_t *ctx, int x,int y,int sz,bool sel){
    int cx=x+sz/2, cy=y+sz/2, r=sz/3;
    int px[5]={cx,cx+r,cx+r/3,cx-r/3,cx-r};
    int py[5]={cy-r,cy-r/3,cy+r,cy+r,cy-r/3};
    uint32_t edge=sel?0xFFFB96:0xFFFFFF;
    draw_circle_filled(ctx,cx,cy,sz/2-4,0x2E1A64);
    for(int i=0;i<5;i++){
        int j=(i+2)%5;
        draw_thick_line(ctx,px[i],py[i],px[j],py[j],edge,3);
    }
    draw_circle_filled(ctx,cx,cy,5,sel?0xF25E95:0x64EFFE);
}

/* Curl/serial: compact USB-style plug with neon cable. */
static void icon_plug(fbview_t *ctx, int x,int y,int sz,bool sel){
    uint32_t body=sel?0xFFFFFF:0x64EFFE;
    uint32_t accent=sel?0xF25E95:0x8B5CFF;
    int bw=sz/2, bh=sz*2/5, bx=x+(sz-bw)/2, by=y+sz/3;
    draw_rect(ctx,bx,by,bw,bh,body);
    draw_rect(ctx,bx+5,by-7,5,8,body);
    draw_rect(ctx,bx+bw-10,by-7,5,8,body);
    draw_rect(ctx,bx+4,by+5,bw-8,4,accent);
    draw_thick_line(ctx,x+sz/2,by+bh-1,x+sz/2,y+sz-4,accent,4);
}

/* Hello: friendly face in the same neon palette. */
static void icon_hello(fbview_t *ctx, int x,int y,int sz,bool sel){
    uint32_t face=sel?0xFFFB96:0x64EFFE;
    int cx=x+sz/2,cy=y+sz/2,r=sz/3;
    draw_circle_outline(ctx,cx,cy,r,face,2);
    draw_circle_filled(ctx,cx-r/3,cy-r/4,2,0xFFFFFF);
    draw_circle_filled(ctx,cx+r/3,cy-r/4,2,0xFFFFFF);
    draw_thick_line(ctx,cx-r/3,cy+r/4,cx,cy+r/3,0xF25E95,2);
    draw_thick_line(ctx,cx,cy+r/3,cx+r/3,cy+r/4,0xF25E95,2);
}

/* UID->icon mapping */
static void draw_app_icon(fbview_t *ctx, const char *uid, int x, int y, int sz, bool selected) {
    if (!uid) uid = "";
    if (strstr(uid, "mini_browser"))      { icon_browser(ctx, x, y, sz, selected); return; }
    if (strstr(uid, "settings"))          { icon_settings(ctx,x, y, sz, selected); return; }
    if (strstr(uid, "why2025_namebadge")) { icon_nametag(ctx, x, y, sz, selected); return; }
    if (strstr(uid, "sponsors"))          { icon_star_bold(ctx,x,y,sz,selected); return; }
    if (strstr(uid, "wifi"))              { icon_wifi(ctx,x,y,sz,selected); return; }
    if (strstr(uid, "sdl_test"))          { icon_snake(ctx,x,y,sz,selected); return; }
    if (strstr(uid, "doom"))              { icon_chip(ctx,x,y,sz,selected); return; }
    if (strstr(uid, "curl"))              { icon_plug(ctx,x,y,sz,selected); return; }
    if (strstr(uid, "serial"))            { icon_plug(ctx,x,y,sz,selected); return; }
    if (strstr(uid, "hardware"))          { icon_chip(ctx,x,y,sz,selected); return; }
    if (strstr(uid, "hello"))             { icon_hello(ctx,x,y,sz,selected); return; }
    if (strstr(uid, "ota"))               { icon_ota(ctx,x,y,sz,selected); return; }
    icon_chip(ctx,x,y,sz,selected);
}

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
    bool            screenshot_pending;
    int             about_index;
} Launcher_Context;

/* ===========================================================
   UI drawing
   =========================================================== */
static void draw_title_bar(fbview_t *ctx, int x, int y, int w, int h, const char *title) {
    draw_rect(ctx, x, y, w, h, COL_TITLE_BG);
    draw_text_bold(ctx, x + 16, y + (h - FONT_HEIGHT)/2, title, COL_TITLE_TEXT);
}

static void draw_window(Launcher_Context *ctx) {
    fbview_t v = { ctx->pixels };
    const int wx = 24, wy = 24, ww = SCREEN_WIDTH - 48, wh = SCREEN_HEIGHT - 48;
    draw_rect(&v, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COL_BG);
    draw_card(&v, wx, wy, ww, wh);

    const int th = 48;
    draw_title_bar(&v, wx + 2, wy + 2, ww - 4, th, "WHY Launcher");

    char info[96];
    snprintf(info, sizeof(info), "To navigate   Enter: Launch   A: About   ESC: Exit");
    draw_text(&v, wx + 16, wy + th + 12, info, COL_TEXT_MID);

    /* Accent swatches to verify palette on-device ... not in use...
    int bx = wx + ww - 16 - 6*10, by = wy + th + 10;
    uint32_t acc[6] = { COL_ACCENT_1, COL_ACCENT_2, COL_ACCENT_3, COL_ACCENT_4, COL_ACCENT_5, COL_ACCENT_6 };
    for (int i=0;i<6;i++){ draw_rect(&v, bx + i*10, by, 8, 8, acc[i]); } */

    const int list_y = wy + th + 36;
    const int list_h = wh - th - 84;
    const int item_h = 78;
    const int list_x = wx + 12;
    const int list_w = ww - 24;

    draw_rect(&v, list_x, list_y, list_w, list_h, COL_PANEL);
    draw_rect(&v, list_x, list_y, list_w, 1, COL_DIVIDER);
    draw_rect(&v, list_x, list_y + list_h - 1, list_w, 1, COL_DIVIDER);
    draw_rect(&v, list_x, list_y, 1, list_h, COL_DIVIDER);
    draw_rect(&v, list_x + list_w - 1, list_y, 1, list_h, COL_DIVIDER);

    draw_rect(&v, wx + 2, wy + wh - 40, ww - 4, 38, COL_BTN);
    draw_text(&v, wx + 16, wy + wh - 32, "WHY2025 • BadgeVMS", COL_BTN_TEXT);

 /*   ctx->items_per_page = (list_h - 6) / item_h; */
/*    ...now 7...... */

    ctx->items_per_page = (list_h - 6) / item_h;
    if (ctx->items_per_page < 7) ctx->items_per_page = 7;
    
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

        draw_rect(&v, ix, iy, iw, item_h - 6, row_bg);
        draw_hline(&v, ix, iy + item_h - 6, iw, COL_DIVIDER);
        if (sel) draw_focus(&v, ix, iy, iw, item_h - 6);

        int icon_sz = 48;
        int icon_x  = ix + 12;
        int icon_y  = iy + (item_h - 6 - icon_sz)/2;
        draw_rect(&v, icon_x - 3, icon_y - 3, icon_sz + 6, icon_sz + 6, COL_FOCUS_BG);
        draw_app_icon(&v, ctx->applications[i]->unique_identifier, icon_x, icon_y, icon_sz, sel);

        const char *name = ctx->applications[i]->name ? ctx->applications[i]->name : "(unnamed)";
        draw_text_bold(&v, icon_x + icon_sz + 16, iy + 10, name, row_text);

        if (ctx->applications[i]->version) {
            char ver[32];
            snprintf(ver, sizeof(ver), "v%s", ctx->applications[i]->version);
            draw_text(&v, icon_x + icon_sz + 16, iy + 10 + 22, ver, COL_TEXT_MID);
        }
    }

    /* scrollbar */
    if (ctx->total_items > ctx->items_per_page) {
        int sbx = list_x + list_w - 20;
        int sby = list_y + 4;
        int sbh = list_h - 8;
        draw_rect(&v, sbx, sby, 12, sbh, COL_BTN);
        draw_rect(&v, sbx, sby, 12, 1, COL_DIVIDER);
        draw_rect(&v, sbx, sby + sbh - 1, 12, 1, COL_DIVIDER);

        int thumb_h = (sbh * ctx->items_per_page) / ctx->total_items;
        if (thumb_h < 24) thumb_h = 24;
        int thumb_y = sby + ((sbh - thumb_h) * ctx->scroll_offset) /
                      (ctx->total_items - ctx->items_per_page);
        draw_rect(&v, sbx + 2, thumb_y, 8, thumb_h, COL_ACCENT_3);
    }
}

/* ===========================================================
   About dialog
   =========================================================== */
static void draw_about(Launcher_Context *ctx, application_t *app) {
    fbview_t v = { ctx->pixels };
    int w = 520, h = 320;
    int x = (SCREEN_WIDTH  - w)/2;
    int y = (SCREEN_HEIGHT - h)/2;

    draw_rect(&v, x-6, y-6, w+12, h+12, COL_BG);
    draw_card(&v, x, y, w, h);
    draw_title_bar(&v, x + 2, y + 2, w - 4, 42, "About");

    const char *name = (app && app->name) ? app->name : "(unknown)";
    const char *ver  = (app && app->version && app->version[0]) ? app->version : "-";
    const char *uid  = (app && app->unique_identifier) ? app->unique_identifier : "-";
    const char *bin  = (app && app->binary_path && app->binary_path[0]) ? app->binary_path : "-";

    char line[256];
    draw_text_center(&v, x, y + 78,  w, name, COL_TEXT);
    snprintf(line, sizeof(line), "Version: %s", ver);
    draw_text_center(&v, x, y + 110, w, line, COL_TEXT_MID);
    snprintf(line, sizeof(line), "UID: %s", uid);
    draw_text_center(&v, x, y + 138, w, line, COL_TEXT_MID);
    snprintf(line, sizeof(line), "Binary: %s", bin);
    draw_text_center(&v, x, y + 166, w, line, COL_TEXT_MID);
    draw_text_center(&v, x, y + 220, w, "ENTER or ESC to close", COL_TEXT_MID);
}

/* ===========================================================
   Serial screenshot streaming — Mini Browser compatible
   =========================================================== */

/*
 * WHY+S requests a screenshot of the visible 720x720 launcher framebuffer.
 *
 * This deliberately uses the same RGB24/RLE5FEC1 serial wire protocol as
 * Mini Browser, so the existing badge_screenshot.py receiver can be reused.
 * The launcher already owns an RGB565 framebuffer, therefore no SDL readback,
 * PNG/JPEG decoder, second framebuffer, or large temporary allocation is
 * needed. Pixels are converted RGB565 -> RGB24 one run at a time.
 */
#define IMG_RAW_CHUNK       48
#define IMG_FEC_GROUP        4
#define SCREENSHOT_PACE_MS   8

typedef struct {
    unsigned char raw[IMG_RAW_CHUNK];
    size_t used;
    unsigned sequence;
    unsigned long compressed_bytes;
    unsigned crc;
    unsigned char parity[IMG_RAW_CHUNK];
    unsigned parity_count;
    unsigned parity_group;
} screenshot_stream_t;

static unsigned screenshot_crc32_update(unsigned crc, const unsigned char *data, size_t len) {
    while (len--) {
        crc ^= *data++;
        for (int bit = 0; bit < 8; bit++) {
            unsigned mask = (unsigned)-(int)(crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return crc;
}

static unsigned screenshot_crc32(const unsigned char *data, size_t len) {
    unsigned crc = 0xFFFFFFFFU;
    crc = screenshot_crc32_update(crc, data, len);
    return crc ^ 0xFFFFFFFFU;
}

static size_t screenshot_base64_encode(char *out, size_t out_size,
                                       const unsigned char *data, size_t len) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t needed = 4U * ((len + 2U) / 3U);
    if (!out || out_size < needed + 1U) return 0;

    size_t i = 0, o = 0;
    while (i + 3U <= len) {
        unsigned a = data[i++], b = data[i++], c = data[i++];
        out[o++] = table[(a >> 2) & 0x3F];
        out[o++] = table[((a & 3U) << 4) | ((b >> 4) & 0x0F)];
        out[o++] = table[((b & 0x0F) << 2) | ((c >> 6) & 3U)];
        out[o++] = table[c & 0x3F];
    }
    size_t rem = len - i;
    if (rem == 1U) {
        unsigned a = data[i];
        out[o++] = table[(a >> 2) & 0x3F];
        out[o++] = table[(a & 3U) << 4];
        out[o++] = '='; out[o++] = '=';
    } else if (rem == 2U) {
        unsigned a = data[i], b = data[i + 1U];
        out[o++] = table[(a >> 2) & 0x3F];
        out[o++] = table[((a & 3U) << 4) | ((b >> 4) & 0x0F)];
        out[o++] = table[(b & 0x0F) << 2];
        out[o++] = '=';
    }
    out[o] = '\0';
    return o;
}

static void screenshot_transport_pause(void) {
    /* badgevms_launcher is built as a VMS app against sdk_staging and does
       not have direct FreeRTOS headers/API available.  stdout is already
       flushed after every transport record; keep this dependency-free. */
}

static bool screenshot_send_parity(screenshot_stream_t *stream) {
    if (!stream || stream->parity_count == 0) return true;
    char encoded[(IMG_RAW_CHUNK * 4 / 3) + 8];
    if (!screenshot_base64_encode(encoded, sizeof(encoded), stream->parity, IMG_RAW_CHUNK)) {
        printf("IMG ERROR parity-base64-buffer\n"); fflush(stdout); return false;
    }
    unsigned crc = screenshot_crc32(stream->parity, IMG_RAW_CHUNK);
    printf("IMG P %06u %08X %s\n", stream->parity_group, crc, encoded);
    fflush(stdout); screenshot_transport_pause();
    memset(stream->parity, 0, sizeof(stream->parity));
    stream->parity_count = 0;
    stream->parity_group++;
    return true;
}

static bool screenshot_stream_flush(screenshot_stream_t *stream) {
    if (!stream || stream->used == 0) return true;
    char encoded[(IMG_RAW_CHUNK * 4 / 3) + 8];
    if (!screenshot_base64_encode(encoded, sizeof(encoded), stream->raw, stream->used)) {
        printf("IMG ERROR data-base64-buffer\n"); fflush(stdout); stream->used = 0; return false;
    }
    unsigned chunk_crc = screenshot_crc32(stream->raw, stream->used);
    printf("IMG D %06u %02u %08X %s\n", stream->sequence,
           (unsigned)stream->used, chunk_crc, encoded);
    fflush(stdout); screenshot_transport_pause();
    for (size_t i = 0; i < stream->used; i++) stream->parity[i] ^= stream->raw[i];
    stream->sequence++;
    stream->parity_count++;
    stream->used = 0;
    if (stream->parity_count == IMG_FEC_GROUP) return screenshot_send_parity(stream);
    return true;
}

static bool screenshot_stream_bytes(screenshot_stream_t *stream,
                                    const unsigned char *data, size_t len) {
    if (!stream || !data) return false;
    stream->crc = screenshot_crc32_update(stream->crc, data, len);
    stream->compressed_bytes += (unsigned long)len;
    while (len > 0) {
        size_t room = IMG_RAW_CHUNK - stream->used;
        size_t take = len < room ? len : room;
        memcpy(stream->raw + stream->used, data, take);
        stream->used += take; data += take; len -= take;
        if (stream->used == IMG_RAW_CHUNK && !screenshot_stream_flush(stream)) return false;
    }
    return true;
}

static bool screenshot_emit_run(screenshot_stream_t *stream, unsigned count,
                                unsigned char r, unsigned char g, unsigned char b) {
    unsigned char record[5] = {
        (unsigned char)(count & 0xFFU),
        (unsigned char)((count >> 8) & 0xFFU), r, g, b
    };
    return screenshot_stream_bytes(stream, record, sizeof(record));
}

static bool screenshot_stream_framebuffer(const uint16_t *pixels) {
    if (!pixels) {
        printf("IMG ERROR no-framebuffer\n"); fflush(stdout); return false;
    }

    screenshot_stream_t stream;
    memset(&stream, 0, sizeof(stream));
    stream.crc = 0xFFFFFFFFU;
    printf("IMG BEGIN %d %d RGB24 RLE5FEC1 %d %d\n",
           SCREEN_WIDTH, SCREEN_HEIGHT, IMG_RAW_CHUNK, IMG_FEC_GROUP);
    fflush(stdout); screenshot_transport_pause();

    bool have_run = false;
    unsigned run_count = 0;
    unsigned char run_r = 0, run_g = 0, run_b = 0;
    const size_t count = (size_t)SCREEN_WIDTH * (size_t)SCREEN_HEIGHT;

    for (size_t i = 0; i < count; i++) {
        uint16_t p = pixels[i];
        unsigned r5 = (p >> 11) & 0x1FU;
        unsigned g6 = (p >> 5)  & 0x3FU;
        unsigned b5 = p & 0x1FU;
        unsigned char r = (unsigned char)((r5 << 3) | (r5 >> 2));
        unsigned char g = (unsigned char)((g6 << 2) | (g6 >> 4));
        unsigned char b = (unsigned char)((b5 << 3) | (b5 >> 2));

        if (have_run && r == run_r && g == run_g && b == run_b && run_count < 65535U) {
            run_count++;
            continue;
        }
        if (have_run && !screenshot_emit_run(&stream, run_count, run_r, run_g, run_b)) return false;
        have_run = true; run_count = 1; run_r = r; run_g = g; run_b = b;
    }
    if (have_run && !screenshot_emit_run(&stream, run_count, run_r, run_g, run_b)) return false;
    if (!screenshot_stream_flush(&stream)) return false;
    if (!screenshot_send_parity(&stream)) return false;

    unsigned final_crc = stream.crc ^ 0xFFFFFFFFU;
    for (int repeat = 0; repeat < 2; repeat++) {
        printf("IMG END %lu %08X %u\n", stream.compressed_bytes, final_crc, stream.sequence);
        fflush(stdout); screenshot_transport_pause();
    }
    return true;
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
                if (ctx->selected_item < ctx->scroll_offset)
                    ctx->scroll_offset = ctx->selected_item;
            }
            break;

        case KEY_SCANCODE_DOWN:
            if (ctx->selected_item < ctx->total_items - 1) {
                ctx->selected_item++;
                if (ctx->selected_item >= ctx->scroll_offset + ctx->items_per_page)
                    ctx->scroll_offset = ctx->selected_item - ctx->items_per_page + 1;
            }
            break;

        case KEY_SCANCODE_RETURN:
        case KEY_SCANCODE_SPACE:
            printf("Launching: %s\n", ctx->applications[ctx->selected_item]->name);
            application_launch(ctx->applications[ctx->selected_item]->unique_identifier);
            break;

        case KEY_SCANCODE_A:
            ctx->about_index = ctx->selected_item;
            ctx->show_about  = true;
            break;

        case KEY_SCANCODE_S:
            /* WHY+S on the badge arrives here as the S scancode. */
            ctx->screenshot_pending = true;
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


static void move_uid_to_front(application_t **apps, size_t num, const char *uid) {
    if (!apps || num == 0 || !uid) return;
    size_t idx = (size_t)-1;
    for (size_t i = 0; i < num; ++i) {
        if (apps[i] && apps[i]->unique_identifier &&
            strcmp(apps[i]->unique_identifier, uid) == 0) {
            idx = i;
            break;
        }
    }
    if (idx == (size_t)-1 || idx == 0) return;        // not found or already first
    application_t *hit = apps[idx];
    memmove(&apps[1], &apps[0], idx * sizeof(*apps)); // shift block right by 1
    apps[0] = hit;
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

        if (ctx.screenshot_pending) {
            ctx.screenshot_pending = false;
            printf("[launcher] screenshot: 720x720 visible framebuffer\n");
            fflush(stdout);
            screenshot_stream_framebuffer(ctx.pixels);
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

        if (app->binary_path && app->binary_path[0] &&
            app->unique_identifier &&
            strcmp(app->unique_identifier, "badgevms_launcher") != 0 &&
            strcmp(app->unique_identifier, "why2025_firmware_ota_c6") != 0) {

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

    /* >>> Add this line to force Mini Browser to position #1 */
    // after building apps[]
    /* move_uid_to_front(apps, num, "third_app");
    move_uid_to_front(apps, num, "second_app");
    move_uid_to_front(apps, num, "mini_browser"); // ends up first */
    /* move mini_browser to top */

    move_uid_to_front(apps, num, "mini_browser");

    for (size_t i = 0; i < num; ++i) {
        printf("FINAL_ORDER[%zu]: %s (%s)\n", i, apps[i]->name, apps[i]->unique_identifier);
    }   

    bool ok = run_launcher(apps, num);

    free(prio);
    free(rest);
    free(apps);

    return ok ? 0 : 1;
}
