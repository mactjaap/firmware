/* Minimal HTTP server to browse & download /sdcard/screenshots/
 * Build as a BadgeVMS app.
 */

#include "badgevms/wifi.h"

#include <SDL3/SDL.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(ESP_PLATFORM)
# include "freertos/FreeRTOS.h"
# include "freertos/task.h"
# define YIELD() vTaskDelay(pdMS_TO_TICKS(2))
#else
# define YIELD() ((void)0)
#endif

#define SCREENSHOT_ROOT   "/sdcard/screenshots"
#define LISTEN_PORT       8080
#define RECV_BUF_SZ       1024
#define SEND_BUF_SZ       1024

/* --- small helpers ------------------------------------------------------- */

static int starts_with(const char *s, const char *p) {
    return s && p && strncmp(s, p, strlen(p)) == 0;
}

static void url_decode_inplace(char *s) {
    /* very small %XX decoder, + stays as + (we don’t convert + to space) */
    if (!s) return;
    char *r = s, *w = s;
    while (*r) {
        if (r[0]=='%' && r[1] && r[2]) {
            int hi = r[1], lo = r[2];
            int v = 0;
            if      (hi>='0'&&hi<='9') v = (hi-'0')<<4;
            else if (hi>='A'&&hi<='F') v = (hi-'A'+10)<<4;
            else if (hi>='a'&&hi<='f') v = (hi-'a'+10)<<4;
            else { *w++ = *r++; continue; }
            if      (lo>='0'&&lo<='9') v |= (lo-'0');
            else if (lo>='A'&&lo<='F') v |= (lo-'A'+10);
            else if (lo>='a'&&lo<='f') v |= (lo-'a'+10);
            else { *w++ = *r++; continue; }
            *w++ = (char)v; r += 3;
        } else {
            *w++ = *r++;
        }
    }
    *w = 0;
}

static const char* guess_content_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (!strcasecmp(ext, ".bmp")) return "image/bmp";
    if (!strcasecmp(ext, ".png")) return "image/png";
    if (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg")) return "image/jpeg";
    if (!strcasecmp(ext, ".gif")) return "image/gif";
    if (!strcasecmp(ext, ".txt")) return "text/plain; charset=utf-8";
    if (!strcasecmp(ext, ".html")|| !strcasecmp(ext, ".htm")) return "text/html; charset=utf-8";
    return "application/octet-stream";
}

static ssize_t send_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t*)buf;
    size_t left = len;
    while (left) {
        ssize_t n = send(fd, p, left, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return n;
        }
        p += n;
        left -= (size_t)n;
    }
    return (ssize_t)len;
}

/* HTML escape minimal */
static void html_escape(const char *in, char *out, size_t cap) {
    size_t o=0;
    for (; *in && o+6<cap; ++in) {
        unsigned char c = (unsigned char)*in;
        if (c=='&'){ memcpy(out+o,"&amp;",5); o+=5; }
        else if(c=='<'){ memcpy(out+o,"&lt;",4); o+=4; }
        else if(c=='>'){ memcpy(out+o,"&gt;",4); o+=4; }
        else { out[o++]=c; }
    }
    out[o]=0;
}

/* --- request handling ---------------------------------------------------- */

static void respond_400(int cfd) {
    const char *h = "HTTP/1.1 400 Bad Request\r\n"
                    "Content-Type: text/plain\r\n"
                    "Connection: close\r\n\r\nBad Request\n";
    send_all(cfd, h, strlen(h));
}

static void respond_404(int cfd) {
    const char *h = "HTTP/1.1 404 Not Found\r\n"
                    "Content-Type: text/plain\r\n"
                    "Connection: close\r\n\r\nNot Found\n";
    send_all(cfd, h, strlen(h));
}

static void respond_500(int cfd, const char *msg) {
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
        "HTTP/1.1 500 Internal Server Error\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n\r\nInternal Error: %s\n",
        msg ? msg : "unknown");
    if (n < 0) n = 0;
    send_all(cfd, buf, (size_t)n);
}

static void serve_file(int cfd, const char *fullpath, const char *req_path) {
    int fd = open(fullpath, O_RDONLY);
    if (fd < 0) { respond_404(cfd); return; }

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd); respond_404(cfd); return;
    }

    const char *ctype = guess_content_type(req_path);
    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: close\r\n\r\n",
        ctype, (long)st.st_size);
    if (hn < 0) { close(fd); respond_500(cfd,"header"); return; }

    if (send_all(cfd, hdr, (size_t)hn) < 0) { close(fd); return; }

    char buf[SEND_BUF_SZ];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        if (send_all(cfd, buf, (size_t)n) < 0) break;
        YIELD();
    }
    close(fd);
}

static void serve_listing(int cfd) {
    DIR *d = opendir(SCREENSHOT_ROOT);
    if (!d) {
        respond_500(cfd, "opendir screenshots");
        return;
    }

    const char *hdr = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: text/html; charset=utf-8\r\n"
                      "Connection: close\r\n\r\n";
    send_all(cfd, hdr, strlen(hdr));

    /* simple page head */
    const char *head =
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<title>Badge Screenshots</title>"
        "<style>body{font-family:system-ui,-apple-system,Segoe UI,Arial,sans-serif;background:#0A0E14;color:#E5E7EB;padding:20px}a{color:#64EFFE;text-decoration:none}li{margin:6px 0}code{background:#111826;padding:2px 6px;border-radius:6px}</style>"
        "</head><body><h1>Badge Screenshots</h1>"
        "<p>Folder: <code>/sdcard/screenshots</code></p><ul>";
    send_all(cfd, head, strlen(head));

    struct dirent *ent;
    char line[512], esc[256];
    while ((ent = readdir(d)) != NULL) {
        const char *n = ent->d_name;
        if (strcmp(n,".")==0 || strcmp(n,"..")==0) continue;

        /* Show only regular files */
        char full[512];
        snprintf(full, sizeof(full), SCREENSHOT_ROOT "/%s", n);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        html_escape(n, esc, sizeof esc);
        int ln = snprintf(line, sizeof(line),
                          "<li><a href=\"/%s\">%s</a> <small>(%ld bytes)</small></li>",
                          esc, esc, (long)st.st_size);
        if (ln > 0) send_all(cfd, line, (size_t)ln);
        YIELD();
    }
    closedir(d);

    const char *tail = "</ul><p>Tip: point your browser to <code>http://badge-ip:8080/</code>.</p></body></html>";
    send_all(cfd, tail, strlen(tail));
}

static void handle_client(int cfd) {
    char req[RECV_BUF_SZ];
    int n = recv(cfd, req, sizeof(req)-1, 0);
    if (n <= 0) { return; }
    req[n] = 0;

    /* parse first line: METHOD SP PATH SP HTTP/x.y */
    char method[8]={0}, path[256]={0};
    if (sscanf(req, "%7s %255s", method, path) != 2) { respond_400(cfd); return; }
    if (strcmp(method, "GET") != 0) { respond_400(cfd); return; }

    url_decode_inplace(path);

    /* security: no traversal */
    if (strstr(path, "..")) { respond_404(cfd); return; }

    if (strcmp(path, "/") == 0) {
        serve_listing(cfd);
        return;
    }

    /* strip leading '/' */
    const char *leaf = path[0]=='/' ? path+1 : path;

    char full[512];
    snprintf(full, sizeof(full), SCREENSHOT_ROOT "/%s", leaf);
    serve_file(cfd, full, leaf);
}

/* --- server loop --------------------------------------------------------- */

static int run_server_loop(void) {
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) { perror("socket"); return -1; }

    int yes = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(LISTEN_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sfd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(sfd);
        return -2;
    }
    if (listen(sfd, 4) != 0) {
        perror("listen");
        close(sfd);
        return -3;
    }

    printf("[screenshot_server] Listening on port %d\n", LISTEN_PORT);

    /* non-blocking accept so we can keep UI responsive */
    int flags = fcntl(sfd, F_GETFL, 0);
    fcntl(sfd, F_SETFL, flags | O_NONBLOCK);

    for (;;) {
        struct sockaddr_in cli; socklen_t cl = sizeof(cli);
        int cfd = accept(sfd, (struct sockaddr*)&cli, &cl);
        if (cfd >= 0) {
            handle_client(cfd);
            close(cfd);
        } else {
            /* no client — allow caller to pump UI / quit */
            return sfd; /* return the listening socket so caller can poll again */
        }
    }
}

/* --- SDL status UI + main ------------------------------------------------ */

int main(void) {
    printf("[screenshot_server] starting\n");
    wifi_connect(); /* ensure Wi-Fi up */

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        printf("[screenshot_server] SDL_Init failed: %s\n", SDL_GetError());
        return 0;
    }

    SDL_Window  *win = SDL_CreateWindow("Screenshot Server", 480, 200, 0);
    SDL_Renderer*ren = SDL_CreateRenderer(win, NULL);
    if (!win || !ren) {
        printf("[screenshot_server] SDL window/renderer failed: %s\n", SDL_GetError());
        if (ren) SDL_DestroyRenderer(ren);
        if (win) SDL_DestroyWindow(win);
        SDL_Quit();
        return 0;
    }

    /* ensure folder exists */
    mkdir(SCREENSHOT_ROOT, 0777);

    bool running = true;
    int listen_fd = -1;

    while (running) {
        /* Pump one accept/handle cycle (non-blocking) */
        int sfd = run_server_loop();
        if (listen_fd < 0 && sfd >= 0) listen_fd = sfd;

        /* Draw status */
        SDL_SetRenderDrawColor(ren, 10,14,20,255);
        SDL_RenderClear(ren);
        SDL_SetRenderDrawColor(ren, 17,24,38,255);
        SDL_FRect panel = {8,8, 480-16, 200-16};
        SDL_RenderFillRect(ren, &panel);

        /* Cyan header */
        SDL_SetRenderDrawColor(ren, 100,239,254,255);
        SDL_FRect bar = {8,8, 480-16, 26};
        SDL_RenderFillRect(ren, &bar);

        /* Text-ish via simple rectangles (no TTF): draw a few bars as hints */
        /* Instead, just rely on serial log for details. */

        SDL_RenderPresent(ren);

        /* Events (ESC to quit) */
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = false;
            if (ev.type == SDL_EVENT_KEY_DOWN) {
                if (ev.key.scancode == SDL_SCANCODE_ESCAPE) running = false;
            }
        }
        SDL_Delay(15);
    }

    if (listen_fd >= 0) close(listen_fd);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    SDL_Quit();
    printf("[screenshot_server] exit\n");
    return 0;
}
