#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ff.h"
#include "config.h"

config_t g_cfg;

static void set_defaults(void) {
    memset(&g_cfg, 0, sizeof(g_cfg));
    strcpy(g_cfg.path_a, "/");
    strcpy(g_cfg.path_b, "/");
}

static void apply(const char *key, const char *val) {
#define STR(k, field) if (!strcmp(key, k)) { strncpy(g_cfg.field, val, sizeof(g_cfg.field) - 1); return; }
    STR("wifi_ssid", wifi_ssid);
    STR("wifi_pass", wifi_pass);
    STR("path_a", path_a);
    STR("path_b", path_b);
#undef STR
    if (!strcmp(key, "split_view")) g_cfg.split_view = atoi(val) != 0;
    else if (!strcmp(key, "sort_mode")) g_cfg.sort_mode = atoi(val);
    else if (!strcmp(key, "show_hidden")) g_cfg.show_hidden = atoi(val) != 0;
}

void config_load(void) {
    set_defaults();
    FIL f;
    if (f_open(&f, CONFIG_PATH, FA_READ) != FR_OK) return;
    char line[700];
    size_t n = 0;
    for (;;) {
        char ch;
        UINT br;
        bool eof = f_read(&f, &ch, 1, &br) != FR_OK || br == 0;
        if (eof || ch == '\n') {
            line[n] = 0;
            if (n && line[n - 1] == '\r') line[--n] = 0;
            char *eq = strchr(line, '=');
            if (eq && line[0] != '#') {
                *eq = 0;
                apply(line, eq + 1);
            }
            n = 0;
            if (eof) break;
        } else if (n < sizeof(line) - 1) {
            line[n++] = ch;
        }
    }
    f_close(&f);
    if (!g_cfg.path_a[0]) strcpy(g_cfg.path_a, "/");
    if (!g_cfg.path_b[0]) strcpy(g_cfg.path_b, "/");
}

bool config_save(void) {
    FIL f;
    if (f_open(&f, CONFIG_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
    char buf[1400];
    int len = snprintf(buf, sizeof(buf),
                       "# PicoCalc File Manager settings\n"
                       "wifi_ssid=%s\nwifi_pass=%s\nsplit_view=%d\nsort_mode=%d\nshow_hidden=%d\npath_a=%s\npath_b=%s\n",
                       g_cfg.wifi_ssid, g_cfg.wifi_pass, g_cfg.split_view, g_cfg.sort_mode, g_cfg.show_hidden,
                       g_cfg.path_a, g_cfg.path_b);
    UINT bw;
    FRESULT fr = f_write(&f, buf, len, &bw);
    f_close(&f);
    return fr == FR_OK && (int)bw == len;
}
