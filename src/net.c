#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include "lwip/altcp.h"
#include "lwip/altcp_tls.h"
#include "mbedtls/ssl.h"
#include "ui.h"
#include "net.h"
#include "ca_certs.h"

#ifndef FM_VERSION
#define FM_VERSION "0.0.0"
#endif

#define CONNECT_TIMEOUT_MS 30000
#define IDLE_TIMEOUT_MS 20000
#define MAX_REDIRECTS 5
#define HDR_LINE_MAX 4096

static bool s_init;

static void seterr(char *err, size_t n, const char *msg) {
    if (err && n) snprintf(err, n, "%s", msg);
}

static bool wifi_init(char *err, size_t errlen) {
    if (s_init) return true;
    if (cyw43_arch_init_with_country(CYW43_COUNTRY_WORLDWIDE)) {
        seterr(err, errlen, "Wi-Fi chip init failed");
        return false;
    }
    cyw43_arch_enable_sta_mode();
    s_init = true;
    return true;
}

void net_wifi_off(void) {
    if (!s_init) return;
    cyw43_arch_deinit();
    s_init = false;
}

/* Returns true if the user cancelled. */
static bool poll_step(void) {
    cyw43_arch_poll();
    ui_progress_tick();
    return ui_progress_cancelled();
}

int net_wifi_connect(const char *ssid, const char *pass, char *err, size_t errlen) {
    if (!ssid || !*ssid) {
        seterr(err, errlen, "No Wi-Fi network configured");
        return -1;
    }
    if (!wifi_init(err, errlen)) return -1;
    if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP) return 0;

    ui_progress_set("Connecting to Wi-Fi", ssid);
    uint32_t auth = (pass && *pass) ? CYW43_AUTH_WPA2_MIXED_PSK : CYW43_AUTH_OPEN;
    if (cyw43_arch_wifi_connect_async(ssid, (pass && *pass) ? pass : NULL, auth)) {
        seterr(err, errlen, "Wi-Fi connect failed");
        return -1;
    }
    absolute_time_t deadline = make_timeout_time_ms(CONNECT_TIMEOUT_MS);
    for (;;) {
        if (poll_step()) {
            seterr(err, errlen, "Cancelled");
            return -2;
        }
        int st = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (st == CYW43_LINK_UP) break;
        if (st == CYW43_LINK_BADAUTH) { seterr(err, errlen, "Wrong Wi-Fi password"); return -1; }
        if (st == CYW43_LINK_NONET) { seterr(err, errlen, "Wi-Fi network not found"); return -1; }
        if (st == CYW43_LINK_FAIL) { seterr(err, errlen, "Wi-Fi connection failed"); return -1; }
        if (time_reached(deadline)) { seterr(err, errlen, "Wi-Fi connect timed out"); return -1; }
        sleep_ms(2);
    }
    return 0;
}

typedef struct {
    char (*ssids)[33];
    int max, n;
} scan_t;

static int scan_cb(void *env, const cyw43_ev_scan_result_t *r) {
    scan_t *s = env;
    if (!r || !r->ssid_len || s->n >= s->max) return 0;
    char name[33];
    int l = r->ssid_len > 32 ? 32 : r->ssid_len;
    memcpy(name, r->ssid, l);
    name[l] = 0;
    for (int i = 0; i < s->n; i++)
        if (!strcmp(s->ssids[i], name)) return 0;
    strcpy(s->ssids[s->n++], name);
    return 0;
}

int net_wifi_scan(char ssids[][33], int max, char *err, size_t errlen) {
    if (!wifi_init(err, errlen)) return -1;
    scan_t s = {ssids, max, 0};
    cyw43_wifi_scan_options_t opts = {0};
    ui_progress_set("Scanning for networks", "");
    if (cyw43_wifi_scan(&cyw43_state, &opts, &s, scan_cb)) {
        seterr(err, errlen, "Wi-Fi scan failed");
        return -1;
    }
    absolute_time_t deadline = make_timeout_time_ms(15000);
    while (cyw43_wifi_scan_active(&cyw43_state)) {
        if (poll_step()) { seterr(err, errlen, "Cancelled"); return -2; }
        if (time_reached(deadline)) break;
        sleep_ms(2);
    }
    return s.n;
}

/* ---------------------------------------------------------------- HTTPS */

enum { ST_STATUS, ST_HEADERS, ST_BODY, ST_CHUNK_SIZE, ST_CHUNK_DATA, ST_CHUNK_END, ST_TRAILER, ST_DONE };

typedef struct {
    struct altcp_pcb *pcb;
    const http_cb_t *cb;
    const char *host, *path, *accept;
    bool connected, closed, done, failed;
    char errmsg[64];
    int state;
    char *line;
    int linelen;
    int status;
    int64_t clen, remaining;
    bool chunked;
    char *location;
    absolute_time_t last_activity;
    int64_t received;
} req_t;

static void req_fail(req_t *r, const char *msg) {
    if (r->failed || r->done) return;
    r->failed = true;
    snprintf(r->errmsg, sizeof(r->errmsg), "%s", msg);
}

static bool is_redirect(int s) {
    return s == 301 || s == 302 || s == 303 || s == 307 || s == 308;
}

static void headers_done(req_t *r) {
    if (r->status != 200) {
        r->state = ST_DONE;
        r->done = true;
        return;
    }
    if (r->cb && r->cb->on_start) r->cb->on_start(r->cb->ctx, r->chunked ? -1 : r->clen);
    if (r->chunked) r->state = ST_CHUNK_SIZE;
    else {
        r->state = ST_BODY;
        r->remaining = r->clen;
        if (r->clen == 0) { r->state = ST_DONE; r->done = true; }
    }
}

static void handle_line(req_t *r) {
    char *l = r->line;
    switch (r->state) {
    case ST_STATUS: {
        char *sp = strchr(l, ' ');
        if (strncmp(l, "HTTP/", 5) || !sp) { req_fail(r, "Bad HTTP response"); return; }
        r->status = atoi(sp + 1);
        r->state = ST_HEADERS;
        break;
    }
    case ST_HEADERS:
        if (!*l) { headers_done(r); break; }
        if (!strncasecmp(l, "Content-Length:", 15)) r->clen = strtoll(l + 15, NULL, 10);
        else if (!strncasecmp(l, "Transfer-Encoding:", 18) && strcasestr(l + 18, "chunked")) r->chunked = true;
        else if (!strncasecmp(l, "Location:", 9)) {
            const char *v = l + 9;
            while (*v == ' ') v++;
            free(r->location);
            r->location = strdup(v);
        }
        break;
    case ST_CHUNK_SIZE: {
        long long n = strtoll(l, NULL, 16);
        if (n == 0) r->state = ST_TRAILER;
        else { r->remaining = n; r->state = ST_CHUNK_DATA; }
        break;
    }
    case ST_CHUNK_END:
        r->state = ST_CHUNK_SIZE;
        break;
    case ST_TRAILER:
        if (!*l) { r->state = ST_DONE; r->done = true; }
        break;
    }
}

static void feed(req_t *r, const uint8_t *d, size_t n) {
    while (n && !r->done && !r->failed) {
        if (r->state == ST_BODY || r->state == ST_CHUNK_DATA) {
            size_t take = n;
            if (r->remaining >= 0 && (int64_t)take > r->remaining) take = (size_t)r->remaining;
            if (r->status == 200 && r->cb && r->cb->on_body && !r->cb->on_body(r->cb->ctx, d, take)) {
                req_fail(r, "Aborted");
                return;
            }
            r->received += take;
            d += take;
            n -= take;
            if (r->remaining >= 0) {
                r->remaining -= take;
                if (r->remaining == 0) {
                    if (r->state == ST_CHUNK_DATA) r->state = ST_CHUNK_END;
                    else { r->state = ST_DONE; r->done = true; }
                }
            }
            continue;
        }
        char c = (char)*d++;
        n--;
        if (c == '\r') continue;
        if (c == '\n') {
            r->line[r->linelen] = 0;
            handle_line(r);
            r->linelen = 0;
            continue;
        }
        if (r->linelen < HDR_LINE_MAX - 1) r->line[r->linelen++] = c;
    }
}

static err_t on_recv(void *arg, struct altcp_pcb *pcb, struct pbuf *p, err_t err) {
    req_t *r = arg;
    if (!p) {
        r->closed = true;
        if (!r->done && !r->failed) {
            if (r->state == ST_BODY && r->clen < 0) r->done = true;
            else req_fail(r, "Connection closed early");
        }
        return ERR_OK;
    }
    r->last_activity = get_absolute_time();
    for (struct pbuf *q = p; q; q = q->next) feed(r, q->payload, q->len);
    altcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void on_err(void *arg, err_t err) {
    req_t *r = arg;
    r->pcb = NULL;
    char m[40];
    snprintf(m, sizeof(m), "Connection error %d", err);
    req_fail(r, m);
}

static err_t on_connected(void *arg, struct altcp_pcb *pcb, err_t err) {
    req_t *r = arg;
    if (err != ERR_OK) { req_fail(r, "Connect failed"); return ERR_OK; }
    r->connected = true;
    r->last_activity = get_absolute_time();
    char *q = malloc(strlen(r->path) + strlen(r->host) + 256);
    if (!q) { req_fail(r, "Out of memory"); return ERR_OK; }
    int len = sprintf(q,
                      "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: PicoCalc-FM/" FM_VERSION "\r\n"
                      "Accept: %s\r\nConnection: close\r\n\r\n",
                      r->path, r->host, r->accept ? r->accept : "*/*");
    err_t e = altcp_write(pcb, q, len, TCP_WRITE_FLAG_COPY);
    if (e == ERR_OK) e = altcp_output(pcb);
    free(q);
    if (e != ERR_OK) req_fail(r, "Send failed");
    return ERR_OK;
}

/* DNS state is static: a lookup abandoned on cancel/timeout may still call back later.
 * The generation number makes stale callbacks harmless. */
typedef struct { bool done; bool ok; ip_addr_t addr; uint32_t gen; } dns_t;
static dns_t s_dns;

static void on_dns(const char *name, const ip_addr_t *ip, void *arg) {
    if ((uint32_t)(uintptr_t)arg != s_dns.gen) return;
    if (ip) { s_dns.addr = *ip; s_dns.ok = true; }
    s_dns.done = true;
}

static void close_pcb(req_t *r) {
    if (!r->pcb) return;
    altcp_arg(r->pcb, NULL);
    altcp_recv(r->pcb, NULL);
    altcp_err(r->pcb, NULL);
    if (altcp_close(r->pcb) != ERR_OK) altcp_abort(r->pcb);
    r->pcb = NULL;
}

/* Splits https://host[:port]/path. Returns false if not https. */
static bool parse_url(const char *url, char *host, size_t hn, int *port, const char **path) {
    if (strncasecmp(url, "https://", 8)) return false;
    const char *h = url + 8;
    const char *slash = strchr(h, '/');
    const char *end = slash ? slash : h + strlen(h);
    const char *colon = memchr(h, ':', end - h);
    size_t l = (colon ? colon : end) - h;
    if (!l || l >= hn) return false;
    memcpy(host, h, l);
    host[l] = 0;
    *port = colon ? atoi(colon + 1) : 443;
    *path = slash ? slash : "/";
    return true;
}

static int do_get(const char *url, const char *accept, const http_cb_t *cb, char **location, char *err, size_t errlen) {
    char host[128];
    int port;
    const char *path;
    if (!parse_url(url, host, sizeof(host), &port, &path)) {
        seterr(err, errlen, "Unsupported URL");
        return -1;
    }

    char l2[40];
    snprintf(l2, sizeof(l2), "%.39s", host);
    ui_progress_set("Resolving", l2);
    dns_t d;
    uint32_t gen = s_dns.gen + 1;
    memset(&s_dns, 0, sizeof(s_dns));
    s_dns.gen = gen;
    cyw43_arch_lwip_begin();
    err_t e = dns_gethostbyname(host, &s_dns.addr, on_dns, (void *)(uintptr_t)gen);
    cyw43_arch_lwip_end();
    if (e == ERR_OK) s_dns.done = s_dns.ok = true;
    else if (e != ERR_INPROGRESS) { seterr(err, errlen, "DNS lookup failed"); return -1; }
    absolute_time_t deadline = make_timeout_time_ms(15000);
    while (!s_dns.done) {
        if (poll_step()) { s_dns.gen++; seterr(err, errlen, "Cancelled"); return -2; }
        if (time_reached(deadline)) { s_dns.gen++; seterr(err, errlen, "DNS timed out"); return -1; }
        sleep_ms(1);
    }
    if (!s_dns.ok) { seterr(err, errlen, "Host not found"); return -1; }
    d = s_dns;

    req_t *r = calloc(1, sizeof(req_t));
    char *line = malloc(HDR_LINE_MAX);
    struct altcp_tls_config *tls = NULL;
    int ret = -1;
    if (!r || !line) { seterr(err, errlen, "Out of memory"); goto out; }
    r->line = line;
    r->cb = cb;
    r->host = host;
    r->path = path;
    r->accept = accept;
    r->clen = -1;
    r->remaining = -1;

    tls = altcp_tls_create_config_client((const u8_t *)GITHUB_CA_CERTS, sizeof(GITHUB_CA_CERTS));
    if (!tls) { seterr(err, errlen, "TLS setup failed (memory?)"); goto out; }
    r->pcb = altcp_tls_new(tls, IP_GET_TYPE(&d.addr));
    if (!r->pcb) { seterr(err, errlen, "TLS socket failed"); goto out; }
    mbedtls_ssl_set_hostname(altcp_tls_context(r->pcb), host);
    altcp_arg(r->pcb, r);
    altcp_recv(r->pcb, on_recv);
    altcp_err(r->pcb, on_err);

    ui_progress_set("Connecting", l2);
    r->last_activity = get_absolute_time();
    cyw43_arch_lwip_begin();
    e = altcp_connect(r->pcb, &d.addr, port, on_connected);
    cyw43_arch_lwip_end();
    if (e != ERR_OK) { seterr(err, errlen, "Connect failed"); goto out; }

    bool shown_dl = false;
    while (!r->done && !r->failed) {
        if (poll_step()) { req_fail(r, "Cancelled"); ret = -2; break; }
        if (r->connected && !shown_dl && r->status) {
            ui_progress_set("Downloading", l2);
            shown_dl = true;
        }
        if (r->closed && !r->done) { req_fail(r, "Connection closed"); break; }
        if (absolute_time_diff_us(r->last_activity, get_absolute_time()) > (int64_t)(r->connected ? IDLE_TIMEOUT_MS : CONNECT_TIMEOUT_MS) * 1000) {
            req_fail(r, "Timed out");
            break;
        }
    }
    if (r->failed) {
        seterr(err, errlen, r->errmsg);
        if (ret != -2) ret = -1;
    } else {
        ret = r->status;
        if (is_redirect(r->status) && r->location) {
            *location = r->location;
            r->location = NULL;
        }
    }
out:
    if (r) {
        close_pcb(r);
        /* Let lwIP finish the close handshake before freeing the TLS config. */
        for (int i = 0; i < 20; i++) { cyw43_arch_poll(); sleep_ms(1); }
        free(r->location);
        free(r);
    }
    if (tls) altcp_tls_free_config(tls);
    free(line);
    return ret;
}

int net_https_get(const char *url, const char *accept, const http_cb_t *cb, char *err, size_t errlen) {
    char *cur = strdup(url);
    if (!cur) { seterr(err, errlen, "Out of memory"); return -1; }
    int st = -1;
    for (int i = 0; i <= MAX_REDIRECTS; i++) {
        char *loc = NULL;
        st = do_get(cur, accept, cb, &loc, err, errlen);
        if (!loc) break;
        if (loc[0] == '/') {
            /* Relative redirect: keep scheme and host. */
            const char *h = cur + 8;
            const char *slash = strchr(h, '/');
            size_t base = slash ? (size_t)(slash - cur) : strlen(cur);
            char *nu = malloc(base + strlen(loc) + 1);
            if (!nu) { free(loc); seterr(err, errlen, "Out of memory"); st = -1; break; }
            memcpy(nu, cur, base);
            strcpy(nu + base, loc);
            free(loc);
            loc = nu;
        }
        free(cur);
        cur = loc;
        if (i == MAX_REDIRECTS) { seterr(err, errlen, "Too many redirects"); st = -1; }
    }
    free(cur);
    if (st > 0 && st != 200 && err && errlen && !is_redirect(st)) snprintf(err, errlen, "HTTP error %d", st);
    return st;
}
