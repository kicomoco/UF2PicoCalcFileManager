#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "ff.h"
#include "lcd.h"
#include "kbd.h"
#include "ui.h"
#include "util.h"
#include "hexedit.h"

#define BPR 8                       /* bytes per row */
#define DATA_Y 1
#define DATA_H (SCR_ROWS - 3)
#define PAGE (BPR * DATA_H)
#define HEX_X 7
#define ASC_X (HEX_X + BPR * 3)

typedef struct { uint64_t off; uint8_t v; } edit_t;
typedef struct { uint64_t off; int had; uint8_t old; uint64_t newsize; } undo_t;

typedef struct {
    char path[MAX_PATH];
    FIL f;
    bool readonly;
    uint64_t size, newsize;
    uint64_t cur, top;
    int nibble;
    bool ascii;
    edit_t *edits;
    int nedits, cap;
    undo_t *undo;
    int nundo, ucap;
    uint8_t page[PAGE];
    uint64_t page_off;
    int page_len;
    bool page_valid;
    char find[64];
} hx_t;

static int find_edit(hx_t *h, uint64_t off, bool *found) {
    int lo = 0, hi = h->nedits;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (h->edits[mid].off < off) lo = mid + 1;
        else hi = mid;
    }
    *found = lo < h->nedits && h->edits[lo].off == off;
    return lo;
}

static void load_page(hx_t *h) {
    if (h->page_valid && h->page_off == h->top) return;
    h->page_off = h->top;
    h->page_len = 0;
    h->page_valid = true;
    if (h->top >= h->size) return;
    UINT br = 0;
    if (f_lseek(&h->f, h->top) == FR_OK && f_read(&h->f, h->page, PAGE, &br) == FR_OK) h->page_len = br;
}

static uint8_t file_byte(hx_t *h, uint64_t off) {
    if (off >= h->page_off && off < h->page_off + h->page_len) return h->page[off - h->page_off];
    uint8_t b = 0;
    UINT br;
    if (off < h->size && f_lseek(&h->f, off) == FR_OK) f_read(&h->f, &b, 1, &br);
    return b;
}

static uint8_t get_byte(hx_t *h, uint64_t off, bool *edited) {
    bool found;
    int i = find_edit(h, off, &found);
    if (edited) *edited = found;
    if (found) return h->edits[i].v;
    return file_byte(h, off);
}

static bool set_byte(hx_t *h, uint64_t off, uint8_t v) {
    bool found;
    int i = find_edit(h, off, &found);
    if (h->nundo == h->ucap) {
        int nc = h->ucap ? h->ucap * 2 : 64;
        undo_t *nu = realloc(h->undo, nc * sizeof(undo_t));
        if (!nu) return false;
        h->undo = nu;
        h->ucap = nc;
    }
    h->undo[h->nundo++] = (undo_t){off, found, found ? h->edits[i].v : 0, h->newsize};
    if (found) {
        h->edits[i].v = v;
    } else {
        if (h->nedits == h->cap) {
            int nc = h->cap ? h->cap * 2 : 64;
            edit_t *ne = realloc(h->edits, nc * sizeof(edit_t));
            if (!ne) { h->nundo--; return false; }
            h->edits = ne;
            h->cap = nc;
        }
        memmove(&h->edits[i + 1], &h->edits[i], (h->nedits - i) * sizeof(edit_t));
        h->edits[i] = (edit_t){off, v};
        h->nedits++;
    }
    if (off >= h->newsize) h->newsize = off + 1;
    return true;
}

static void undo(hx_t *h) {
    if (!h->nundo) return;
    undo_t u = h->undo[--h->nundo];
    bool found;
    int i = find_edit(h, u.off, &found);
    if (found) {
        if (u.had) h->edits[i].v = u.old;
        else {
            memmove(&h->edits[i], &h->edits[i + 1], (h->nedits - i - 1) * sizeof(edit_t));
            h->nedits--;
        }
    }
    h->newsize = u.newsize;
    h->cur = u.off;
    h->nibble = 0;
}

static bool save(hx_t *h) {
    if (!h->nedits) return true;
    ui_progress_begin("Saving");
    ui_progress_set(path_base(h->path), "");
    FRESULT fr = FR_OK;
    uint8_t run[256];
    int i = 0;
    while (i < h->nedits && fr == FR_OK) {
        uint64_t start = h->edits[i].off;
        int n = 0;
        while (i < h->nedits && n < (int)sizeof(run) && h->edits[i].off == start + n) run[n++] = h->edits[i++].v;
        UINT bw;
        fr = f_lseek(&h->f, start);
        if (fr == FR_OK) fr = f_write(&h->f, run, n, &bw);
        if (fr == FR_OK && (int)bw != n) fr = FR_DENIED;
        ui_progress_tick();
    }
    if (fr == FR_OK) fr = f_sync(&h->f);
    ui_progress_end();
    if (fr != FR_OK) {
        ui_error("Save", fr_str(fr));
        return false;
    }
    h->nedits = 0;
    h->nundo = 0;
    h->size = h->newsize = f_size(&h->f);
    h->page_valid = false;
    return true;
}

static bool parse_number(const char *s, uint64_t *out) {
    while (*s == ' ') s++;
    int base = 10;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
    else if (*s == '$' || *s == '#') { base = 16; s++; }
    else {
        for (const char *p = s; *p; p++)
            if (isalpha((unsigned char)*p)) { base = 16; break; }
    }
    if (!*s) return false;
    char *end;
    *out = strtoull(s, &end, base);
    return *end == 0 || *end == ' ';
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static void do_find(hx_t *h) {
    if (!ui_input("Find", "Text, or hex bytes after '#'\n(e.g. #DEADBEEF):", h->find, sizeof(h->find), false)) return;
    uint8_t pat[32];
    int pl = 0;
    if (h->find[0] == '#') {
        int hi = -1;
        for (const char *p = h->find + 1; *p && pl < (int)sizeof(pat); p++) {
            int v = hexval((unsigned char)*p);
            if (v < 0) continue;
            if (hi < 0) hi = v;
            else { pat[pl++] = (hi << 4) | v; hi = -1; }
        }
    } else {
        pl = strlen(h->find);
        if (pl > (int)sizeof(pat)) pl = sizeof(pat);
        memcpy(pat, h->find, pl);
    }
    if (!pl) return;

    enum { BLK = 4096 };
    uint8_t *buf = malloc(BLK + sizeof(pat));
    if (!buf) return;
    ui_progress_begin("Searching");
    uint64_t pos = h->cur + 1;
    bool found = false;
    while (pos + pl <= h->size && !found) {
        char l2[40];
        snprintf(l2, sizeof(l2), "Offset 0x%llX", (unsigned long long)pos);
        ui_progress_set("Scanning file...", l2);
        if (ui_progress_cancelled()) break;
        UINT br = 0;
        if (f_lseek(&h->f, pos) != FR_OK || f_read(&h->f, buf, BLK + pl - 1, &br) != FR_OK || br < (UINT)pl) break;
        for (UINT i = 0; i + pl <= br; i++) {
            if (memcmp(buf + i, pat, pl) == 0) {
                h->cur = pos + i;
                found = true;
                break;
            }
        }
        pos += br - pl + 1;
    }
    ui_progress_end();
    free(buf);
    h->page_valid = false;
    h->nibble = 0;
    if (!found) ui_message("Find", "Not found");
}

static void draw(hx_t *h) {
    if (h->cur < h->top) h->top = h->cur / BPR * BPR;
    if (h->cur >= h->top + PAGE) h->top = (h->cur / BPR - DATA_H + 1) * BPR;
    load_page(h);

    char name[SCR_COLS + 1], hdr[SCR_COLS + 1];
    utf8_to_disp(name, sizeof(name), path_base(h->path));
    snprintf(hdr, sizeof(hdr), "%s%s%s", h->readonly ? "HexView:" : "Hex:", h->nedits ? "*" : "", name);
    scr_putsw(0, 0, hdr, C_BLACK, C_CYAN, SCR_COLS);

    uint64_t limit = h->readonly ? h->newsize : h->newsize + 1;
    for (int r = 0; r < DATA_H; r++) {
        uint64_t ro = h->top + (uint64_t)r * BPR;
        int y = DATA_Y + r;
        scr_fill(0, y, SCR_COLS, 1, ' ', C_LGRAY, C_BLUE);
        if (ro >= limit) continue;
        char off[8];
        snprintf(off, sizeof(off), "%06llX", (unsigned long long)(ro & 0xFFFFFF));
        scr_puts(0, y, off, C_LCYAN, C_BLUE, 6);
        for (int i = 0; i < BPR; i++) {
            uint64_t o = ro + i;
            int hx = HEX_X + i * 3, ax = ASC_X + i;
            if (o >= h->newsize) {
                if (o == h->newsize && !h->readonly && o == h->cur) {
                    scr_puts(hx, y, "__", C_BLACK, h->ascii ? C_LGRAY : C_CYAN, 2);
                    scr_putc(ax, y, '_', C_BLACK, h->ascii ? C_CYAN : C_LGRAY);
                }
                continue;
            }
            bool edited;
            uint8_t b = get_byte(h, o, &edited);
            uint8_t fg = edited ? C_YELLOW : ((i & 4) ? C_LGRAY : C_WHITE);
            uint8_t bg = C_BLUE, abg = C_BLUE, afg = edited ? C_YELLOW : C_LGRAY;
            if (o == h->cur) {
                if (h->ascii) { bg = C_LGRAY; fg = C_BLACK; abg = C_CYAN; afg = C_BLACK; }
                else { bg = C_CYAN; fg = C_BLACK; abg = C_LGRAY; afg = C_BLACK; }
            }
            char hb[3];
            snprintf(hb, sizeof(hb), "%02X", b);
            scr_puts(hx, y, hb, fg, bg, 2);
            scr_putc(ax, y, (b >= 32 && b < 127) ? b : '.', afg, abg);
        }
    }

    bool e;
    uint8_t b = h->cur < h->newsize ? get_byte(h, h->cur, &e) : 0;
    char st[SCR_COLS + 1];
    if (h->cur < h->newsize)
        snprintf(st, sizeof(st), "%s @%llX/%llX =%02X %3u", h->ascii ? "ASC" : "HEX",
                 (unsigned long long)h->cur, (unsigned long long)h->newsize, b, b);
    else
        snprintf(st, sizeof(st), "%s @%llX  <end: type to append>", h->ascii ? "ASC" : "HEX",
                 (unsigned long long)h->cur);
    scr_putsw(0, SCR_ROWS - 2, st, C_BLACK, C_LGRAY, SCR_COLS);

    static const char *const kb_edit[10] = {"Hlp", "Sav", NULL, NULL, NULL, NULL, "Fnd", NULL, "Go", "Quit"};
    static const char *const kb_view[10] = {"Hlp", NULL, NULL, "Edt", NULL, NULL, "Fnd", NULL, "Go", "Quit"};
    ui_keybar(h->readonly ? kb_view : kb_edit);

    int r = (int)((h->cur - h->top) / BPR), i = (int)((h->cur - h->top) % BPR);
    if (h->readonly) scr_set_cursor(0, 0, false);
    else if (h->ascii) scr_set_cursor(ASC_X + i, DATA_Y + r, true);
    else scr_set_cursor(HEX_X + i * 3 + h->nibble, DATA_Y + r, true);
    scr_flush();
}

static const char hex_help[] =
    "Hex editor / viewer\n"
    "\n"
    "Arrows        Move by byte / row\n"
    "Shift+Up/Dn   Page up / down\n"
    "Shift+Tab/Del Start / end of row\n"
    "Tab           Switch HEX / ASCII column\n"
    "0-9 A-F       Type nibbles (HEX column)\n"
    "Any key       Type character (ASCII)\n"
    "Ctrl+Z        Undo last change\n"
    "\n"
    "F2  / Ctrl+S  Save changes\n"
    "F4            Viewer: switch to edit\n"
    "F7  / Ctrl+F  Find text or #hex bytes\n"
    "F9  / Ctrl+G  Go to offset\n"
    "              (0x1F, $1F or decimal)\n"
    "F10 / Esc     Close\n"
    "\n"
    "Edits overwrite bytes in place. Move\n"
    "past the last byte to append data.\n"
    "Changed bytes are shown in yellow.\n";

void hexedit_open(const char *path, bool readonly) {
    hx_t *h = calloc(1, sizeof(hx_t));
    if (!h) { ui_error("Hex", "Out of memory"); return; }
    strncpy(h->path, path, sizeof(h->path) - 1);
    h->readonly = readonly;
    FRESULT fr = f_open(&h->f, path, readonly ? FA_READ : (FA_READ | FA_WRITE));
    if (fr == FR_DENIED && !readonly) {
        fr = f_open(&h->f, path, FA_READ);
        h->readonly = true;
    }
    if (fr != FR_OK) {
        ui_error("Open", fr_str(fr));
        free(h);
        return;
    }
    h->size = h->newsize = f_size(&h->f);

    for (;;) {
        draw(h);
        int k = kbd_wait(NULL);
        int c = KEY_CODE(k);
        bool ctrl = k & KM_CTRL;
        uint64_t maxoff = h->readonly ? (h->newsize ? h->newsize - 1 : 0) : h->newsize;

        if (c == KEY_F10 || c == KEY_ESC) {
            if (h->nedits) {
                static const char *const o[] = {"Save", "Discard", "Cancel"};
                int a = ui_choice("Close", "Save changes?", o, 3, 0, false);
                if (a == 0 && !save(h)) continue;
                if (a != 0 && a != 1) continue;
            }
            break;
        }
        if (c == KEY_F1) { ui_textpage("Hex editor help", hex_help); continue; }
        if (c == KEY_F7 || (ctrl && c == 'f')) { do_find(h); continue; }
        if (c == KEY_F9 || (ctrl && c == 'g')) {
            char buf[24] = "";
            uint64_t o;
            if (ui_input("Go to", "Offset (0x.. hex or decimal):", buf, sizeof(buf), false)) {
                if (parse_number(buf, &o)) { h->cur = o > maxoff ? maxoff : o; h->nibble = 0; }
                else ui_error("Go to", "Invalid offset");
            }
            continue;
        }
        if (c == KEY_TAB) { h->ascii = !h->ascii; h->nibble = 0; continue; }

        bool moved = true;
        switch (c) {
        case KEY_LEFT:
            if (!h->ascii && h->nibble) h->nibble = 0;
            else if (h->cur > 0) { h->cur--; h->nibble = 0; }
            break;
        case KEY_RIGHT: if (h->cur < maxoff) h->cur++; h->nibble = 0; break;
        case KEY_UP: if (h->cur >= BPR) h->cur -= BPR; break;
        case KEY_DOWN: h->cur = h->cur + BPR <= maxoff ? h->cur + BPR : maxoff; break;
        case KEY_PGUP: h->cur = h->cur >= PAGE ? h->cur - PAGE : 0; h->top = h->top >= PAGE ? h->top - PAGE : 0; break;
        case KEY_PGDN: h->cur = h->cur + PAGE <= maxoff ? h->cur + PAGE : maxoff; h->top += PAGE; if (h->top > h->cur) h->top = h->cur / BPR * BPR; break;
        case KEY_HOME: h->cur = h->cur / BPR * BPR; h->nibble = 0; break;
        case KEY_END: h->cur = h->cur / BPR * BPR + BPR - 1; if (h->cur > maxoff) h->cur = maxoff; h->nibble = 0; break;
        default: moved = false;
        }
        if (moved) continue;

        if (h->readonly) {
            if (c == KEY_F4) {
                f_close(&h->f);
                if (f_open(&h->f, path, FA_READ | FA_WRITE) == FR_OK) h->readonly = false;
                else {
                    f_open(&h->f, path, FA_READ);
                    ui_error("Edit", "File is read-only");
                }
                h->page_valid = false;
            }
            continue;
        }
        if (c == KEY_F2 || (ctrl && c == 's')) { save(h); continue; }
        if (ctrl && c == 'z') { undo(h); continue; }
        if (ctrl) continue;

        if (h->ascii) {
            if (c >= 32 && c < 127) {
                if (set_byte(h, h->cur, (uint8_t)c) && h->cur < h->newsize) h->cur++;
            }
        } else {
            int v = c < 128 ? hexval(c) : -1;
            if (v >= 0) {
                uint8_t b = h->cur < h->newsize ? get_byte(h, h->cur, NULL) : 0;
                b = h->nibble ? (b & 0xF0) | v : (b & 0x0F) | (v << 4);
                if (set_byte(h, h->cur, b)) {
                    if (h->nibble) { h->nibble = 0; h->cur++; }
                    else h->nibble = 1;
                }
            }
        }
    }
    f_close(&h->f);
    free(h->edits);
    free(h->undo);
    free(h);
}
