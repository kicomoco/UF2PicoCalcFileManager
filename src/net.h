#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    /* Called once headers of the final (non-redirect) 200 response are parsed. */
    void (*on_start)(void *ctx, int64_t content_length);
    /* Called with body data. Return false to abort the transfer. */
    bool (*on_body)(void *ctx, const uint8_t *data, size_t len);
    void *ctx;
} http_cb_t;

/* All functions expect a ui_progress dialog to be active (they tick the spinner). */
int net_wifi_connect(const char *ssid, const char *pass, char *err, size_t errlen);
int net_wifi_scan(char ssids[][33], int max, char *err, size_t errlen);
void net_wifi_off(void);
/* Performs an HTTPS GET, following redirects. Returns HTTP status (200 on success) or <0 on error. */
int net_https_get(const char *url, const char *accept, const http_cb_t *cb, char *err, size_t errlen);
