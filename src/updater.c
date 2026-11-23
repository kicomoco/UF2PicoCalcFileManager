#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "ff.h"
#include "ui.h"
#include "net.h"
#include "config.h"
#include "util.h"
#include "updater.h"

#ifndef FM_VERSION
#define FM_VERSION "0.0.0"
#endif
#ifndef FM_GITHUB_REPO
#define FM_GITHUB_REPO "kicomoco/UF2PicoCalcFileManager"
#endif

#define JSON_MAX (96 * 1024)

typedef struct {
    char *buf;
    size_t len, cap;
    bool overflow;
} membuf_t;

static bool mem_body(void *ctx, const uint8_t *d, size_t n) {
    membuf_t *m = ctx;
    if (m->len + n + 1 > m->cap) {
        if (m->cap >= JSON_MAX) { m->overflow = true; n = m->cap - m->len - 1; }
        else {
            size_t nc = m->cap ? m->cap * 2 : 8192;
            while (nc < m->len + n + 1) nc *= 2;
            if (nc > JSON_MAX) nc = JSON_MAX;
            char *nb = realloc(m->buf, nc);
            if (!nb) return false;
            m->buf = nb;
            m->cap = nc;
            if (m->len + n + 1 > m->cap) { m->overflow = true; n = m->cap - m->len - 1; }
        }
    }
    memcpy(m->buf + m->len, d, n);
    m->len += n;
    m->buf[m->len] = 0;
    return true;
}

/* Finds "key": "value" starting at p; copies value into out. Returns pointer past it or NULL. */
static const char *json_str(const char *p, const char *key, char *out, size_t on) {
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *k = strstr(p, pat);
    if (!k) return NULL;
    const char *v = k + strlen(pat);
    while (*v == ' ' || *v == ':' || *v == '\t' || *v == '\n' || *v == '\r') v++;
    if (*v != '"') return v;
    v++;
    size_t i = 0;
    while (*v && *v != '"') {
        if (*v == '\\' && v[1]) v++;
        if (i + 1 < on) out[i++] = *v;
        v++;
    }
    out[i] = 0;
    return v;
}

typedef struct {
    FIL f;
    int64_t total, done;
    FRESULT fr;
    uint32_t first[2];
    int nfirst;
} dl_t;

static void dl_start(void *ctx, int64_t len) {
    dl_t *d = ctx;
    d->total = len;
}

static bool dl_body(void *ctx, const uint8_t *data, size_t n) {
    dl_t *d = ctx;
    if (d->nfirst < 8) {
        size_t take = 8 - d->nfirst;
        if (take > n) take = n;
        memcpy((uint8_t *)d->first + d->nfirst, data, take);
        d->nfirst += take;
    }
    UINT bw;
    d->fr = f_write(&d->f, data, n, &bw);
    if (d->fr == FR_OK && bw != n) d->fr = FR_DENIED;
    if (d->fr != FR_OK) return false;
    d->done += n;
    char l1[40], a[12], b[12];
    fmt_size(a, sizeof(a), d->done);
    if (d->total > 0) {
        fmt_size(b, sizeof(b), d->total);
        snprintf(l1, sizeof(l1), "%s / %s (%d%%)", a, b, (int)(d->done * 100 / d->total));
    } else snprintf(l1, sizeof(l1), "%s", a);
    ui_progress_set("Downloading update", l1);
    return true;
}

static bool download(const char *url, const char *dest, char *err, size_t errlen) {
    char tmp[MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s.tmp", dest);
    dl_t *d = calloc(1, sizeof(dl_t));
    if (!d) { snprintf(err, errlen, "Out of memory"); return false; }
    FRESULT fr = f_mkdir(UPDATE_DIR);
    if (fr != FR_OK && fr != FR_EXIST) { snprintf(err, errlen, "mkdir: %s", fr_str(fr)); free(d); return false; }
    fr = f_open(&d->f, tmp, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) { snprintf(err, errlen, "Create: %s", fr_str(fr)); free(d); return false; }
    d->total = -1;
    http_cb_t cb = {dl_start, dl_body, d};
    int st = net_https_get(url, "application/octet-stream", &cb, err, errlen);
    FRESULT cfr = f_close(&d->f);
    bool ok = st == 200;
    if (ok && cfr != FR_OK) { snprintf(err, errlen, "Write: %s", fr_str(cfr)); ok = false; }
    if (ok && d->fr != FR_OK) { snprintf(err, errlen, "Write: %s", fr_str(d->fr)); ok = false; }
    if (ok && d->total > 0 && d->done != d->total) { snprintf(err, errlen, "Incomplete download"); ok = false; }
    if (ok && (d->done < 512 || d->done % 512 || d->first[0] != 0x0A324655 || d->first[1] != 0x9E5D5157)) {
        snprintf(err, errlen, "Downloaded file is not a UF2");
        ok = false;
    }
    if (ok) {
        f_chmod(dest, 0, AM_RDO);
        fr = f_unlink(dest);
        if (fr != FR_OK && fr != FR_NO_FILE) { snprintf(err, errlen, "Replace: %s", fr_str(fr)); ok = false; }
        else if ((fr = f_rename(tmp, dest)) != FR_OK) { snprintf(err, errlen, "Rename: %s", fr_str(fr)); ok = false; }
    }
    if (!ok) f_unlink(tmp);
    free(d);
    return ok;
}

void updater_check(void) {
    if (!g_cfg.wifi_ssid[0]) {
        ui_error("Update", "Configure Wi-Fi first\n(F9 menu > Wi-Fi settings)");
        return;
    }
    char err[96] = "";
    char tag[32] = "", url[512] = "";
    ui_progress_begin("Check for update");
    int rc = net_wifi_connect(g_cfg.wifi_ssid, g_cfg.wifi_pass, err, sizeof(err));
    if (rc == 0) {
        membuf_t m = {0};
        http_cb_t cb = {NULL, mem_body, &m};
        int st = net_https_get("https://api.github.com/repos/" FM_GITHUB_REPO "/releases/latest",
                               "application/vnd.github+json", &cb, err, sizeof(err));
        if (st == 404) { snprintf(err, sizeof(err), "No releases published yet"); rc = -1; }
        else if (st != 200) rc = -1;
        else if (!m.buf || !json_str(m.buf, "tag_name", tag, sizeof(tag)) || !tag[0]) {
            snprintf(err, sizeof(err), "Unexpected reply from GitHub");
            rc = -1;
        } else {
            const char *p = m.buf;
            char u[512];
            while ((p = json_str(p, "browser_download_url", u, sizeof(u))) != NULL) {
                size_t l = strlen(u), fl = strlen(UPDATE_FILE);
                if (l >= fl && !strcmp(u + l - fl, UPDATE_FILE)) { strcpy(url, u); break; }
            }
            if (!url[0])
                snprintf(url, sizeof(url), "https://github.com/" FM_GITHUB_REPO "/releases/download/%s/" UPDATE_FILE, tag);
        }
        free(m.buf);
    }
    ui_progress_end();

    if (rc != 0) {
        net_wifi_off();
        if (rc != -2) ui_error("Update check failed", err);
        return;
    }

    char msg[160];
    int cmp = version_cmp(tag, FM_VERSION);
    if (cmp <= 0) {
        snprintf(msg, sizeof(msg), "You have the latest version.\n\nInstalled: v%s\nLatest:    %s\n\nDownload it again anyway?", FM_VERSION, tag);
        static const char *const o[] = {"No", "Yes"};
        if (ui_choice("Update", msg, o, 2, 0, false) != 1) { net_wifi_off(); return; }
    } else {
        snprintf(msg, sizeof(msg), "New version available!\n\nInstalled: v%s\nLatest:    %s\n\nDownload to SD card?", FM_VERSION, tag);
        static const char *const o[] = {"Yes", "No"};
        if (ui_choice("Update", msg, o, 2, 0, false) != 0) { net_wifi_off(); return; }
    }

    char dest[MAX_PATH];
    snprintf(dest, sizeof(dest), UPDATE_DIR "/" UPDATE_FILE);
    ui_progress_begin("Update");
    bool ok = download(url, dest, err, sizeof(err));
    ui_progress_end();
    net_wifi_off();
    if (!ok) {
        if (strcmp(err, "Cancelled")) ui_error("Download failed", err);
        return;
    }
    snprintf(msg, sizeof(msg), "Saved %s to\n" UPDATE_DIR "/\n\nRestart, then pick it in UF2Loader\n(hold Up/F1 at power-on).\n\nRestart now?", tag);
    static const char *const o[] = {"Restart", "Later"};
    if (ui_choice("Update downloaded", msg, o, 2, 0, false) == 0) {
        config_save();
        watchdog_reboot(0, 0, 10);
        for (;;) tight_loop_contents();
    }
}
