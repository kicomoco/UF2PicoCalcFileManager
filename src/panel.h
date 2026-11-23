#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "util.h"

enum { SORT_NAME, SORT_EXT, SORT_SIZE, SORT_DATE, SORT_COUNT };

typedef struct {
    char *name;
    uint64_t size;
    uint16_t fdate, ftime;
    uint8_t attr;
    bool is_dir;
    bool tagged;
} entry_t;

typedef struct {
    char path[MAX_PATH];
    entry_t *ents;
    int count, cap;
    int sel, top;
    int ntagged;
    int err;
} panel_t;

extern int g_sort_mode;
extern bool g_show_hidden;

void panel_init(panel_t *p, const char *path);
/* Reloads the directory, keeping the cursor on keep_name if given. Returns FatFs result. */
int panel_load(panel_t *p, const char *keep_name);
void panel_free(panel_t *p);
entry_t *panel_cur(panel_t *p);
void panel_select_name(panel_t *p, const char *name);
bool panel_is_root(const panel_t *p);
void panel_tag_toggle(panel_t *p, int idx);
void panel_tag_all(panel_t *p, int mode); /* 0 = clear, 1 = all files, 2 = invert */
uint64_t panel_tagged_bytes(panel_t *p);
