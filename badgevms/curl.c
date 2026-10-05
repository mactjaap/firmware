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

#include "curl/curl.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_tls_errors.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "miniz.h" /* esp_rom: tinfl_decompress() is in the ESP32-P4 ROM */
#include "task.h"
#include "thirdparty/dlmalloc.h"
#include "why_io.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <string.h>
#include <time.h>

static char const *TAG = "ESP_CURL";

typedef enum { CURL_SAMESITE_NONE = 0, CURL_SAMESITE_LAX, CURL_SAMESITE_STRICT } curl_samesite;

typedef struct cookie_entry {
    char                *name;
    char                *value;
    char                *domain;
    char                *path;
    time_t               expires;
    bool                 secure;
    bool                 http_only;
    curl_samesite        samesite;
    struct cookie_entry *next;
} cookie_entry_t;

struct curl_handle {
    esp_http_client_handle_t esp_client;
    esp_http_client_config_t config;

    curl_write_callback  write_function;
    void                *write_data;
    curl_header_callback header_function;
    void                *header_data;

    char              *post_data;
    size_t             post_data_size;
    struct curl_slist *headers;

    cookie_entry_t *cookies;
    char           *cookie_file;
    char           *cookie_jar;
    char           *manual_cookies;

    char          *proxy_url;
    char          *proxy_userpwd;
    curl_proxytype proxy_type;
    long           proxy_port;
    long           proxy_auth;

    int     response_code;
    int64_t content_length;
    char   *content_type;
    char   *effective_url;

    bool configured;
    bool verbose;
    bool ssl_verify_peer;
    long http_auth;

    /* 4.3: progress callback */
    curl_xferinfo_callback xferinfo_function;
    void                  *xferinfo_data;
    bool                   noprogress; /* libcurl default: 1 (no progress calls) */

    /* 4.3: redirects are followed here, not by esp_http_client */
    bool follow_location;
    long max_redirs;
    long redirect_count;

    /* 4.3: timeouts.  total_timeout_ms is only set by CURLOPT_TIMEOUT(_MS);
     * without it a transfer may take as long as data keeps arriving. */
    long total_timeout_ms;
    long idle_timeout_ms;

    /* 4.3: Accept-Encoding / transparent gzip+deflate decoding */
    char *accept_encoding;

    /* 4.3: per-transfer state */
    int64_t size_download;   /* body bytes received (before decoding) */
    char   *location;        /* Location header of the current response */
    int     content_encoding;
    bool    server_closes;   /* "Connection: close" in the current response */

    /* 4.3: connection reuse.  The esp_http_client (and its TCP/TLS
     * connection) stays alive between curl_easy_perform() calls on the same
     * handle while the server allows it, as libcurl does. */
    bool  conn_alive;        /* a connection from an earlier request is open */
    bool  recreate_client;   /* an option changed that esp_http_client only reads at init */
    char *conn_origin;       /* "scheme://host:port" of the open connection */
    char *sent_header_keys;  /* request headers set by the previous perform, '\n' separated */
    bool  auto_content_type; /* Content-Type was added for a request body */
    bool  custom_request;    /* CURLOPT_CUSTOMREQUEST chose the method */
    bool  own_cookie_header; /* the application set a "Cookie:" header */
};

enum { CONTENT_ENCODING_NONE = 0, CONTENT_ENCODING_GZIP, CONTENT_ENCODING_DEFLATE };

static size_t default_write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    printf("%.*s", (int)realsize, (char *)contents);
    return realsize;
}

static size_t default_header_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    why_fprintf(stderr, "%.*s", (int)realsize, (char *)contents);
    return realsize;
}

static void free_cookie(cookie_entry_t *cookie) {
    if (!cookie)
        return;

    dlfree(cookie->name);
    dlfree(cookie->value);
    dlfree(cookie->domain);
    dlfree(cookie->path);
    dlfree(cookie);
}

static void free_all_cookies(cookie_entry_t *cookies) {
    while (cookies) {
        cookie_entry_t *next = cookies->next;
        free_cookie(cookies);
        cookies = next;
    }
}

/* Lower-case host of url ("https://user@Host:8443/x" -> "host"), dlmalloc'd. */
static char *curl_url_host(char const *url) {
    char const *p = url ? strstr(url, "://") : NULL;
    if (!p)
        return NULL;
    p += 3;
    size_t      n  = strcspn(p, "/?#");
    char const *at = memchr(p, '@', n);
    if (at) {
        n -= (size_t)(at + 1 - p);
        p  = at + 1;
    }
    size_t host_len = n;
    if (n && p[0] == '[') {
        char const *close = memchr(p, ']', n);
        host_len          = close ? (size_t)(close - p + 1) : n;
    } else {
        char const *colon = memchr(p, ':', n);
        if (colon)
            host_len = (size_t)(colon - p);
    }
    char *host = dlmalloc(host_len + 1);
    if (!host)
        return NULL;
    for (size_t i = 0; i < host_len; i++) host[i] = (char)tolower((unsigned char)p[i]);
    host[host_len] = 0;
    return host;
}

/* Does a cookie for domain go to host?  Same host, or a subdomain of it.
 * "example.com" is what BadgeVMS before 4.3 stored for every cookie (it did
 * not know the host): such cookies from old jar files go everywhere, as before. */
static bool cookie_domain_matches(char const *domain, char const *host) {
    if (!domain || !*domain || strcmp(domain, "example.com") == 0)
        return true;
    if (!host)
        return false;
    while (*domain == '.') domain++;
    size_t dl = strlen(domain), hl = strlen(host);
    if (hl == dl)
        return strcasecmp(host, domain) == 0;
    return hl > dl && host[hl - dl - 1] == '.' && strcasecmp(host + hl - dl, domain) == 0;
}

static cookie_entry_t *parse_set_cookie(char const *set_cookie_header, char const *request_host) {
    if (!set_cookie_header) {
        ESP_LOGW(TAG, "No cookie header");
        return NULL;
    }

    ESP_LOGW(TAG, "Parsing header '%s'", set_cookie_header);

    cookie_entry_t *cookie = dlcalloc(1, sizeof(cookie_entry_t));
    if (!cookie) {
        ESP_LOGW(TAG, "Cookie dlmalloc failed (cookie)");
        return NULL;
    }

    char *header_copy = why_strdup(set_cookie_header);
    if (!header_copy) {
        ESP_LOGW(TAG, "Cookie dlmalloc failed (header_copy)");
        free_cookie(cookie);
        return NULL;
    }

    char *first_semicolon = strchr(header_copy, ';');
    char *name_value_part = header_copy;

    if (first_semicolon) {
        *first_semicolon = '\0';
    }

    char *equals = strchr(name_value_part, '=');
    if (equals) {
        *equals = '\0';

        char *name  = name_value_part;
        char *value = equals + 1;

        while (*name == ' ' || *name == '\t') name++;
        char *name_end = name + strlen(name) - 1;
        while (name_end > name && (*name_end == ' ' || *name_end == '\t')) {
            *name_end = '\0';
            name_end--;
        }

        while (*value == ' ' || *value == '\t') value++;
        char *value_end = value + strlen(value) - 1;
        while (value_end > value && (*value_end == ' ' || *value_end == '\t')) {
            *value_end = '\0';
            value_end--;
        }

        cookie->name  = why_strdup(name);
        cookie->value = why_strdup(value);

        if (!cookie->name || !cookie->value) {
            ESP_LOGW(TAG, "No name or no value for '%s'", header_copy);
            dlfree(header_copy);
            free_cookie(cookie);
            return NULL;
        }
    } else {
        ESP_LOGW(TAG, "No = for '%s'", header_copy);
        dlfree(header_copy);
        free_cookie(cookie);
        return NULL;
    }

    cookie->domain    = why_strdup(request_host && *request_host ? request_host : "example.com");
    cookie->path      = why_strdup("/");
    cookie->expires   = 0;
    cookie->secure    = false;
    cookie->http_only = false;
    cookie->samesite  = CURL_SAMESITE_NONE; // Default per RFC

    if (!cookie->domain || !cookie->path) {
        ESP_LOGW(TAG, "No domain or cookie path for '%s'", header_copy);
        dlfree(header_copy);
        free_cookie(cookie);
        return NULL;
    }

    if (first_semicolon) {
        char *attr_start = first_semicolon + 1;

        while (attr_start && *attr_start) {
            while (*attr_start == ' ' || *attr_start == '\t') {
                attr_start++;
            }

            if (!*attr_start)
                break;

            char *attr_end = strchr(attr_start, ';');
            if (attr_end) {
                *attr_end = '\0';
            }

            char *attr_equals = strchr(attr_start, '=');
            if (attr_equals) {
                *attr_equals     = '\0';
                char *attr_name  = attr_start;
                char *attr_value = attr_equals + 1;

                while (*attr_name == ' ' || *attr_name == '\t') attr_name++;
                char *name_end = attr_name + strlen(attr_name) - 1;
                while (name_end > attr_name && (*name_end == ' ' || *name_end == '\t')) {
                    *name_end = '\0';
                    name_end--;
                }

                while (*attr_value == ' ' || *attr_value == '\t') {
                    attr_value++;
                }
                char *value_end = attr_value + strlen(attr_value) - 1;
                while (value_end > attr_value && (*value_end == ' ' || *value_end == '\t')) {
                    *value_end = '\0';
                    value_end--;
                }

                if (strcasecmp(attr_name, "domain") == 0) {
                    if (request_host && *request_host && !cookie_domain_matches(attr_value, request_host)) {
                        ESP_LOGW(TAG, "Cookie domain %s does not match %s: ignored", attr_value, request_host);
                        dlfree(header_copy);
                        free_cookie(cookie);
                        return NULL;
                    }
                    dlfree(cookie->domain);
                    cookie->domain = why_strdup(attr_value);
                    if (!cookie->domain) {
                        ESP_LOGW(TAG, "No domain for '%s'", header_copy);
                        dlfree(header_copy);
                        free_cookie(cookie);
                        return NULL;
                    }
                } else if (strcasecmp(attr_name, "path") == 0) {
                    dlfree(cookie->path);
                    cookie->path = why_strdup(attr_value);
                    if (!cookie->path) {
                        ESP_LOGW(TAG, "No path for '%s'", header_copy);
                        dlfree(header_copy);
                        free_cookie(cookie);
                        return NULL;
                    }
                } else if (strcasecmp(attr_name, "expires") == 0) {
                    // RFC 1123 date format...
                    cookie->expires = 2147483647; // Max time_t for 32-bit (year 2038)
                } else if (strcasecmp(attr_name, "max-age") == 0) {
                    long max_age = atol(attr_value);
                    if (max_age > 0) {
                        time_t current_time = time(NULL);
                        if (current_time != (time_t)-1) {
                            cookie->expires = current_time + max_age;
                        }
                    } else if (max_age == 0) {
                        // max-age=0 means delete cookie immediately
                        cookie->expires = 1; // Past time
                    }
                } else if (strcasecmp(attr_name, "samesite") == 0) {
                    if (strcasecmp(attr_value, "strict") == 0) {
                        cookie->samesite = CURL_SAMESITE_STRICT;
                    } else if (strcasecmp(attr_value, "lax") == 0) {
                        cookie->samesite = CURL_SAMESITE_LAX;
                    } else if (strcasecmp(attr_value, "none") == 0) {
                        cookie->samesite = CURL_SAMESITE_NONE;
                    } else {
                        // Invalid SameSite value, default to None per spec
                        cookie->samesite = CURL_SAMESITE_NONE;
                        ESP_LOGW(TAG, "Invalid SameSite value: %s, defaulting to None", attr_value);
                    }
                }
                // More attributes... maybe

            } else {
                char *attr_name = attr_start;

                while (*attr_name == ' ' || *attr_name == '\t') attr_name++;
                char *name_end = attr_name + strlen(attr_name) - 1;
                while (name_end > attr_name && (*name_end == ' ' || *name_end == '\t')) {
                    *name_end = '\0';
                    name_end--;
                }

                if (strcasecmp(attr_name, "secure") == 0) {
                    cookie->secure = true;
                } else if (strcasecmp(attr_name, "httponly") == 0) {
                    cookie->http_only = true;
                }
            }

            if (attr_end) {
                attr_start = attr_end + 1;
            } else {
                break;
            }
        }
    }

    dlfree(header_copy);
    return cookie;
}

static void add_cookie(curl_handle_t *curl, cookie_entry_t *new_cookie) {
    if (!curl || !new_cookie)
        return;

    cookie_entry_t **current = &curl->cookies;
    while (*current) {
        if (strcmp((*current)->name, new_cookie->name) == 0 &&
            strcasecmp((*current)->domain ? (*current)->domain : "", new_cookie->domain ? new_cookie->domain : "") == 0) {
            cookie_entry_t *to_remove = *current;
            *current                  = (*current)->next;
            free_cookie(to_remove);
            break;
        }
        current = &(*current)->next;
    }

    new_cookie->next = curl->cookies;
    curl->cookies    = new_cookie;
}

/* Cookie header for a request to url: CURLOPT_COOKIE plus the stored
 * cookies for that host.  Stored cookies are left out when the application
 * sends its own "Cookie:" header (it manages cookies itself). */
static char *build_cookie_header(curl_handle_t *curl, char const *url, bool use_store) {
    char  *host      = use_store ? curl_url_host(url) : NULL;
    time_t now       = time(NULL);
    size_t total_len = curl->manual_cookies ? strlen(curl->manual_cookies) : 0;

    for (int pass = 0; pass < 2; pass++) {
        char *result = NULL;
        if (pass == 1) {
            if (total_len == 0)
                break;
            result = dlmalloc(total_len + 1);
            if (!result)
                break;
            result[0] = 0;
            if (curl->manual_cookies)
                strcat(result, curl->manual_cookies);
        }
        for (cookie_entry_t *c = use_store ? curl->cookies : NULL; c; c = c->next) {
            if (!cookie_domain_matches(c->domain, host) || (c->expires > 0 && c->expires < now))
                continue;
            if (pass == 0) {
                total_len += (total_len ? 2 : 0) + strlen(c->name) + 1 + strlen(c->value);
            } else {
                if (result[0])
                    strcat(result, "; ");
                strcat(result, c->name);
                strcat(result, "=");
                strcat(result, c->value);
            }
        }
        if (pass == 1) {
            dlfree(host);
            return result;
        }
    }
    dlfree(host);
    return NULL;
}

static int load_cookies_from_file(curl_handle_t *curl, char const *filename) {
    if (!curl || !filename)
        return -1;

    FILE *file = why_fopen(filename, "r");
    if (!file) {
        ESP_LOGW(TAG, "Cookie file not found or can't be opened: %s", filename);
        return 0;
    }

    char line[512];
    int  cookies_loaded = 0;

    while (why_fgets(line, sizeof(line), file)) {
        if (line[0] == '\n' || line[0] == '#' || line[0] == '\0') {
            continue;
        }

        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }

        char *name          = strtok(line, "\t");
        char *value         = strtok(NULL, "\t");
        char *domain        = strtok(NULL, "\t");
        char *path          = strtok(NULL, "\t");
        char *expires_str   = strtok(NULL, "\t");
        char *secure_str    = strtok(NULL, "\t");
        char *http_only_str = strtok(NULL, "\t");
        char *samesite_str  = strtok(NULL, "\t");

        if (!name || !value || !domain || !path || !expires_str || !secure_str || !http_only_str || !samesite_str) {
            ESP_LOGW(TAG, "Malformed cookie line in file: %s", filename);
            continue;
        }

        cookie_entry_t *cookie = dlcalloc(1, sizeof(cookie_entry_t));
        if (!cookie) {
            ESP_LOGE(TAG, "Failed to allocate memory for cookie");
            continue;
        }

        cookie->name      = why_strdup(name);
        cookie->value     = why_strdup(value);
        cookie->domain    = why_strdup(domain);
        cookie->path      = why_strdup(path);
        cookie->expires   = (time_t)atol(expires_str);
        cookie->secure    = (atoi(secure_str) != 0);
        cookie->http_only = (atoi(http_only_str) != 0);
        cookie->samesite  = (atoi(samesite_str) != 0);

        time_t current_time = time(NULL);
        if (cookie->expires > 0 && cookie->expires < current_time) {
            ESP_LOGW(TAG, "Skipping expired cookie: %s", cookie->name);
            free_cookie(cookie);
            continue;
        }

        add_cookie(curl, cookie);
        cookies_loaded++;
    }

    why_fclose(file);
    ESP_LOGW(TAG, "Loaded %d cookies from file: %s", cookies_loaded, filename);
    return cookies_loaded;
}

static int save_cookies_to_file(curl_handle_t *curl, char const *filename) {
    if (!curl || !filename)
        return -1;

    FILE *file = why_fopen(filename, "w");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open cookie file for writing: %s", filename);
        return -1;
    }

    why_fprintf(file, "# BadgeVMS cookie jar file\n");
    why_fprintf(file, "# Format: name\\tvalue\\tdomain\\tpath\\texpires\\tsecure\\thttp_only\\tsamesite\t\n");

    int    cookies_saved = 0;
    time_t current_time  = time(NULL);

    cookie_entry_t *cookie = curl->cookies;
    while (cookie) {
        if (cookie->expires > 0 && cookie->expires < current_time) {
            ESP_LOGW(TAG, "Skipping expired cookie when saving: %s", cookie->name);
            cookie = cookie->next;
            continue;
        }
        ESP_LOGW(TAG, "Saving cookie: %s", cookie->name);

        why_fprintf(
            file,
            "%s\t%s\t%s\t%s\t%ld\t%d\t%d\t%d\t\n",
            cookie->name ? cookie->name : "",
            cookie->value ? cookie->value : "",
            cookie->domain ? cookie->domain : "",
            cookie->path ? cookie->path : "",
            (long)cookie->expires,
            cookie->secure ? 1 : 0,
            cookie->http_only ? 1 : 0,
            cookie->samesite ? 1 : 0
        );

        cookies_saved++;
        cookie = cookie->next;
    }

    ESP_LOGW(TAG, "Closing jar");
    why_fclose(file);
    ESP_LOGW(TAG, "Saved %d cookies to file: %s", cookies_saved, filename);
    return cookies_saved;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    curl_handle_t *curl = (curl_handle_t *)evt->user_data;

    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            if (curl->verbose) {
                ESP_LOGI(TAG, "HTTP_EVENT_ERROR");
            }
            break;

        case HTTP_EVENT_ON_CONNECTED:
            if (curl->verbose) {
                ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
            }
            break;

        case HTTP_EVENT_HEADER_SENT:
            if (curl->verbose) {
                ESP_LOGI(TAG, "HTTP_EVENT_HEADER_SENT");
            }
            break;

        case HTTP_EVENT_ON_HEADER:
            if (evt->header_key && evt->header_value) {
                char const *key   = evt->header_key;
                char const *value = evt->header_value;

                if (strcasecmp(key, "Content-Type") == 0) {
                    /* Remember Content-Type for CURLINFO_CONTENT_TYPE. */
                    dlfree(curl->content_type);
                    curl->content_type = why_strdup(value);
                } else if (strcasecmp(key, "Location") == 0) {
                    dlfree(curl->location);
                    curl->location = why_strdup(value);
                } else if (strcasecmp(key, "Content-Encoding") == 0) {
                    if (strcasecmp(value, "gzip") == 0 || strcasecmp(value, "x-gzip") == 0) {
                        curl->content_encoding = CONTENT_ENCODING_GZIP;
                    } else if (strcasecmp(value, "deflate") == 0) {
                        curl->content_encoding = CONTENT_ENCODING_DEFLATE;
                    } else {
                        curl->content_encoding = CONTENT_ENCODING_NONE;
                    }
                } else if (strcasecmp(key, "Connection") == 0) {
                    if (strcasecmp(value, "close") == 0) {
                        curl->server_closes = true;
                    }
                } else if (strcasecmp(key, "Set-Cookie") == 0) {
                    /* Cookies are kept per handle (as BadgeVMS always did) and
                     * sent back only to the host that set them (4.3). */
                    char           *host   = curl_url_host(curl->effective_url);
                    cookie_entry_t *cookie = parse_set_cookie(value, host);
                    dlfree(host);
                    if (cookie) {
                        add_cookie(curl, cookie);
                    }
                }
            }

            if (curl->header_function && evt->header_key && evt->header_value) {
                size_t key_len = strlen(evt->header_key);
                size_t val_len = strlen(evt->header_value);

                char *tmp_header = dlmalloc(key_len + val_len + 2);
                if (!tmp_header) {
                    break;
                }
                memcpy(tmp_header, evt->header_key, key_len);
                tmp_header[key_len]     = ':';
                tmp_header[key_len + 1] = ' ';
                memcpy(&tmp_header[key_len + 2], evt->header_value, val_len);

                curl->header_function(tmp_header, 1, key_len + val_len + 2, curl->header_data);
                dlfree(tmp_header);
            }
            break;

        case HTTP_EVENT_ON_DATA:
            /* The body is read with esp_http_client_read() in
             * curl_easy_perform(), which can stop the transfer. */
            break;

        case HTTP_EVENT_ON_FINISH:
            break;

        case HTTP_EVENT_DISCONNECTED:
            if (curl->verbose) {
                ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            }
            break;

        case HTTP_EVENT_REDIRECT:
            if (curl->verbose) {
                ESP_LOGI(TAG, "HTTP_EVENT_REDIRECT");
            }
            break;
    }
    return ESP_OK;
}

CURL *curl_easy_init(void) {
    curl_handle_t *curl = dlcalloc(1, sizeof(curl_handle_t));
    if (!curl) {
        return NULL;
    }

    curl->write_function  = default_write_callback;
    curl->header_function = default_header_callback;

    memset(&curl->config, 0, sizeof(esp_http_client_config_t));
    curl->config.event_handler = http_event_handler;
    curl->config.user_data     = curl;
    curl->config.timeout_ms    = 30000;

    curl->ssl_verify_peer          = true;
    curl->config.crt_bundle_attach = esp_crt_bundle_attach;

    // Initialize proxy settings
    curl->proxy_type = CURLPROXY_HTTP;
    curl->proxy_port = 0;
    curl->proxy_auth = CURLAUTH_BASIC;
    curl->http_auth  = CURLAUTH_BASIC;

    curl->noprogress       = true;
    curl->follow_location  = false;
    curl->max_redirs       = 20;
    curl->total_timeout_ms = 0;
    curl->idle_timeout_ms  = 30000;

    return (CURL *)curl;
}

CURLcode curl_easy_setopt(CURL *curl_handle, CURLoption option, ...) {
    if (!curl_handle) {
        return CURLE_FAILED_INIT;
    }

    curl_handle_t *curl = (curl_handle_t *)curl_handle;
    va_list        args;
    va_start(args, option);

    switch (option) {
        case CURLOPT_URL: {
            char const *url = va_arg(args, char const *);
            dlfree((void *)curl->config.url);
            curl->config.url = why_strdup(url);
            break;
        }

        case CURLOPT_USERAGENT: {
            char const *agent = va_arg(args, char const *);
            dlfree((void *)curl->config.user_agent);
            curl->config.user_agent = why_strdup(agent);
            curl->recreate_client   = true;
            break;
        }

        case CURLOPT_TIMEOUT: {
            long timeout            = va_arg(args, long);
            curl->config.timeout_ms = timeout > 0 ? timeout * 1000 : 30000;
            curl->total_timeout_ms  = timeout > 0 ? timeout * 1000 : 0;
            curl->idle_timeout_ms   = curl->config.timeout_ms;
            break;
        }

        case CURLOPT_TIMEOUT_MS: {
            long timeout_ms         = va_arg(args, long);
            curl->config.timeout_ms = timeout_ms > 0 ? timeout_ms : 30000;
            curl->total_timeout_ms  = timeout_ms > 0 ? timeout_ms : 0;
            curl->idle_timeout_ms   = curl->config.timeout_ms;
            break;
        }

        case CURLOPT_SSL_VERIFYPEER: {
            long verify           = va_arg(args, long);
            curl->ssl_verify_peer = (verify != 0);
            if (verify) {
                curl->config.crt_bundle_attach           = esp_crt_bundle_attach;
                curl->config.skip_cert_common_name_check = false;
            } else {
                curl->config.crt_bundle_attach           = NULL;
                curl->config.skip_cert_common_name_check = true;
            }
            curl->recreate_client = true;
            break;
        }

        case CURLOPT_SSL_VERIFYHOST: {
            long verify = va_arg(args, long);
            if (verify == 0) {
                curl->config.skip_cert_common_name_check = true;
            } else {
                curl->config.skip_cert_common_name_check = false;
            }
            curl->recreate_client = true;
            break;
        }

        case CURLOPT_CAINFO: {
            char const *ca_file = va_arg(args, char const *);
            // In ESP-IDF, this should be the actual certificate content, not a file path
            dlfree((void *)curl->config.cert_pem);
            curl->config.cert_pem          = why_strdup(ca_file);
            curl->config.crt_bundle_attach = NULL;
            curl->recreate_client          = true;
            break;
        }

        case CURLOPT_USERPWD: {
            char const *userpwd      = va_arg(args, char const *);
            char       *userpwd_copy = why_strdup(userpwd);
            char       *colon        = strchr(userpwd_copy, ':');
            if (colon) {
                *colon = '\0';
                dlfree((void *)curl->config.username);
                dlfree((void *)curl->config.password);
                curl->config.username = why_strdup(userpwd_copy);
                curl->config.password = why_strdup(colon + 1);
            }
            dlfree(userpwd_copy);
            curl->recreate_client = true;
            break;
        }

        case CURLOPT_POSTFIELDS: {
            char const *data = va_arg(args, char const *);
            dlfree(curl->post_data);
            curl->post_data      = data ? why_strdup(data) : NULL;
            curl->post_data_size = data ? strlen(data) : 0;
            /* As in libcurl, POSTFIELDS makes the request a POST (unless
             * CURLOPT_CUSTOMREQUEST chose another method, e.g. PUT). */
            if (data && !curl->custom_request)
                curl->config.method = HTTP_METHOD_POST;
            break;
        }

        case CURLOPT_COOKIELIST: {
            char const *cmd = va_arg(args, char const *);
            if (!cmd)
                break;
            if (strcasecmp(cmd, "ALL") == 0) {
                free_all_cookies(curl->cookies);
                curl->cookies = NULL;
            } else if (strcasecmp(cmd, "SESS") == 0) {
                cookie_entry_t **c = &curl->cookies;
                while (*c) {
                    if ((*c)->expires == 0) {
                        cookie_entry_t *gone = *c;
                        *c                   = gone->next;
                        free_cookie(gone);
                    } else {
                        c = &(*c)->next;
                    }
                }
            } else if (strcasecmp(cmd, "FLUSH") == 0) {
                if (curl->cookie_jar)
                    save_cookies_to_file(curl, curl->cookie_jar);
            } else if (strcasecmp(cmd, "RELOAD") == 0) {
                if (curl->cookie_file)
                    load_cookies_from_file(curl, curl->cookie_file);
            } else if (strncasecmp(cmd, "Set-Cookie:", 11) == 0) {
                char           *host   = curl_url_host(curl->config.url);
                cookie_entry_t *cookie = parse_set_cookie(cmd + 11, host);
                dlfree(host);
                if (cookie)
                    add_cookie(curl, cookie);
            } else {
                ESP_LOGW(TAG, "CURLOPT_COOKIELIST: unsupported command '%s'", cmd);
            }
            break;
        }

        case CURLOPT_POSTFIELDSIZE: {
            long size            = va_arg(args, long);
            curl->post_data_size = size;
            break;
        }

        case CURLOPT_HTTPHEADER: {
            struct curl_slist *headers = va_arg(args, struct curl_slist *);
            curl->headers              = headers;
            break;
        }

        case CURLOPT_WRITEFUNCTION: {
            curl_write_callback func = va_arg(args, curl_write_callback);
            curl->write_function     = func;
            break;
        }

        case CURLOPT_WRITEDATA: {
            void *data       = va_arg(args, void *);
            curl->write_data = data;
            break;
        }

        case CURLOPT_HEADERFUNCTION: {
            curl_header_callback func = va_arg(args, curl_header_callback);
            curl->header_function     = func;
            break;
        }

        case CURLOPT_HEADERDATA: {
            void *data        = va_arg(args, void *);
            curl->header_data = data;
            break;
        }

        case CURLOPT_FOLLOWLOCATION: {
            long follow = va_arg(args, long);
            /*
             * Redirects are followed by curl_easy_perform() itself.  With
             * FOLLOWLOCATION 0 the 3xx response (and its Location header) is
             * returned to the caller, as in libcurl.
             */
            curl->follow_location              = follow != 0;
            curl->config.disable_auto_redirect = true;
            break;
        }

        case CURLOPT_MAXREDIRS: {
            long max_redirs  = va_arg(args, long);
            curl->max_redirs = max_redirs < 0 ? 50 : max_redirs;
            break;
        }

        case CURLOPT_NOPROGRESS: {
            long noprogress  = va_arg(args, long);
            curl->noprogress = noprogress != 0;
            break;
        }

        case CURLOPT_XFERINFOFUNCTION: {
            curl->xferinfo_function = va_arg(args, curl_xferinfo_callback);
            break;
        }

        case CURLOPT_XFERINFODATA: {
            curl->xferinfo_data = va_arg(args, void *);
            break;
        }

        case CURLOPT_ACCEPT_ENCODING: {
            /* NULL: no Accept-Encoding and no decoding.  "" (all supported)
             * or a list with gzip/deflate: send it and decode the body. */
            char const *encoding = va_arg(args, char const *);
            dlfree(curl->accept_encoding);
            curl->accept_encoding = NULL;
            if (encoding) {
                curl->accept_encoding = why_strdup(encoding[0] ? encoding : "gzip, deflate");
            }
            break;
        }

        case CURLOPT_VERBOSE: {
            long verbose  = va_arg(args, long);
            curl->verbose = verbose != 0;
            break;
        }

        case CURLOPT_CUSTOMREQUEST: {
            char const *method  = va_arg(args, char const *);
            curl->custom_request = method != NULL;
            if (!method) {
                curl->config.method = curl->post_data ? HTTP_METHOD_POST : HTTP_METHOD_GET;
                break;
            }
            curl->config.method = HTTP_METHOD_GET;
            if (strcmp(method, "GET") == 0)
                curl->config.method = HTTP_METHOD_GET;
            else if (strcmp(method, "POST") == 0)
                curl->config.method = HTTP_METHOD_POST;
            else if (strcmp(method, "PUT") == 0)
                curl->config.method = HTTP_METHOD_PUT;
            else if (strcmp(method, "DELETE") == 0)
                curl->config.method = HTTP_METHOD_DELETE;
            else if (strcmp(method, "HEAD") == 0)
                curl->config.method = HTTP_METHOD_HEAD;
            else if (strcmp(method, "PATCH") == 0)
                curl->config.method = HTTP_METHOD_PATCH;
            break;
        }

        case CURLOPT_POST: {
            long post = va_arg(args, long);
            if (post) {
                curl->config.method = HTTP_METHOD_POST;
            }
            break;
        }

        case CURLOPT_PUT: {
            long put = va_arg(args, long);
            if (put) {
                curl->config.method = HTTP_METHOD_PUT;
            }
            break;
        }

        case CURLOPT_HTTPGET: {
            long get = va_arg(args, long);
            if (get) {
                curl->config.method  = HTTP_METHOD_GET;
                curl->custom_request = false;
            }
            break;
        }

        case CURLOPT_NOBODY: {
            long nobody = va_arg(args, long);
            if (nobody) {
                curl->config.method = HTTP_METHOD_HEAD;
            }
            break;
        }

        case CURLOPT_COOKIE: {
            char const *cookies = va_arg(args, char const *);
            dlfree(curl->manual_cookies);
            curl->manual_cookies = why_strdup(cookies);
            break;
        }

        case CURLOPT_COOKIEFILE: {
            char const *filename = va_arg(args, char const *);
            dlfree(curl->cookie_file);
            curl->cookie_file = why_strdup(filename);
            load_cookies_from_file(curl, filename);
            break;
        }

        case CURLOPT_COOKIEJAR: {
            char const *filename = va_arg(args, char const *);
            dlfree(curl->cookie_jar);
            curl->cookie_jar = why_strdup(filename);
            break;
        }

        case CURLOPT_BUFFERSIZE: {
            long size                = va_arg(args, long);
            curl->config.buffer_size = size;
            curl->recreate_client    = true;
            break;
        }

        // Proxy options (stubs - not implemented in esp_http_client)
        case CURLOPT_PROXY: {
            char const *proxy = va_arg(args, char const *);
            dlfree(curl->proxy_url);
            curl->proxy_url = why_strdup(proxy);
            ESP_LOGW(TAG, "Proxy support not implemented in BadgeVMS HTTP client: %s", proxy);
            va_end(args);
            return CURLE_UNSUPPORTED_PROTOCOL;
        }

        case CURLOPT_PROXYUSERPWD: {
            char const *userpwd = va_arg(args, char const *);
            dlfree(curl->proxy_userpwd);
            curl->proxy_userpwd = why_strdup(userpwd);
            ESP_LOGW(TAG, "Proxy authentication not implemented in BadgeVMS HTTP client");
            va_end(args);
            return CURLE_UNSUPPORTED_PROTOCOL;
        }

        case CURLOPT_PROXYTYPE: {
            long type        = va_arg(args, long);
            curl->proxy_type = (curl_proxytype)type;
            ESP_LOGW(TAG, "Proxy type configuration not implemented in BadgeVMS HTTP client");
            va_end(args);
            return CURLE_UNSUPPORTED_PROTOCOL;
        }

        case CURLOPT_PROXYPORT: {
            long port        = va_arg(args, long);
            curl->proxy_port = port;
            ESP_LOGW(TAG, "Proxy port configuration not implemented in BadgeVMS HTTP client");
            va_end(args);
            return CURLE_UNSUPPORTED_PROTOCOL;
        }

        case CURLOPT_PROXYAUTH: {
            long auth        = va_arg(args, long);
            curl->proxy_auth = auth;
            ESP_LOGW(TAG, "Proxy authentication type not implemented in BadgeVMS HTTP client");
            va_end(args);
            return CURLE_UNSUPPORTED_PROTOCOL;
        }

        case CURLOPT_HTTPAUTH: {
            long auth       = va_arg(args, long);
            curl->http_auth = auth;
            // The actual HTTP auth is handled through username/password
            break;
        }

        case CURLOPT_RANGE: {
            char const *range = va_arg(args, char const *);
            ESP_LOGW(TAG, "CURLOPT_RANGE not implemented: %s", range);
            va_end(args);
            return CURLE_UNSUPPORTED_PROTOCOL;
        }

        case CURLOPT_REFERER: {
            char const *referer = va_arg(args, char const *);
            ESP_LOGW(TAG, "CURLOPT_REFERER not implemented: %s", referer);
            va_end(args);
            return CURLE_UNSUPPORTED_PROTOCOL;
        }

        default:
            ESP_LOGW(TAG, "Unsupported curl option: %d", option);
            va_end(args);
            return CURLE_UNSUPPORTED_PROTOCOL;
    }

    va_end(args);
    return CURLE_OK;
}

/* ---------- 4.3: curl_easy_perform() on the native esp_http_client API ----------
 *
 * esp_http_client_perform() delivers the body through HTTP_EVENT_ON_DATA and
 * ignores what the event handler returns, so a transfer could not be stopped,
 * did not report progress and always closed its connection.  The request is
 * now driven with esp_http_client_open() / fetch_headers() / read(), which
 * lets curl_easy_perform():
 *   - honour the write callback: returning less than the size given stops
 *     the transfer with CURLE_WRITE_ERROR (as in libcurl);
 *   - call CURLOPT_XFERINFOFUNCTION at least twice a second, also while it
 *     waits for the server; non-zero stops with CURLE_ABORTED_BY_CALLBACK;
 *   - follow redirects itself and report the final URL (CURLINFO_EFFECTIVE_URL);
 *   - keep the connection open for the next request on the same handle
 *     (same scheme, host and port), unless the server closes it;
 *   - decode "Content-Encoding: gzip/deflate" when CURLOPT_ACCEPT_ENCODING
 *     is set, with the inflater in the ESP32-P4 ROM;
 *   - tell DNS, connect, TLS and timeout failures apart.
 */

#define CURL_POLL_MS        500   /* longest wait without a progress call */
#define CURL_READ_CHUNK     4096
#define CURL_DRAIN_MAX      (64 * 1024)

typedef struct {
    tinfl_decompressor decomp;
    uint8_t            dict[TINFL_LZ_DICT_SIZE];
    size_t             dict_ofs;
    bool               zlib;       /* deflate with a zlib header */
    bool               done;
    uint8_t            first[2];   /* "deflate": first bytes, to detect a zlib header */
    /* gzip header parser */
    int      gz_pos;               /* bytes of the fixed 10-byte header seen */
    uint8_t  gz_flags;
    int      gz_stage;             /* 0 fixed, 1 extra len, 2 extra, 3 name, 4 comment, 5 hcrc, 6 data */
    uint32_t gz_skip;
    int      encoding;
} curl_inflate_t;

static uint32_t curl_now_ms(void) {
    return (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
}

/* "https://host:443" for url, so a kept connection is only reused for the
 * same scheme, host and port. */
static char *curl_url_origin(char const *url) {
    if (!url)
        return NULL;
    char const *sep = strstr(url, "://");
    if (!sep)
        return NULL;
    size_t scheme_len = (size_t)(sep - url);
    char const *host  = sep + 3;
    size_t host_len   = strcspn(host, "/?#");
    char const *at    = memchr(host, '@', host_len);
    if (at) {
        host_len -= (size_t)(at + 1 - host);
        host = at + 1;
    }
    bool has_port = false;
    if (host_len && host[0] == '[') {
        char const *close = memchr(host, ']', host_len);
        has_port          = close && (size_t)(close - host + 1) < host_len;
    } else {
        has_port = memchr(host, ':', host_len) != NULL;
    }
    bool https = scheme_len == 5 && strncasecmp(url, "https", 5) == 0;
    char *out  = dlmalloc(scheme_len + 3 + host_len + 8);
    if (!out)
        return NULL;
    size_t n = 0;
    for (size_t i = 0; i < scheme_len; i++) out[n++] = (char)tolower((unsigned char)url[i]);
    memcpy(out + n, "://", 3);
    n += 3;
    for (size_t i = 0; i < host_len; i++) out[n++] = (char)tolower((unsigned char)host[i]);
    out[n] = 0;
    if (!has_port)
        strcat(out, https ? ":443" : ":80");
    return out;
}

/* Resolve a Location header against the URL it came from. */
static char *curl_resolve_location(char const *base, char const *location) {
    if (!location || !*location)
        return NULL;
    while (*location == ' ' || *location == '\t') location++;

    /* Absolute ("https://..."): a scheme is a letter followed by letters,
     * digits, '+', '-' or '.', then ':'. */
    if (isalpha((unsigned char)location[0])) {
        char const *p = location + 1;
        while (isalnum((unsigned char)*p) || *p == '+' || *p == '-' || *p == '.') p++;
        if (*p == ':')
            return why_strdup(location);
    }

    char const *sep = base ? strstr(base, "://") : NULL;
    if (!sep)
        return why_strdup(location);
    size_t scheme_len = (size_t)(sep - base);
    char const *host  = sep + 3;
    size_t host_len   = strcspn(host, "/?#");

    size_t authority_end = (size_t)(host + host_len - base);
    size_t path_end      = authority_end + strcspn(host + host_len, "?#");

    size_t prefix_len;
    if (location[0] == '?') {
        prefix_len = path_end; /* same path, new query */
    } else if (location[0] == '#') {
        prefix_len = authority_end + strcspn(host + host_len, "#");
    } else if (location[0] == '/' && location[1] == '/') {
        prefix_len = scheme_len + 1; /* "https:" + "//host/x" */
    } else if (location[0] == '/') {
        prefix_len = authority_end;
    } else {
        /* Relative path: keep the base up to its last '/' (before ? or #). */
        prefix_len = authority_end;
        for (size_t i = path_end; i > prefix_len; i--) {
            if (base[i - 1] == '/') {
                prefix_len = i;
                break;
            }
        }
        if (prefix_len == authority_end) {
            /* No path in the base: "http://host" + "/" + relative */
            size_t len = prefix_len + 1 + strlen(location) + 1;
            char  *out = dlmalloc(len);
            if (out)
                snprintf(out, len, "%.*s/%s", (int)prefix_len, base, location);
            return out;
        }
    }
    size_t len = prefix_len + strlen(location) + 1;
    char  *out = dlmalloc(len);
    if (out)
        snprintf(out, len, "%.*s%s", (int)prefix_len, base, location);
    return out;
}

/* Map an esp_http_client error to a curl code, using the transport's last
 * TLS/socket error when there is one. */
static CURLcode curl_map_error(curl_handle_t *curl, esp_err_t err, CURLcode fallback) {
    int       tls_code  = 0;
    int       tls_flags = 0;
    esp_err_t last      = esp_http_client_get_and_clear_last_tls_error(curl->esp_client, &tls_code, &tls_flags);

    if (curl->verbose) {
        ESP_LOGI(TAG, "transfer error %s (tls: %s, code -0x%x, flags 0x%x)", esp_err_to_name(err),
                 esp_err_to_name(last), -tls_code, tls_flags);
    }

    switch (last) {
        case ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME: return CURLE_COULDNT_RESOLVE_HOST;
        case ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT:
        case ESP_ERR_ESP_TLS_SERVER_HANDSHAKE_TIMEOUT: return CURLE_OPERATION_TIMEDOUT;
        case ESP_ERR_ESP_TLS_CANNOT_CREATE_SOCKET:
        case ESP_ERR_ESP_TLS_UNSUPPORTED_PROTOCOL_FAMILY:
        case ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST: return CURLE_COULDNT_CONNECT;
        case ESP_ERR_ESP_TLS_TCP_CLOSED_FIN: return CURLE_RECV_ERROR;
        case ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED:
            /* Non-zero flags: the certificate did not verify. */
            return tls_flags ? CURLE_SSL_PEER_CERTIFICATE : CURLE_SSL_CONNECT_ERROR;
        case ESP_ERR_MBEDTLS_CERT_PARTLY_OK:
        case ESP_ERR_MBEDTLS_CTR_DRBG_SEED_FAILED:
        case ESP_ERR_MBEDTLS_SSL_SET_HOSTNAME_FAILED:
        case ESP_ERR_MBEDTLS_SSL_CONFIG_DEFAULTS_FAILED:
        case ESP_ERR_MBEDTLS_SSL_CONF_ALPN_PROTOCOLS_FAILED:
        case ESP_ERR_MBEDTLS_X509_CRT_PARSE_FAILED:
        case ESP_ERR_MBEDTLS_SSL_CONF_OWN_CERT_FAILED:
        case ESP_ERR_MBEDTLS_SSL_SETUP_FAILED:
        case ESP_ERR_MBEDTLS_SSL_WRITE_FAILED: return CURLE_SSL_CONNECT_ERROR;
        default: break;
    }

    switch (err) {
        case ESP_ERR_TIMEOUT:
        case -ESP_ERR_HTTP_EAGAIN:
        case ESP_ERR_HTTP_EAGAIN: return CURLE_OPERATION_TIMEDOUT;
        case ESP_ERR_HTTP_CONNECT: return CURLE_COULDNT_CONNECT;
        case ESP_ERR_HTTP_WRITE_DATA: return CURLE_SEND_ERROR;
        case ESP_ERR_HTTP_FETCH_HEADER:
        case ESP_ERR_HTTP_CONNECTION_CLOSED: return CURLE_RECV_ERROR;
        case ESP_ERR_HTTP_INVALID_TRANSPORT: return CURLE_UNSUPPORTED_PROTOCOL;
        case ESP_ERR_NO_MEM: return CURLE_OUT_OF_MEMORY;
        default: return fallback;
    }
}

static void curl_close_connection(curl_handle_t *curl) {
    if (curl->esp_client)
        esp_http_client_close(curl->esp_client);
    curl->conn_alive = false;
    dlfree(curl->conn_origin);
    curl->conn_origin = NULL;
}

/* Progress call; true = the application asked to stop. */
static bool curl_progress(curl_handle_t *curl, int64_t ultotal, int64_t ulnow) {
    if (curl->noprogress || !curl->xferinfo_function)
        return false;
    int64_t total = curl->content_length > 0 ? curl->content_length : 0;
    return curl->xferinfo_function(curl->xferinfo_data, total, curl->size_download, ultotal, ulnow) != 0;
}

/* Hand body bytes to the write callback; false = it took fewer bytes. */
static bool curl_deliver(curl_handle_t *curl, char *data, size_t len) {
    if (!len || !curl->write_function)
        return true;
    return curl->write_function(data, 1, len, curl->write_data) == len;
}

/* Run deflate data through the ROM inflater; output goes to the write callback. */
static CURLcode curl_inflate_run(curl_handle_t *curl, curl_inflate_t *z, uint8_t const *in, size_t len) {
    while (!z->done) {
        size_t    in_size  = len;
        size_t    out_size = TINFL_LZ_DICT_SIZE - z->dict_ofs;
        mz_uint32 flags    = TINFL_FLAG_HAS_MORE_INPUT | (z->zlib ? TINFL_FLAG_PARSE_ZLIB_HEADER : 0);
        tinfl_status status =
            tinfl_decompress(&z->decomp, in, &in_size, z->dict, z->dict + z->dict_ofs, &out_size, flags);
        in  += in_size;
        len -= in_size;
        if (out_size) {
            if (!curl_deliver(curl, (char *)z->dict + z->dict_ofs, out_size))
                return CURLE_WRITE_ERROR;
            z->dict_ofs = (z->dict_ofs + out_size) & (TINFL_LZ_DICT_SIZE - 1);
        }
        if (status == TINFL_STATUS_DONE) {
            z->done = true; /* the gzip trailer (CRC, size) is ignored */
            break;
        }
        if (status < 0)
            return CURLE_BAD_CONTENT_ENCODING;
        if (status == TINFL_STATUS_NEEDS_MORE_INPUT && len == 0)
            break;
        if (in_size == 0 && out_size == 0 && status != TINFL_STATUS_HAS_MORE_OUTPUT)
            break; /* no progress possible */
    }
    return CURLE_OK;
}

/* Feed compressed body bytes (gzip or deflate). */
static CURLcode curl_inflate_feed(curl_handle_t *curl, curl_inflate_t *z, uint8_t const *in, size_t len) {
    if (z->encoding == CONTENT_ENCODING_DEFLATE && z->gz_stage < 6) {
        /* "deflate" is meant to be zlib-wrapped, but some servers send raw
         * deflate: look at the first two bytes for a valid zlib header. */
        while (z->gz_pos < 2 && len) {
            z->first[z->gz_pos++] = *in++;
            len--;
        }
        if (z->gz_pos < 2)
            return CURLE_OK;
        z->zlib     = (z->first[0] & 0x0F) == 8 && ((z->first[0] << 8) | z->first[1]) % 31 == 0;
        z->gz_stage = 6;
        CURLcode rc = curl_inflate_run(curl, z, z->first, 2);
        if (rc != CURLE_OK)
            return rc;
    }

    /* gzip member header (RFC 1952) */
    while (len && z->gz_stage < 6) {
        uint8_t c = *in++;
        len--;
        switch (z->gz_stage) {
            case 0:
                if ((z->gz_pos == 0 && c != 0x1F) || (z->gz_pos == 1 && c != 0x8B) || (z->gz_pos == 2 && c != 8))
                    return CURLE_BAD_CONTENT_ENCODING;
                if (z->gz_pos == 3)
                    z->gz_flags = c;
                if (++z->gz_pos == 10) {
                    z->gz_stage = (z->gz_flags & 4) ? 1 : 3;
                    z->gz_pos   = 0;
                }
                break;
            case 1: /* FEXTRA length, little endian */
                z->gz_skip |= (uint32_t)c << (8 * z->gz_pos);
                if (++z->gz_pos == 2)
                    z->gz_stage = z->gz_skip ? 2 : 3;
                break;
            case 2:
                if (--z->gz_skip == 0)
                    z->gz_stage = 3;
                break;
            case 3: /* FNAME, zero terminated */
                if (!(z->gz_flags & 8) || c == 0) {
                    z->gz_stage = 4;
                    if (!(z->gz_flags & 8)) {
                        in--;
                        len++;
                    }
                }
                break;
            case 4: /* FCOMMENT */
                if (!(z->gz_flags & 16) || c == 0) {
                    z->gz_stage = 5;
                    z->gz_skip  = (z->gz_flags & 2) ? 2 : 0;
                    if (!(z->gz_flags & 16)) {
                        in--;
                        len++;
                    }
                }
                break;
            case 5: /* FHCRC */
                if (z->gz_skip == 0) {
                    in--;
                    len++;
                    z->gz_stage = 6;
                } else if (--z->gz_skip == 0) {
                    z->gz_stage = 6;
                }
                break;
        }
    }
    if (z->gz_stage < 6 || !len)
        return CURLE_OK;
    return curl_inflate_run(curl, z, in, len);
}

/* Remove the request headers the previous perform set on a reused client. */
static void curl_forget_request_headers(curl_handle_t *curl) {
    if (!curl->sent_header_keys)
        return;
    char *p = curl->sent_header_keys;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (nl)
            *nl = 0;
        if (*p)
            esp_http_client_delete_header(curl->esp_client, p);
        if (!nl)
            break;
        p = nl + 1;
    }
    dlfree(curl->sent_header_keys);
    curl->sent_header_keys = NULL;
}

static void curl_remember_header_key(curl_handle_t *curl, char const *key) {
    size_t old_len = curl->sent_header_keys ? strlen(curl->sent_header_keys) : 0;
    size_t key_len = strlen(key);
    char  *keys    = dlrealloc(curl->sent_header_keys, old_len + key_len + 2);
    if (!keys)
        return;
    memcpy(keys + old_len, key, key_len);
    keys[old_len + key_len]     = '\n';
    keys[old_len + key_len + 1] = 0;
    curl->sent_header_keys      = keys;
}

static void curl_set_header(curl_handle_t *curl, char const *key, char const *value) {
    if (value && *value) {
        esp_http_client_set_header(curl->esp_client, key, value);
    } else {
        /* "Name:" with no value removes a header, as in libcurl. */
        esp_http_client_delete_header(curl->esp_client, key);
    }
    curl_remember_header_key(curl, key);
}

static void curl_apply_request_headers(curl_handle_t *curl) {
    curl_forget_request_headers(curl);
    curl->auto_content_type = false;

    if (curl->accept_encoding)
        curl_set_header(curl, "Accept-Encoding", curl->accept_encoding);

    bool own_cookie_header = false;
    for (struct curl_slist *header = curl->headers; header; header = header->next) {
        if (!header->data || !strchr(header->data, ':'))
            continue;
        if (strncasecmp(header->data, "Cookie:", 7) == 0)
            own_cookie_header = true;
        char *header_copy = why_strdup(header->data);
        if (!header_copy)
            continue;
        char *colon = strchr(header_copy, ':');
        *colon      = '\0';
        char *value = colon + 1;
        while (*value == ' ' || *value == '\t') value++;
        curl_set_header(curl, header_copy, value);
        dlfree(header_copy);
    }

    curl->own_cookie_header = own_cookie_header;
}

/* Cookie header for this hop (redirects can go to another host). */
static void curl_apply_cookie_header(curl_handle_t *curl, char const *url) {
    if (curl->own_cookie_header)
        return; /* the application sends its own */
    char *cookie_header = build_cookie_header(curl, url, true);
    if (cookie_header) {
        curl_set_header(curl, "Cookie", cookie_header);
        dlfree(cookie_header);
    } else {
        esp_http_client_delete_header(curl->esp_client, "Cookie");
    }
}

static bool curl_method_has_body(esp_http_client_method_t method) {
    return method == HTTP_METHOD_POST || method == HTTP_METHOD_PUT || method == HTTP_METHOD_PATCH;
}

static bool curl_status_is_redirect(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

/* Read and discard a (small) response body so the connection can be reused;
 * closes the connection when that is not possible. */
static void curl_drain_or_close(curl_handle_t *curl, char *buf) {
    int total = 0;
    while (curl->conn_alive && total < CURL_DRAIN_MAX) {
        int n = esp_http_client_read(curl->esp_client, buf, CURL_READ_CHUNK);
        if (n <= 0)
            break;
        total += n;
    }
    if (curl->server_closes || !esp_http_client_is_complete_data_received(curl->esp_client))
        curl_close_connection(curl);
}

/* Wait for the response headers; progress calls every CURL_POLL_MS. */
static CURLcode curl_fetch_headers(curl_handle_t *curl, uint32_t start_ms, int64_t post_len) {
    uint32_t idle_since = curl_now_ms();
    esp_http_client_set_timeout_ms(curl->esp_client, CURL_POLL_MS);
    for (;;) {
        int64_t r = esp_http_client_fetch_headers(curl->esp_client);
        if (r >= 0)
            return CURLE_OK;
        if (r != -ESP_ERR_HTTP_EAGAIN)
            return curl_map_error(curl, ESP_ERR_HTTP_FETCH_HEADER, CURLE_RECV_ERROR);

        uint32_t now = curl_now_ms();
        if (curl_progress(curl, post_len, post_len))
            return CURLE_ABORTED_BY_CALLBACK;
        if ((curl->total_timeout_ms > 0 && (int32_t)(now - start_ms) >= curl->total_timeout_ms) ||
            (int32_t)(now - idle_since) >= curl->idle_timeout_ms)
            return CURLE_OPERATION_TIMEDOUT;
    }
}

/* Send the request and read the response headers, following redirects and
 * a Basic/Digest authentication challenge.  On CURLE_OK the body is ready
 * to be read. */
static CURLcode curl_request(curl_handle_t *curl, uint32_t start_ms, char *buf) {
    char const *url          = curl->config.url;
    bool        retried      = false;
    int         auth_retries = 0;
    esp_http_client_method_t method = curl->config.method;

    for (;;) {
        /* A kept connection can only be reused for the same origin. */
        char *origin = curl_url_origin(url);
        if (curl->conn_alive && (!origin || !curl->conn_origin || strcmp(origin, curl->conn_origin) != 0))
            curl_close_connection(curl);
        if (esp_http_client_set_url(curl->esp_client, url) != ESP_OK) {
            dlfree(origin);
            return CURLE_URL_MALFORMAT;
        }
        esp_http_client_set_method(curl->esp_client, method);
        curl_apply_cookie_header(curl, url);

        bool        reused    = curl->conn_alive;
        int64_t     post_len  = 0;
        char const *post_data = NULL;
        if (curl_method_has_body(method) && curl->post_data) {
            post_data = curl->post_data;
            post_len  = (int64_t)curl->post_data_size;
        }
        /* libcurl's default Content-Type for a request body. */
        char *content_type = NULL;
        esp_http_client_get_header(curl->esp_client, "Content-Type", &content_type);
        if (post_data && !content_type) {
            curl_set_header(curl, "Content-Type", "application/x-www-form-urlencoded");
            curl->auto_content_type = true;
        } else if (!post_data && curl->auto_content_type) {
            esp_http_client_delete_header(curl->esp_client, "Content-Type");
            curl->auto_content_type = false;
        }

        /* Per-response state */
        curl->content_encoding = CONTENT_ENCODING_NONE;
        curl->server_closes    = false;
        curl->content_length   = 0;
        dlfree(curl->location);
        curl->location = NULL;
        dlfree(curl->content_type); /* Content-Type is per response */
        curl->content_type = NULL;
        esp_http_client_get_and_clear_last_tls_error(curl->esp_client, NULL, NULL);

        if (curl_progress(curl, post_len, 0)) {
            dlfree(origin);
            return CURLE_ABORTED_BY_CALLBACK;
        }

        /* Connect (if needed) and send the request line and headers with
         * the configured timeout: DNS, TCP and TLS cannot be interrupted. */
        esp_http_client_set_timeout_ms(curl->esp_client, curl->config.timeout_ms);
        esp_err_t err = esp_http_client_open(curl->esp_client, (int)post_len);
        if (err == ESP_OK && post_len > 0) {
            int written = esp_http_client_write(curl->esp_client, post_data, (int)post_len);
            if (written != (int)post_len)
                err = ESP_ERR_HTTP_WRITE_DATA;
        }

        CURLcode rc = CURLE_OK;
        if (err != ESP_OK) {
            rc = curl_map_error(curl, err, CURLE_COULDNT_CONNECT);
        } else {
            curl->conn_alive = true;
            dlfree(curl->conn_origin);
            curl->conn_origin = origin;
            origin            = NULL;
            rc                = curl_fetch_headers(curl, start_ms, post_len);
        }
        dlfree(origin);

        if (rc != CURLE_OK) {
            curl_close_connection(curl);
            /* A kept connection may have been closed by the server while it
             * was idle: try once more on a new connection. */
            if (reused && !retried && rc != CURLE_ABORTED_BY_CALLBACK && rc != CURLE_OPERATION_TIMEDOUT) {
                retried = true;
                if (curl->verbose)
                    ESP_LOGI(TAG, "kept connection was closed, reconnecting");
                continue;
            }
            return rc;
        }
        retried = false;

        int status           = esp_http_client_get_status_code(curl->esp_client);
        curl->response_code  = status;
        curl->content_length = esp_http_client_get_content_length(curl->esp_client);

        if (curl->follow_location && curl_status_is_redirect(status) && curl->location) {
            if (curl->redirect_count >= curl->max_redirs)
                return CURLE_TOO_MANY_REDIRECTS;
            char *next = curl_resolve_location(curl->effective_url, curl->location);
            if (!next)
                return CURLE_OUT_OF_MEMORY;
            if (strncasecmp(next, "http://", 7) != 0 && strncasecmp(next, "https://", 8) != 0) {
                dlfree(next);
                return CURLE_UNSUPPORTED_PROTOCOL;
            }
            curl_drain_or_close(curl, buf);
            /* 303, and 301/302 after a POST, continue with GET (as browsers
             * and libcurl do); 307/308 repeat the request as it was. */
            if (status == 303 || ((status == 301 || status == 302) && method == HTTP_METHOD_POST))
                method = HTTP_METHOD_GET;
            if (curl->verbose)
                ESP_LOGI(TAG, "redirect %d -> %s", status, next);
            dlfree(curl->effective_url);
            curl->effective_url = next;
            url                 = next;
            curl->redirect_count++;
            continue;
        }

        if (status == 401 && curl->config.username && auth_retries == 0) {
            if (esp_http_client_add_auth(curl->esp_client) == ESP_OK) {
                auth_retries++;
                curl_drain_or_close(curl, buf);
                continue;
            }
        }
        return CURLE_OK;
    }
}

/* Read the response body into the write callback. */
static CURLcode curl_read_body(curl_handle_t *curl, uint32_t start_ms, char *buf) {
    curl_inflate_t *z = NULL;
    if (curl->accept_encoding && curl->content_encoding != CONTENT_ENCODING_NONE) {
        z = dlcalloc(1, sizeof(curl_inflate_t));
        if (!z)
            return CURLE_OUT_OF_MEMORY;
        tinfl_init(&z->decomp);
        z->encoding = curl->content_encoding;
        z->gz_stage = 0;
    }

    CURLcode rc         = CURLE_OK;
    uint32_t idle_since = curl_now_ms();
    uint32_t next_call  = 0;
    esp_http_client_set_timeout_ms(curl->esp_client, CURL_POLL_MS);

    for (;;) {
        int n = esp_http_client_read(curl->esp_client, buf, CURL_READ_CHUNK);
        uint32_t now = curl_now_ms();

        if (n > 0) {
            idle_since = now;
            curl->size_download += n;
            rc = z ? curl_inflate_feed(curl, z, (uint8_t const *)buf, (size_t)n)
                   : (curl_deliver(curl, buf, (size_t)n) ? CURLE_OK : CURLE_WRITE_ERROR);
            if (rc != CURLE_OK)
                break;
        } else if (n == 0) {
            if (!esp_http_client_is_complete_data_received(curl->esp_client) && curl->content_length > 0 &&
                curl->size_download < curl->content_length) {
                ESP_LOGW(TAG, "connection closed after %lld of %lld bytes", curl->size_download, curl->content_length);
                rc = CURLE_PARTIAL_FILE;
            }
            break;
        } else if (n != -ESP_ERR_HTTP_EAGAIN) {
            rc = curl_map_error(curl, ESP_FAIL, CURLE_RECV_ERROR);
            break;
        }

        /* Progress at least every CURL_POLL_MS, and on every chunk. */
        if (n > 0 || (int32_t)(now - next_call) >= 0) {
            next_call = now + CURL_POLL_MS;
            if (curl_progress(curl, 0, 0)) {
                rc = CURLE_ABORTED_BY_CALLBACK;
                break;
            }
        }
        if ((curl->total_timeout_ms > 0 && (int32_t)(now - start_ms) >= curl->total_timeout_ms) ||
            (int32_t)(now - idle_since) >= curl->idle_timeout_ms) {
            rc = CURLE_OPERATION_TIMEDOUT;
            break;
        }
    }

    if (z) {
        if (rc == CURLE_OK && !z->done) {
            ESP_LOGW(TAG, "compressed body ended early");
            rc = CURLE_BAD_CONTENT_ENCODING;
        }
        dlfree(z);
    }

    /* Keep the connection only after a complete, cleanly ended response. */
    if (rc != CURLE_OK || curl->server_closes || !esp_http_client_is_complete_data_received(curl->esp_client))
        curl_close_connection(curl);
    return rc;
}

CURLcode curl_easy_perform(CURL *curl_handle) {
    if (!curl_handle) {
        return CURLE_FAILED_INIT;
    }

    curl_handle_t *curl = (curl_handle_t *)curl_handle;
    if (!curl->config.url || !curl->config.url[0]) {
        return CURLE_URL_MALFORMAT;
    }

    /* Per-transfer results */
    curl->response_code  = 0;
    curl->content_length = 0;
    curl->size_download  = 0;
    curl->redirect_count = 0;
    dlfree(curl->effective_url);
    curl->effective_url = why_strdup(curl->config.url);

    /* esp_http_client reads TLS, authentication and buffer settings only
     * in esp_http_client_init(): start a new client when one changed. */
    if (curl->esp_client && curl->recreate_client) {
        curl_close_connection(curl);
        esp_http_client_cleanup(curl->esp_client);
        curl->esp_client = NULL;
        dlfree(curl->sent_header_keys);
        curl->sent_header_keys = NULL;
    }
    if (!curl->esp_client) {
        curl->esp_client = esp_http_client_init(&curl->config);
        if (!curl->esp_client) {
            return CURLE_FAILED_INIT;
        }
        curl->conn_alive      = false;
        curl->recreate_client = false;
    }

    char *buf = dlmalloc(CURL_READ_CHUNK);
    if (!buf) {
        return CURLE_OUT_OF_MEMORY;
    }

    uint32_t start_ms = curl_now_ms();
    esp_http_client_reset_redirect_counter(curl->esp_client); /* counts authentication retries */
    curl_apply_request_headers(curl);

    CURLcode rc = curl_request(curl, start_ms, buf);
    if (rc == CURLE_OK && curl->config.method != HTTP_METHOD_HEAD) {
        rc = curl_read_body(curl, start_ms, buf);
    } else if (rc == CURLE_OK) {
        if (curl->server_closes)
            curl_close_connection(curl);
    } else {
        curl_close_connection(curl);
    }
    dlfree(buf);

    if (curl->verbose) {
        ESP_LOGI(TAG, "%s: rc=%d status=%d bytes=%lld redirects=%ld keep=%d", curl->effective_url, rc,
                 curl->response_code, curl->size_download, curl->redirect_count, curl->conn_alive);
    }

    if (curl->cookie_jar) {
        save_cookies_to_file(curl, curl->cookie_jar);
    }

    return rc;
}

void curl_easy_cleanup(CURL *curl_handle) {
    if (!curl_handle) {
        return;
    }

    curl_handle_t *curl = (curl_handle_t *)curl_handle;

    dlfree((void *)curl->config.url);
    dlfree((void *)curl->config.user_agent);
    dlfree((void *)curl->config.username);
    dlfree((void *)curl->config.password);
    dlfree((void *)curl->config.cert_pem);
    dlfree(curl->post_data);
    dlfree(curl->content_type);
    dlfree(curl->effective_url);
    dlfree(curl->accept_encoding);
    dlfree(curl->location);
    dlfree(curl->conn_origin);
    dlfree(curl->sent_header_keys);

    free_all_cookies(curl->cookies);
    dlfree(curl->cookie_file);
    dlfree(curl->cookie_jar);
    dlfree(curl->manual_cookies);

    dlfree(curl->proxy_url);
    dlfree(curl->proxy_userpwd);

    if (curl->esp_client) {
        esp_http_client_cleanup(curl->esp_client);
    }

    dlfree(curl);
}

CURLcode curl_easy_getinfo(CURL *curl_handle, curl_easy_info_t info, ...) {
    if (!curl_handle) {
        return CURLE_FAILED_INIT;
    }

    curl_handle_t *curl = (curl_handle_t *)curl_handle;
    va_list        args;
    va_start(args, info);

    switch (info) {
        case CURLINFO_RESPONSE_CODE: {
            long *code = va_arg(args, long *);
            *code      = curl->response_code;
            break;
        }

        case CURLINFO_CONTENT_LENGTH_DOWNLOAD: {
            double *length = va_arg(args, double *);
            *length        = (double)curl->content_length;
            break;
        }

        case CURLINFO_CONTENT_TYPE: {
            char **type = va_arg(args, char **);
            *type       = curl->content_type;
            break;
        }

        case CURLINFO_EFFECTIVE_URL: {
            char **url = va_arg(args, char **);
            *url       = curl->effective_url ? curl->effective_url : (char *)curl->config.url;
            break;
        }

        case CURLINFO_SIZE_DOWNLOAD: {
            double *size = va_arg(args, double *);
            *size        = (double)curl->size_download;
            break;
        }

        case CURLINFO_REDIRECT_COUNT: {
            long *count = va_arg(args, long *);
            *count      = curl->redirect_count;
            break;
        }

        default: va_end(args); return CURLE_UNSUPPORTED_PROTOCOL;
    }

    va_end(args);
    return CURLE_OK;
}

char const *curl_easy_strerror(CURLcode error) {
    switch (error) {
        case CURLE_OK: return "No error";
        case CURLE_UNSUPPORTED_PROTOCOL: return "Unsupported protocol";
        case CURLE_FAILED_INIT: return "Failed initialization";
        case CURLE_URL_MALFORMAT: return "URL malformat";
        case CURLE_COULDNT_RESOLVE_HOST: return "Couldn't resolve host";
        case CURLE_COULDNT_CONNECT: return "Couldn't connect";
        case CURLE_HTTP_RETURNED_ERROR: return "HTTP returned error";
        case CURLE_OPERATION_TIMEDOUT: return "Operation timed out";
        case CURLE_SSL_CONNECT_ERROR: return "SSL connect error";
        case CURLE_SSL_PEER_CERTIFICATE: return "SSL peer certificate was not OK";
        case CURLE_WEIRD_SERVER_REPLY: return "Weird server reply";
        case CURLE_WRITE_ERROR: return "Failed writing received data";
        case CURLE_OUT_OF_MEMORY: return "Out of memory";
        case CURLE_SEND_ERROR: return "Failed sending data to the peer";
        case CURLE_RECV_ERROR: return "Failure when receiving data from the peer";
        case CURLE_BAD_CONTENT_ENCODING: return "Unrecognized or bad HTTP Content or Transfer-Encoding";
        case CURLE_FILESIZE_EXCEEDED: return "Maximum file size exceeded";
        case CURLE_LOGIN_DENIED: return "Login denied";
        case CURLE_ABORTED_BY_CALLBACK: return "Operation was aborted by an application callback";
        case CURLE_TOO_MANY_REDIRECTS: return "Number of redirects hit maximum amount";
        case CURLE_PARTIAL_FILE: return "Transferred a partial file";
        default: return "Unknown error";
    }
}

struct curl_slist *curl_slist_append(struct curl_slist *list, char const *string) {
    struct curl_slist *new_node = dlmalloc(sizeof(struct curl_slist));
    if (!new_node) {
        return list;
    }

    new_node->data = why_strdup(string);
    new_node->next = NULL;

    if (!list) {
        return new_node;
    }

    struct curl_slist *current = list;
    while (current->next) {
        current = current->next;
    }
    current->next = new_node;

    return list;
}

void curl_slist_free_all(struct curl_slist *list) {
    while (list) {
        struct curl_slist *next = list->next;
        dlfree(list->data);
        dlfree(list);
        list = next;
    }
}

CURLcode curl_global_init(long flags) {
    return CURLE_OK;
}

void curl_global_cleanup(void) {
}
