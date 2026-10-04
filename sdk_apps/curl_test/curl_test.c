/*
 * curl_test - the BadgeVMS libcurl examples, now with on-screen results.
 *
 * Runs the same ten example requests as before (GET, POST, memory capture,
 * basic auth, PUT, connection reuse, cookie jar, cookie persistence, proxy
 * stubs and cookie header visibility) and shows them in the WHY2025 launcher
 * style: a list of tests with PASS/FAIL, and a detail panel with the log of
 * the selected test, including the first part of each response body.
 *
 * Everything is still printed on the serial console as well.
 *
 * Keys: Up/Down select a test, Left/Right scroll its log,
 *       R run all tests again, Esc stop / quit.
 */
#include "badgevms/wifi.h"
#include "curl/curl.h"
#include "font.h"

#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN_WIDTH  720
#define SCREEN_HEIGHT 720

/* ===========================================================
   Color theme (RGB888) - WHY2025 palette, same as the launcher
   =========================================================== */
#define COL_BG                0x0A0E14
#define COL_PANEL             0x111826
#define COL_ACCENT_3          0xF25E95 /* pink */
#define COL_ACCENT_6          0xF24436 /* red */
#define COL_TITLE_BG          0x0F1220
#define COL_TITLE_TEXT        0xFFFFFF
#define COL_TEXT              0xE5E7EB
#define COL_TEXT_MID          0x9CA3AF
#define COL_DIVIDER           0x2A3446
#define COL_BTN               0x1C2433
#define COL_BTN_TEXT          0xE5E7EB
#define COL_FOCUS_OUT         0x64EFFE /* cyan */
#define COL_ROW_SELECTED_BG   0x18283C
#define COL_SUCCESS           0x34D399

#define WIN_X    24
#define WIN_Y    24
#define WIN_W    (SCREEN_WIDTH - 48)
#define WIN_H    (SCREEN_HEIGHT - 48)
#define TITLE_H  48
#define FOOTER_H 38

#define UA "BadgeVMS-libcurl/1.0"

/* ===========================================================
   Test bookkeeping
   =========================================================== */
#define NUM_TESTS      10
#define LOG_LINES_MAX  160
#define LOG_COLS       52 /* characters per detail line */
#define BODY_SHOW_MAX  480 /* bytes of each response body shown */

typedef enum { ST_PENDING, ST_RUNNING, ST_PASS, ST_FAIL, ST_SKIPPED } test_state_t;

typedef struct {
    char const  *name;
    bool (*run)(void);
    test_state_t state;
    char         summary[48];
    uint32_t     ms;
    char        *lines[LOG_LINES_MAX];
    int          line_count;
} test_t;

static test_t   *g_tests;
static int       g_current = -1; /* test that receives log lines */
static char      g_partial[LOG_COLS + 1];
static int       g_partial_len;

typedef struct {
    SDL_Window   *window;
    SDL_Renderer *renderer;
    SDL_Texture  *texture;
    uint16_t     *pixels;
    int           selected;
    int           log_scroll;
    bool          running;
    bool          abort;
    bool          quit;
    char          wifi_text[64];
    uint32_t      wifi_color;
} app_t;

static app_t g_app;

static void render(void);

/* ===========================================================
   Logging: serial + per-test line buffer (wrapped at LOG_COLS)
   =========================================================== */
static void log_flush_line(void) {
    if (g_current < 0)
        return;
    test_t *t = &g_tests[g_current];
    g_partial[g_partial_len] = '\0';
    if (t->line_count < LOG_LINES_MAX) {
        t->lines[t->line_count] = strdup(g_partial);
        if (t->lines[t->line_count])
            t->line_count++;
    }
    g_partial_len = 0;
}

static void log_bytes(char const *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\r')
            continue;
        if (c == '\n') {
            log_flush_line();
            continue;
        }
        if (c == '\t')
            c = ' ';
        if ((unsigned char)c < 32 || (unsigned char)c > 126)
            c = '?';
        if (g_partial_len >= LOG_COLS)
            log_flush_line();
        g_partial[g_partial_len++] = c;
    }
}

static void tlog(char const *fmt, ...) {
    char    buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("%s", buf);
    log_bytes(buf, strlen(buf));
}

/* Response body collector: keeps the whole body for checks, shows the
   first BODY_SHOW_MAX bytes on screen. */
typedef struct {
    char  *data;
    size_t size;
} body_t;

static size_t body_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t  realsize = size * nmemb;
    body_t *b        = userp;
    char   *p        = realloc(b->data, b->size + realsize + 1);
    if (!p)
        return 0;
    b->data = p;
    memcpy(b->data + b->size, contents, realsize);
    b->size           += realsize;
    b->data[b->size]   = '\0';
    return realsize;
}

static void body_reset(body_t *b) {
    free(b->data);
    b->data = NULL;
    b->size = 0;
}

static void body_show(body_t const *b) {
    if (!b->size) {
        tlog("(empty body)\n");
        return;
    }
    size_t n = b->size < BODY_SHOW_MAX ? b->size : BODY_SHOW_MAX;
    printf("%.*s\n", (int)n, b->data);
    log_bytes(b->data, n);
    log_flush_line();
    if (b->size > n)
        tlog("... (%u bytes total)\n", (unsigned)b->size);
}

static bool body_has(body_t const *b, char const *needle) {
    return b->data && strstr(b->data, needle);
}

/* Perform + report. Returns the HTTP code, or -1 on a curl error. */
static long perform(CURL *curl, char const *what) {
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        tlog("%s failed: %s\n", what, curl_easy_strerror(res));
        return -1;
    }
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    tlog("%s: HTTP %ld\n", what, code);
    return code;
}

static void set_summary(char const *fmt, ...) {
    if (g_current < 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_tests[g_current].summary, sizeof(g_tests[g_current].summary), fmt, ap);
    va_end(ap);
}

/* ===========================================================
   The tests (same requests as the original examples)
   =========================================================== */
static bool t_simple_get(void) {
    body_t b    = {0};
    CURL  *curl = curl_easy_init();
    if (!curl)
        return false;
    curl_easy_setopt(curl, CURLOPT_URL, "https://httpbin.org/get");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, UA);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, body_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    tlog("GET https://httpbin.org/get\n");
    long code = perform(curl, "GET");
    body_show(&b);
    bool ok = code == 200 && body_has(&b, UA);
    set_summary("HTTP %ld, %u bytes", code, (unsigned)b.size);
    body_reset(&b);
    curl_easy_cleanup(curl);
    return ok;
}

static bool t_post_json(void) {
    body_t             b       = {0};
    struct curl_slist *headers = NULL;
    CURL              *curl    = curl_easy_init();
    if (!curl)
        return false;
    curl_easy_setopt(curl, CURLOPT_URL, "https://httpbin.org/post");
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "{\"name\":\"BadgeVMS\",\"value\":42}");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, body_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    tlog("POST {\"name\":\"BadgeVMS\",\"value\":42}\n");
    long code = perform(curl, "POST");
    body_show(&b);
    bool ok = code == 200 && body_has(&b, "BadgeVMS");
    set_summary("HTTP %ld, echo %s", code, ok ? "ok" : "missing");
    body_reset(&b);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ok;
}

static bool t_capture(void) {
    body_t b    = {0};
    CURL  *curl = curl_easy_init();
    if (!curl)
        return false;
    curl_easy_setopt(curl, CURLOPT_URL, "https://api.github.com/users/espressif");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, body_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, UA);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 128);
    tlog("GET https://api.github.com/users/espressif\n");
    long code = perform(curl, "GET");
    body_show(&b);
    bool ok = code == 200 && b.size > 0;
    set_summary("HTTP %ld, %u bytes", code, (unsigned)b.size);
    body_reset(&b);
    curl_easy_cleanup(curl);
    return ok;
}

static bool t_auth(void) {
    body_t b    = {0};
    CURL  *curl = curl_easy_init();
    if (!curl)
        return false;
    curl_easy_setopt(curl, CURLOPT_URL, "https://httpbin.org/basic-auth/user/pass");
    curl_easy_setopt(curl, CURLOPT_USERPWD, "user:pass");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, body_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    tlog("GET basic-auth user:pass\n");
    long code = perform(curl, "Auth");
    body_show(&b);
    bool ok = code == 200;
    set_summary("HTTP %ld", code);
    body_reset(&b);
    curl_easy_cleanup(curl);
    return ok;
}

static bool t_put(void) {
    body_t             b       = {0};
    struct curl_slist *headers = NULL;
    CURL              *curl    = curl_easy_init();
    if (!curl)
        return false;
    curl_easy_setopt(curl, CURLOPT_URL, "https://httpbin.org/put");
    headers = curl_slist_append(headers, "Content-Type: text/plain");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "This is test data from BadgeVMS");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, body_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    tlog("PUT \"This is test data from BadgeVMS\"\n");
    long code = perform(curl, "PUT");
    body_show(&b);
    bool ok = code == 200 && body_has(&b, "test data from BadgeVMS");
    set_summary("HTTP %ld, echo %s", code, ok ? "ok" : "missing");
    body_reset(&b);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ok;
}

static bool t_multiple(void) {
    static char const *urls[] = {
        "https://httpbin.org/get?page=1",
        "https://httpbin.org/get?page=2",
        "https://httpbin.org/get?page=3"
    };
    body_t b    = {0};
    int    good = 0;
    CURL  *curl = curl_easy_init();
    if (!curl)
        return false;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, body_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    for (int i = 0; i < 3; i++) {
        tlog("--- Request %d: %s\n", i + 1, urls[i]);
        curl_easy_setopt(curl, CURLOPT_URL, urls[i]);
        long code = perform(curl, "GET");
        char want[16];
        snprintf(want, sizeof(want), "page=%d", i + 1);
        if (code == 200 && body_has(&b, want))
            good++;
        tlog("%u bytes, %s %s\n", (unsigned)b.size, want, body_has(&b, want) ? "echoed" : "MISSING");
        body_reset(&b);
    }
    set_summary("%d/3 requests ok", good);
    curl_easy_cleanup(curl);
    return good == 3;
}

static bool t_cookie_jar(void) {
    char const *file = "FLASH0:test_cookies.txt";
    body_t      b    = {0};
    bool        ok1 = false, ok2 = false, ok3 = false;

    tlog("1) Manual cookies\n");
    CURL *curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, "https://httpbin.org/cookies");
        curl_easy_setopt(curl, CURLOPT_COOKIE, "manual_cookie=test_value; session_id=abc123");
        curl_easy_setopt(curl, CURLOPT_USERAGENT, UA);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, body_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
        ok1 = perform(curl, "Manual") == 200 && body_has(&b, "abc123");
        body_show(&b);
        body_reset(&b);
        curl_easy_cleanup(curl);
    }

    tlog("2) COOKIEJAR -> %s\n", file);
    curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, "https://httpbin.org/cookies/set/jar_test/saved_value");
        curl_easy_setopt(curl, CURLOPT_USERAGENT, UA);
        curl_easy_setopt(curl, CURLOPT_COOKIEJAR, file);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, body_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
        perform(curl, "Set");
        body_reset(&b);
        curl_easy_cleanup(curl); /* the jar is written on cleanup */
        FILE *f = fopen(file, "r");
        if (f) {
            char line[256];
            int  n = 0;
            tlog("Cookie file:\n");
            while (fgets(line, sizeof(line), f) && n < 5) {
                tlog("  %s", line);
                if (!strchr(line, '\n'))
                    tlog("\n");
                n++;
            }
            fclose(f);
            ok2 = true;
        } else {
            tlog("Cookie file was not created\n");
        }
    }

    tlog("3) COOKIEFILE <- %s\n", file);
    curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, "https://httpbin.org/cookies");
        curl_easy_setopt(curl, CURLOPT_USERAGENT, UA);
        curl_easy_setopt(curl, CURLOPT_COOKIEFILE, file);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, body_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
        ok3 = perform(curl, "Load") == 200 && body_has(&b, "jar_test");
        body_show(&b);
        body_reset(&b);
        curl_easy_cleanup(curl);
    }

    if (remove(file) == 0)
        tlog("Test cookie file cleaned up\n");
    set_summary("manual %s, jar %s, load %s", ok1 ? "ok" : "X", ok2 ? "ok" : "X", ok3 ? "ok" : "X");
    return ok1 && ok2 && ok3;
}

static bool t_persistence(void) {
    body_t b    = {0};
    bool   ok   = false;
    CURL  *curl = curl_easy_init();
    if (!curl)
        return false;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, body_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, UA);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    tlog("1) Set persistent_cookie\n");
    curl_easy_setopt(curl, CURLOPT_URL, "https://httpbin.org/cookies/set/persistent_cookie/esp32_value");
    perform(curl, "Set");
    body_reset(&b);

    tlog("2) Same handle: GET /cookies\n");
    curl_easy_setopt(curl, CURLOPT_URL, "https://httpbin.org/cookies");
    long code = perform(curl, "Check");
    body_show(&b);
    ok = code == 200 && body_has(&b, "esp32_value");
    set_summary("cookie %s", ok ? "sent back" : "NOT sent back");
    body_reset(&b);
    curl_easy_cleanup(curl);
    return ok;
}

static bool t_proxy_stubs(void) {
    int   good = 0, total = 0;
    CURL *curl = curl_easy_init();
    if (!curl)
        return false;

#define EXPECT_ERR(opt, val, label)                                                       \
    do {                                                                                  \
        CURLcode r = curl_easy_setopt(curl, opt, val);                                    \
        total++;                                                                          \
        if (r != CURLE_OK) good++;                                                        \
        tlog("%-16s %s\n", label, r != CURLE_OK ? curl_easy_strerror(r) : "ACCEPTED (?)"); \
    } while (0)

    EXPECT_ERR(CURLOPT_PROXY, "http://proxy.example.com:8080", "PROXY:");
    EXPECT_ERR(CURLOPT_PROXYUSERPWD, "user:pass", "PROXYUSERPWD:");
    EXPECT_ERR(CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5, "PROXYTYPE:");
    EXPECT_ERR(CURLOPT_PROXYPORT, 1080L, "PROXYPORT:");
    EXPECT_ERR(CURLOPT_PROXYAUTH, CURLAUTH_DIGEST, "PROXYAUTH:");
    EXPECT_ERR(CURLOPT_RANGE, "0-1023", "RANGE:");
    EXPECT_ERR(CURLOPT_REFERER, "https://example.com", "REFERER:");
#undef EXPECT_ERR

    CURLcode r = curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    total++;
    if (r == CURLE_OK)
        good++;
    tlog("%-16s %s\n", "HTTPAUTH:", r == CURLE_OK ? "ok (supported)" : curl_easy_strerror(r));

    set_summary("%d/%d options as expected", good, total);
    curl_easy_cleanup(curl);
    return good == total;
}

static bool g_saw_set_cookie;

static size_t header_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t      realsize = size * nmemb;
    char const *h        = contents;
    (void)userp;
    if (realsize >= 11 && strncasecmp(h, "Set-Cookie:", 11) == 0) {
        g_saw_set_cookie = true;
        tlog("Server set cookie: %.*s\n", (int)realsize, h);
    }
    return realsize;
}

static bool t_cookie_headers(void) {
    body_t b    = {0};
    CURL  *curl = curl_easy_init();
    if (!curl)
        return false;
    g_saw_set_cookie = false;
    curl_easy_setopt(curl, CURLOPT_URL, "https://httpbin.org/cookies/set/test_visibility/working");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, UA);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, body_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);

    tlog("1) Expect a Set-Cookie header\n");
    long c1 = perform(curl, "Set");
    body_reset(&b);

    tlog("2) GET /cookies, no redirects\n");
    curl_easy_setopt(curl, CURLOPT_URL, "https://httpbin.org/cookies");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    long c2 = perform(curl, "Check");
    body_show(&b);
    bool sent = body_has(&b, "test_visibility");
    tlog("Set-Cookie seen: %s, cookie sent back: %s\n", g_saw_set_cookie ? "yes" : "no", sent ? "yes" : "no");
    set_summary("Set-Cookie %s, sent %s", g_saw_set_cookie ? "seen" : "missing", sent ? "yes" : "no");
    body_reset(&b);
    curl_easy_cleanup(curl);
    return c1 > 0 && c2 == 200 && g_saw_set_cookie;
}

static test_t g_test_table[NUM_TESTS] = {
    {.name = "Simple GET", .run = t_simple_get},
    {.name = "POST JSON", .run = t_post_json},
    {.name = "Capture in memory", .run = t_capture},
    {.name = "HTTPS basic auth", .run = t_auth},
    {.name = "PUT upload", .run = t_put},
    {.name = "Connection reuse", .run = t_multiple},
    {.name = "Cookie jar file", .run = t_cookie_jar},
    {.name = "Cookie persistence", .run = t_persistence},
    {.name = "Proxy/option stubs", .run = t_proxy_stubs},
    {.name = "Cookie headers", .run = t_cookie_headers},
};

/* ===========================================================
   Drawing (framebuffer helpers, same as System Settings)
   =========================================================== */
static inline uint16_t rgb565(uint32_t c) {
    return (((c >> 16) & 0xF8) << 8) | (((c >> 8) & 0xFC) << 3) | ((c & 0xFF) >> 3);
}

static void draw_rect(int x, int y, int w, int h, uint32_t color) {
    uint16_t c  = rgb565(color);
    int      x2 = x + w, y2 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x2 > SCREEN_WIDTH) x2 = SCREEN_WIDTH;
    if (y2 > SCREEN_HEIGHT) y2 = SCREEN_HEIGHT;
    for (int py = y; py < y2; py++) {
        uint16_t *row = &g_app.pixels[py * SCREEN_WIDTH];
        for (int px = x; px < x2; px++)
            row[px] = c;
    }
}

static void draw_outline(int x, int y, int w, int h, uint32_t color) {
    draw_rect(x, y, w, 1, color);
    draw_rect(x, y + h - 1, w, 1, color);
    draw_rect(x, y, 1, h, color);
    draw_rect(x + w - 1, y, 1, h, color);
}

static void draw_char(int x, int y, char ch, uint32_t color) {
    if (ch < FONT_FIRST_CHAR || ch > FONT_LAST_CHAR)
        return;
    uint16_t const *rows = pixel_font[ch - FONT_FIRST_CHAR];
    uint16_t        c    = rgb565(color);
    for (int r = 0; r < FONT_HEIGHT; r++) {
        int py = y + r;
        if ((unsigned)py >= SCREEN_HEIGHT)
            continue;
        for (int col = 0; col < FONT_WIDTH; col++) {
            if (rows[r] & (0x800 >> col)) {
                int px = x + col;
                if ((unsigned)px < SCREEN_WIDTH)
                    g_app.pixels[py * SCREEN_WIDTH + px] = c;
            }
        }
    }
}

static void draw_text(int x, int y, char const *s, uint32_t color) {
    for (; *s; s++, x += FONT_WIDTH)
        draw_char(x, y, *s, color);
}

static void draw_text_bold(int x, int y, char const *s, uint32_t color) {
    draw_text(x, y, s, color);
    draw_text(x + 1, y, s, color);
}

static int text_w(char const *s) {
    return (int)strlen(s) * FONT_WIDTH;
}

static void draw_text_clipped(int x, int y, int max_w, char const *s, uint32_t color) {
    char buf[96];
    int  max_chars = max_w / FONT_WIDTH;
    if (max_chars < 0) max_chars = 0;
    if (max_chars > (int)sizeof(buf) - 1) max_chars = (int)sizeof(buf) - 1;
    snprintf(buf, sizeof(buf), "%s", s);
    if ((int)strlen(buf) > max_chars) {
        buf[max_chars] = '\0';
        if (max_chars >= 2) buf[max_chars - 1] = buf[max_chars - 2] = '.';
    }
    draw_text(x, y, buf, color);
}

/* Small rounded-ish status pill */
static void draw_pill(int x, int y, char const *label, uint32_t bg, uint32_t fg) {
    int w = text_w(label) + 12;
    draw_rect(x, y, w, FONT_HEIGHT + 2, bg);
    draw_text_bold(x + 6, y + 1, label, fg);
}

static int passed_count(void) {
    int n = 0;
    for (int i = 0; i < NUM_TESTS; i++)
        if (g_tests[i].state == ST_PASS) n++;
    return n;
}

static int done_count(void) {
    int n = 0;
    for (int i = 0; i < NUM_TESTS; i++)
        if (g_tests[i].state == ST_PASS || g_tests[i].state == ST_FAIL) n++;
    return n;
}

#define ROW_H        30
#define LIST_Y       (WIN_Y + TITLE_H + 40)
#define LIST_H       (NUM_TESTS * ROW_H + 8)
#define DETAIL_Y     (LIST_Y + LIST_H + 10)
#define DETAIL_H     (WIN_Y + WIN_H - FOOTER_H - 8 - DETAIL_Y)
#define DETAIL_ROWS  ((DETAIL_H - 40) / FONT_HEIGHT)

static void render(void) {
    app_t *a = &g_app;
    draw_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COL_BG);
    draw_rect(WIN_X, WIN_Y, WIN_W, WIN_H, COL_PANEL);
    draw_outline(WIN_X, WIN_Y, WIN_W, WIN_H, COL_DIVIDER);

    /* Title bar with the WiFi state on the right */
    draw_rect(WIN_X + 2, WIN_Y + 2, WIN_W - 4, TITLE_H, COL_TITLE_BG);
    draw_text_bold(WIN_X + 16, WIN_Y + 2 + (TITLE_H - FONT_HEIGHT) / 2, "curl test", COL_TITLE_TEXT);
    draw_text(WIN_X + WIN_W - 16 - text_w(a->wifi_text), WIN_Y + 2 + (TITLE_H - FONT_HEIGHT) / 2, a->wifi_text,
              a->wifi_color);

    draw_text(WIN_X + 16, WIN_Y + TITLE_H + 10,
              a->running ? "Running...  Esc: stop after this test" : "Up/Down: Test  Left/Right: Scroll  R: Rerun",
              COL_TEXT_MID);

    /* Test list */
    int lx = WIN_X + 12, lw = WIN_W - 24;
    draw_rect(lx, LIST_Y, lw, LIST_H, COL_PANEL);
    draw_outline(lx, LIST_Y, lw, LIST_H, COL_DIVIDER);
    for (int i = 0; i < NUM_TESTS; i++) {
        test_t *t   = &g_tests[i];
        int     y   = LIST_Y + 4 + i * ROW_H;
        bool    sel = i == a->selected;
        if (sel) {
            draw_rect(lx + 4, y, lw - 8, ROW_H - 2, COL_ROW_SELECTED_BG);
            draw_outline(lx + 3, y - 1, lw - 6, ROW_H, COL_FOCUS_OUT);
        }
        char num[8];
        snprintf(num, sizeof(num), "%2d", i + 1);
        draw_text(lx + 10, y + 2, num, COL_TEXT_MID);
        draw_text(lx + 46, y + 2, t->name, sel ? 0xFFFFFF : COL_TEXT);

        int sx = lx + 46 + 19 * FONT_WIDTH + 8;
        switch (t->state) {
            case ST_PASS: draw_pill(sx, y + 1, "PASS", 0x0F3D2E, COL_SUCCESS); break;
            case ST_FAIL: draw_pill(sx, y + 1, "FAIL", 0x3D1414, COL_ACCENT_6); break;
            case ST_RUNNING: draw_pill(sx, y + 1, "RUN ", 0x0E3440, COL_FOCUS_OUT); break;
            case ST_SKIPPED: draw_pill(sx, y + 1, "SKIP", COL_BTN, COL_TEXT_MID); break;
            default: draw_pill(sx, y + 1, " .. ", COL_BTN, COL_TEXT_MID); break;
        }
        if (t->summary[0]) {
            int dx = sx + 6 * FONT_WIDTH + 8;
            draw_text_clipped(dx, y + 2, lx + lw - 10 - dx, t->summary, COL_TEXT_MID);
        }
    }

    /* Detail panel: log of the selected test */
    test_t *t = &g_tests[a->selected];
    draw_rect(lx, DETAIL_Y, lw, DETAIL_H, 0x0C121C);
    draw_outline(lx, DETAIL_Y, lw, DETAIL_H, COL_DIVIDER);
    char head[96];
    if (t->state == ST_PASS || t->state == ST_FAIL)
        snprintf(head, sizeof(head), "%d. %s  (%u ms)", a->selected + 1, t->name, (unsigned)t->ms);
    else
        snprintf(head, sizeof(head), "%d. %s", a->selected + 1, t->name);
    draw_text_bold(lx + 10, DETAIL_Y + 6, head, COL_ACCENT_3);
    draw_rect(lx + 10, DETAIL_Y + 32, lw - 20, 1, COL_DIVIDER);

    int max_scroll = t->line_count - DETAIL_ROWS;
    if (max_scroll < 0) max_scroll = 0;
    if (a->log_scroll > max_scroll) a->log_scroll = max_scroll;
    if (a->log_scroll < 0) a->log_scroll = 0;
    if (!t->line_count) {
        draw_text(lx + 10, DETAIL_Y + 38, t->state == ST_RUNNING ? "Waiting for the server..." : "No output yet",
                  COL_TEXT_MID);
    }
    for (int r = 0; r < DETAIL_ROWS && a->log_scroll + r < t->line_count; r++) {
        char const *line  = t->lines[a->log_scroll + r];
        uint32_t    color = COL_TEXT;
        if (strstr(line, "failed") || strstr(line, "MISSING") || strstr(line, "NOT "))
            color = COL_ACCENT_6;
        else if (strstr(line, "HTTP 2"))
            color = COL_SUCCESS;
        else if (line[0] == '{' || line[0] == ' ' || line[0] == '"' || line[0] == '}')
            color = COL_TEXT_MID;
        draw_text(lx + 10, DETAIL_Y + 38 + r * FONT_HEIGHT, line, color);
    }
    if (t->line_count > DETAIL_ROWS) {
        /* scrollbar */
        int sbh = DETAIL_H - 44, sby = DETAIL_Y + 38, sbx = lx + lw - 14;
        draw_rect(sbx, sby, 6, sbh, COL_BTN);
        int th = sbh * DETAIL_ROWS / t->line_count;
        if (th < 16) th = 16;
        int ty = sby + (sbh - th) * a->log_scroll / (max_scroll ? max_scroll : 1);
        draw_rect(sbx, ty, 6, th, COL_ACCENT_3);
    }

    /* Footer */
    int fy = WIN_Y + WIN_H - FOOTER_H - 2;
    draw_rect(WIN_X + 2, fy, WIN_W - 4, FOOTER_H, COL_BTN);
    draw_text(WIN_X + 16, fy + (FOOTER_H - FONT_HEIGHT) / 2, "WHY2025 - BadgeVMS", COL_BTN_TEXT);
    char score[48];
    uint32_t score_col;
    if (a->running) {
        snprintf(score, sizeof(score), "%d/%d done", done_count(), NUM_TESTS);
        score_col = COL_FOCUS_OUT;
    } else {
        snprintf(score, sizeof(score), "%d/%d passed", passed_count(), NUM_TESTS);
        score_col = passed_count() == NUM_TESTS ? COL_SUCCESS : COL_ACCENT_3;
    }
    draw_text_bold(WIN_X + WIN_W - 16 - text_w(score), fy + (FOOTER_H - FONT_HEIGHT) / 2, score, score_col);

    SDL_UpdateTexture(a->texture, NULL, a->pixels, SCREEN_WIDTH * sizeof(uint16_t));
    SDL_RenderClear(a->renderer);
    SDL_RenderTexture(a->renderer, a->texture, NULL, NULL);
    SDL_RenderPresent(a->renderer);
}

/* ===========================================================
   Input
   =========================================================== */
static void handle_key(SDL_Keycode key) {
    app_t *a = &g_app;
    if (key == SDLK_ESCAPE) {
        if (a->running) a->abort = true;
        else a->quit = true;
    } else if (key == SDLK_UP) {
        if (a->selected > 0) { a->selected--; a->log_scroll = 0; }
    } else if (key == SDLK_DOWN) {
        if (a->selected < NUM_TESTS - 1) { a->selected++; a->log_scroll = 0; }
    } else if (key == SDLK_LEFT || key == SDLK_PAGEUP) {
        a->log_scroll -= DETAIL_ROWS - 1;
    } else if (key == SDLK_RIGHT || key == SDLK_PAGEDOWN) {
        a->log_scroll += DETAIL_ROWS - 1;
    }
}

/* Called between requests while the tests run. */
static void pump_events(void) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_QUIT) {
            g_app.abort = true;
            g_app.quit  = true;
        } else if (ev.type == SDL_EVENT_KEY_DOWN) {
            handle_key(ev.key.key);
        }
    }
}

/* ===========================================================
   Test runner
   =========================================================== */
static void clear_results(void) {
    for (int i = 0; i < NUM_TESTS; i++) {
        test_t *t = &g_tests[i];
        for (int l = 0; l < t->line_count; l++)
            free(t->lines[l]);
        t->line_count = 0;
        t->state      = ST_PENDING;
        t->summary[0] = '\0';
        t->ms         = 0;
    }
}

static void run_all(void) {
    app_t *a = &g_app;
    clear_results();
    a->running    = true;
    a->abort      = false;
    a->log_scroll = 0;

    printf("=== ESP-CURL Examples ===\n");
    snprintf(a->wifi_text, sizeof(a->wifi_text), "WiFi: connecting...");
    a->wifi_color = COL_FOCUS_OUT;
    render();

    wifi_connection_status_t ws = wifi_connect();
    if (ws != WIFI_CONNECTED) {
        snprintf(a->wifi_text, sizeof(a->wifi_text), "WiFi: not connected");
        a->wifi_color = COL_ACCENT_6;
        printf("WiFi not connected (status %d)\n", (int)ws);
        for (int i = 0; i < NUM_TESTS; i++) {
            g_tests[i].state = ST_SKIPPED;
            snprintf(g_tests[i].summary, sizeof(g_tests[i].summary), "no WiFi");
        }
        a->running = false;
        render();
        return;
    }
    snprintf(a->wifi_text, sizeof(a->wifi_text), "WiFi: connected");
    a->wifi_color = COL_SUCCESS;
    curl_global_init(0);

    for (int i = 0; i < NUM_TESTS && !a->abort; i++) {
        test_t *t = &g_tests[i];
        a->selected   = i; /* follow the running test */
        a->log_scroll = 0;
        t->state      = ST_RUNNING;
        render();

        printf("\n%d. %s:\n", i + 1, t->name);
        g_current      = i;
        g_partial_len  = 0;
        uint32_t start = SDL_GetTicks();
        bool     ok    = t->run();
        if (g_partial_len) log_flush_line();
        t->ms     = SDL_GetTicks() - start;
        t->state  = ok ? ST_PASS : ST_FAIL;
        g_current = -1;
        printf("=> %s (%u ms)\n", ok ? "PASS" : "FAIL", (unsigned)t->ms);

        pump_events();
        render();
    }

    for (int i = 0; i < NUM_TESTS; i++) {
        if (g_tests[i].state == ST_PENDING) {
            g_tests[i].state = ST_SKIPPED;
            snprintf(g_tests[i].summary, sizeof(g_tests[i].summary), "stopped");
        }
    }

    curl_global_cleanup();
    a->running = false;
    printf("=== %d/%d passed ===\n", passed_count(), NUM_TESTS);
    /* Show the first failure, if any. */
    for (int i = 0; i < NUM_TESTS; i++) {
        if (g_tests[i].state == ST_FAIL) {
            a->selected = i;
            break;
        }
    }
    a->log_scroll = 0;
    render();
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    g_tests = g_test_table;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    app_t *a  = &g_app;
    a->window = SDL_CreateWindow("curl test", SCREEN_WIDTH, SCREEN_HEIGHT, SDL_WINDOW_FULLSCREEN);
    a->renderer = a->window ? SDL_CreateRenderer(a->window, NULL) : NULL;
    a->texture  = a->renderer ? SDL_CreateTexture(a->renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING,
                                                  SCREEN_WIDTH, SCREEN_HEIGHT)
                              : NULL;
    a->pixels   = calloc(SCREEN_WIDTH * SCREEN_HEIGHT, sizeof(uint16_t));
    if (!a->texture || !a->pixels) {
        printf("curl_test: no display (%s)\n", SDL_GetError());
        free(a->pixels);
        if (a->texture) SDL_DestroyTexture(a->texture);
        if (a->renderer) SDL_DestroyRenderer(a->renderer);
        if (a->window) SDL_DestroyWindow(a->window);
        SDL_Quit();
        return 1;
    }

    run_all();

    while (!a->quit) {
        SDL_Event ev;
        if (!SDL_WaitEvent(&ev)) continue;
        bool rerun = false;
        do {
            if (ev.type == SDL_EVENT_QUIT) a->quit = true;
            else if (ev.type == SDL_EVENT_KEY_DOWN) {
                if (ev.key.key == SDLK_R) rerun = true;
                else handle_key(ev.key.key);
            }
        } while (SDL_PollEvent(&ev));
        if (rerun && !a->quit) run_all();
        else render();
    }

    wifi_disconnect();
    clear_results();
    free(a->pixels);
    SDL_DestroyTexture(a->texture);
    SDL_DestroyRenderer(a->renderer);
    SDL_DestroyWindow(a->window);
    SDL_Quit();
    return 0;
}
