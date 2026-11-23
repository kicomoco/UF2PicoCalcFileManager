#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "ff.h"
#include "panel.h"

int g_sort_mode = SORT_NAME;
bool g_show_hidden = false;

#define MAX_ENTRIES 4000

void panel_init(panel_t *p, const char *path) {
    memset(p, 0, sizeof(*p));
    strncpy(p->path, path, sizeof(p->path) - 1);
}

void panel_free(panel_t *p) {
    for (int i = 0; i < p->count; i++) free(p->ents[i].name);
    free(p->ents);
    p->ents = NULL;
    p->count = p->cap = 0;
    p->ntagged = 0;
}

bool panel_is_root(const panel_t *p) {
    return strcmp(p->path, "/") == 0;
}

static const char *ext_of(const char *n) {
    const char *d = strrchr(n, '.');
    return (d && d != n) ? d + 1 : "";
}

static int cmp_entries(const void *a, const void *b) {
    const entry_t *x = a, *y = b;
    if (strcmp(x->name, "..") == 0) return -1;
    if (strcmp(y->name, "..") == 0) return 1;
    if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;
    int r = 0;
    switch (g_sort_mode) {
    case SORT_EXT: r = strcasecmp(ext_of(x->name), ext_of(y->name)); break;
    case SORT_SIZE: r = (x->size < y->size) - (x->size > y->size); break;
    case SORT_DATE: {
        uint32_t dx = ((uint32_t)x->fdate << 16) | x->ftime, dy = ((uint32_t)y->fdate << 16) | y->ftime;
        r = (dx < dy) - (dx > dy);
        break;
    }
    default: break;
    }
    if (r == 0) r = strcasecmp(x->name, y->name);
    return r;
}

static bool add_entry(panel_t *p, const char *name, uint64_t size, uint8_t attr, uint16_t fd, uint16_t ft) {
    if (p->count >= MAX_ENTRIES) return false;
    if (p->count == p->cap) {
        int nc = p->cap ? p->cap * 2 : 64;
        entry_t *ne = realloc(p->ents, nc * sizeof(entry_t));
        if (!ne) return false;
        p->ents = ne;
        p->cap = nc;
    }
    char *n = strdup(name);
    if (!n) return false;
    entry_t *e = &p->ents[p->count++];
    e->name = n;
    e->size = size;
    e->attr = attr;
    e->fdate = fd;
    e->ftime = ft;
    e->is_dir = (attr & AM_DIR) != 0;
    e->tagged = false;
    return true;
}

int panel_load(panel_t *p, const char *keep_name) {
    char keep[256] = "";
    if (keep_name) strncpy(keep, keep_name, sizeof(keep) - 1);
    else if (p->count && p->sel < p->count) strncpy(keep, p->ents[p->sel].name, sizeof(keep) - 1);
    int old_sel = p->sel;

    panel_free(p);
    if (!panel_is_root(p)) add_entry(p, "..", 0, AM_DIR, 0, 0);

    DIR dir;
    FILINFO fi;
    FRESULT fr = f_opendir(&dir, p->path);
    if (fr == FR_OK) {
        for (;;) {
            fr = f_readdir(&dir, &fi);
            if (fr != FR_OK || fi.fname[0] == 0) break;
            if (!g_show_hidden && (fi.fattrib & (AM_HID | AM_SYS))) continue;
            if (!g_show_hidden && fi.fname[0] == '.') continue;
            if (!add_entry(p, fi.fname, fi.fsize, fi.fattrib, fi.fdate, fi.ftime)) break;
        }
        f_closedir(&dir);
    }
    p->err = fr;
    if (p->count > 1) qsort(p->ents, p->count, sizeof(entry_t), cmp_entries);

    p->sel = -1;
    if (keep[0]) panel_select_name(p, keep);
    if (p->sel < 0) p->sel = old_sel;
    if (p->sel >= p->count) p->sel = p->count - 1;
    if (p->sel < 0) p->sel = 0;
    return fr;
}

void panel_select_name(panel_t *p, const char *name) {
    for (int i = 0; i < p->count; i++)
        if (strcmp(p->ents[i].name, name) == 0) { p->sel = i; return; }
}

entry_t *panel_cur(panel_t *p) {
    if (p->sel < 0 || p->sel >= p->count) return NULL;
    return &p->ents[p->sel];
}

void panel_tag_toggle(panel_t *p, int idx) {
    if (idx < 0 || idx >= p->count) return;
    entry_t *e = &p->ents[idx];
    if (strcmp(e->name, "..") == 0) return;
    e->tagged = !e->tagged;
    p->ntagged += e->tagged ? 1 : -1;
}

void panel_tag_all(panel_t *p, int mode) {
    p->ntagged = 0;
    for (int i = 0; i < p->count; i++) {
        entry_t *e = &p->ents[i];
        if (strcmp(e->name, "..") == 0) continue;
        if (mode == 0) e->tagged = false;
        else if (mode == 1) e->tagged = !e->is_dir || e->tagged;
        else e->tagged = !e->tagged;
        if (e->tagged) p->ntagged++;
    }
}

uint64_t panel_tagged_bytes(panel_t *p) {
    uint64_t t = 0;
    for (int i = 0; i < p->count; i++)
        if (p->ents[i].tagged && !p->ents[i].is_dir) t += p->ents[i].size;
    return t;
}
