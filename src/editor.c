#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include "ff.h"
#include "lcd.h"
#include "kbd.h"
#include "ui.h"
#include "util.h"
#include "editor.h"

#define TABW 4
#define TEXT_Y 1
#define TEXT_H (SCR_ROWS - 2)

typedef struct {
    char *s;
    int len, cap;
} line_t;

typedef struct {
    char path[MAX_PATH];
    line_t *lines;
    int n, cap;
    int cy, cx, want_col;
    int top, left;
    bool crlf, modified, readonly;
    char find[64];
} ed_t;

static bool line_reserve(line_t *l, int need) {
    if (need <= l->cap) return true;
    int nc = l->cap ? l->cap : 16;
    while (nc < need) nc *= 2;
    char *ns = realloc(l->s, nc);
    if (!ns) return false;
    l->s = ns;
    l->cap = nc;
    return true;
}

static bool insert_line(ed_t *e, int at, const char *s, int len) {
    if (e->n == e->cap) {
        int nc = e->cap ? e->cap * 2 : 64;
        line_t *nl = realloc(e->lines, nc * sizeof(line_t));
        if (!nl) return false;
        e->lines = nl;
        e->cap = nc;
    }
    memmove(&e->lines[at + 1], &e->lines[at], (e->n - at) * sizeof(line_t));
    line_t *l = &e->lines[at];
    memset(l, 0, sizeof(*l));
    e->n++;
    if (!line_reserve(l, len + 1)) return false;
    if (len) memcpy(l->s, s, len);
    l->len = len;
    return true;
}

static void delete_line(ed_t *e, int at) {
    free(e->lines[at].s);
    memmove(&e->lines[at], &e->lines[at + 1], (e->n - at - 1) * sizeof(line_t));
    e->n--;
}

static void free_all(ed_t *e) {
    for (int i = 0; i < e->n; i++) free(e->lines[i].s);
    free(e->lines);
}

static int disp_col(const line_t *l, int idx) {
    int col = 0;
    for (int i = 0; i < idx && i < l->len; i++) {
        unsigned char c = l->s[i];
        if (c == '\t') col = (col / TABW + 1) * TABW;
        else if ((c & 0xC0) != 0x80) col++;
    }
    return col;
}

static int idx_for_col(const line_t *l, int want) {
    int col = 0, i = 0;
    while (i < l->len) {
        unsigned char c = l->s[i];
        int next = col;
        if (c == '\t') next = (col / TABW + 1) * TABW;
        else if ((c & 0xC0) != 0x80) next = col + 1;
        if (next > want) break;
        col = next;
        i++;
        while (i < l->len && ((unsigned char)l->s[i] & 0xC0) == 0x80) i++;
    }
    return i;
}

static int load(ed_t *e) {
    FIL f;
    FRESULT fr = f_open(&f, e->path, FA_READ);
    if (fr == FR_NO_FILE) return insert_line(e, 0, "", 0) ? EDIT_OK : EDIT_ERROR;
    if (fr != FR_OK) {
        ui_error("Open", fr_str(fr));
        return EDIT_ERROR;
    }
    FSIZE_t size = f_size(&f);
    if (size > EDIT_MAX_FILE) {
        f_close(&f);
        return EDIT_TOO_LARGE;
    }
    char *buf = malloc(size + 1);
    if (!buf) {
        f_close(&f);
        ui_error("Open", "Not enough memory");
        return EDIT_ERROR;
    }
    UINT br = 0;
    fr = f_read(&f, buf, size, &br);
    f_close(&f);
    if (fr != FR_OK) {
        free(buf);
        ui_error("Read", fr_str(fr));
        return EDIT_ERROR;
    }
    e->crlf = memmem(buf, br, "\r\n", 2) != NULL;
    int start = 0;
    bool ok = true;
    for (int i = 0; i <= (int)br && ok; i++) {
        if (i == (int)br || buf[i] == '\n') {
            int len = i - start;
            if (e->crlf && len > 0 && buf[start + len - 1] == '\r' && i < (int)br) len--;
            ok = insert_line(e, e->n, buf + start, len);
            start = i + 1;
        }
    }
    free(buf);
    if (!ok) {
        ui_error("Open", "Not enough memory");
        return EDIT_ERROR;
    }
    return EDIT_OK;
}

static bool save(ed_t *e) {
    char tmp[MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s.~tmp", e->path);
    FIL f;
    FRESULT fr = f_open(&f, tmp, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        ui_error("Save", fr_str(fr));
        return false;
    }
    ui_progress_begin("Saving");
    ui_progress_set(path_base(e->path), "");
    const char *eol = e->crlf ? "\r\n" : "\n";
    int eoll = e->crlf ? 2 : 1;
    UINT bw;
    for (int i = 0; i < e->n && fr == FR_OK; i++) {
        if (e->lines[i].len) {
            fr = f_write(&f, e->lines[i].s, e->lines[i].len, &bw);
            if (fr == FR_OK && bw != (UINT)e->lines[i].len) fr = FR_DENIED;
        }
        if (fr == FR_OK && i < e->n - 1) {
            fr = f_write(&f, eol, eoll, &bw);
            if (fr == FR_OK && bw != (UINT)eoll) fr = FR_DENIED;
        }
        if ((i & 63) == 0) ui_progress_tick();
    }
    FRESULT cfr = f_close(&f);
    if (fr == FR_OK) fr = cfr;
    if (fr == FR_OK) {
        /* Keep the original as a backup until the new file is in place. */
        char bak[MAX_PATH];
        snprintf(bak, sizeof(bak), "%s.~bak", e->path);
        FILINFO fi;
        bool had = f_stat(e->path, &fi) == FR_OK;
        if (had) {
            f_unlink(bak);
            fr = f_rename(e->path, bak);
        }
        if (fr == FR_OK) {
            fr = f_rename(tmp, e->path);
            if (fr != FR_OK && had) f_rename(bak, e->path);
            else if (had) f_unlink(bak);
        }
    }
    if (fr != FR_OK) f_unlink(tmp);
    ui_progress_end();
    if (fr != FR_OK) {
        ui_error("Save", fr_str(fr));
        return false;
    }
    e->modified = false;
    return true;
}

static void draw(ed_t *e) {
    line_t *cl = &e->lines[e->cy];
    int ccol = disp_col(cl, e->cx);
    if (e->cy < e->top) e->top = e->cy;
    if (e->cy >= e->top + TEXT_H) e->top = e->cy - TEXT_H + 1;
    if (ccol < e->left) e->left = ccol > 8 ? ccol - 8 : 0;
    if (ccol >= e->left + SCR_COLS) e->left = ccol - SCR_COLS + 8;

    char hdr[SCR_COLS + 1], name[SCR_COLS + 1], pos[24];
    utf8_to_disp(name, sizeof(name), path_base(e->path));
    snprintf(pos, sizeof(pos), "%s%d:%d", e->modified ? "* " : "", e->cy + 1, ccol + 1);
    int room = SCR_COLS - (int)strlen(pos) - 2;
    snprintf(hdr, sizeof(hdr), "%s%.*s", e->readonly ? "View:" : "", room - (e->readonly ? 5 : 0), name);
    scr_fill(0, 0, SCR_COLS, 1, ' ', C_BLACK, C_CYAN);
    scr_puts(0, 0, hdr, C_BLACK, C_CYAN, room);
    scr_puts(SCR_COLS - strlen(pos), 0, pos, C_BLACK, C_CYAN, -1);

    for (int r = 0; r < TEXT_H; r++) {
        int li = e->top + r;
        scr_fill(0, TEXT_Y + r, SCR_COLS, 1, ' ', C_LGRAY, C_BLUE);
        if (li >= e->n) {
            if (li == e->n) scr_putc(0, TEXT_Y + r, '~', C_DGRAY, C_BLUE);
            continue;
        }
        line_t *l = &e->lines[li];
        int col = 0;
        for (int i = 0; i < l->len && col < e->left + SCR_COLS; i++) {
            unsigned char c = l->s[i];
            if ((c & 0xC0) == 0x80) continue;
            if (c == '\t') {
                int next = (col / TABW + 1) * TABW;
                for (; col < next; col++)
                    if (col >= e->left) scr_putc(col - e->left, TEXT_Y + r, ' ', C_DGRAY, C_BLUE);
                continue;
            }
            if (col >= e->left) {
                uint8_t fg = C_WHITE;
                if (c < 32) { c = '.'; fg = C_DGRAY; }
                else if (c >= 0x80) { c = '?'; fg = C_LCYAN; }
                scr_putc(col - e->left, TEXT_Y + r, c, fg, C_BLUE);
            }
            col++;
        }
        if (e->left > 0) scr_putc(0, TEXT_Y + r, '<', C_YELLOW, C_BLUE);
        if (col > e->left + SCR_COLS) scr_putc(SCR_COLS - 1, TEXT_Y + r, '>', C_YELLOW, C_BLUE);
    }
    static const char *const kb_edit[10] = {"Hlp", "Sav", NULL, NULL, NULL, NULL, "Fnd", "DLn", "Go", "Quit"};
    static const char *const kb_view[10] = {"Hlp", NULL, NULL, "Edt", NULL, NULL, "Fnd", NULL, "Go", "Quit"};
    ui_keybar(e->readonly ? kb_view : kb_edit);
    if (e->readonly) scr_set_cursor(0, 0, false);
    else scr_set_cursor(ccol - e->left, TEXT_Y + e->cy - e->top, true);
    scr_flush();
}

static void do_find(ed_t *e, bool prompt) {
    if (prompt || !e->find[0]) {
        if (!ui_input("Find", "Search for (case-insensitive):", e->find, sizeof(e->find), false)) return;
    }
    if (!e->find[0]) return;
    int fl = strlen(e->find);
    for (int pass = 0; pass <= e->n; pass++) {
        int li = (e->cy + pass) % e->n;
        line_t *l = &e->lines[li];
        int from = pass == 0 ? e->cx + 1 : 0;
        for (int i = from; i + fl <= l->len; i++) {
            if (strncasecmp(l->s + i, e->find, fl) == 0) {
                e->cy = li;
                e->cx = i;
                e->want_col = disp_col(l, i);
                return;
            }
        }
    }
    ui_message("Find", "Not found");
}

static void do_goto(ed_t *e) {
    char buf[16] = "";
    if (!ui_input("Go to line", "Line number:", buf, sizeof(buf), false)) return;
    int ln = atoi(buf);
    if (ln < 1) ln = 1;
    if (ln > e->n) ln = e->n;
    e->cy = ln - 1;
    e->cx = 0;
    e->want_col = 0;
}

static const char editor_help[] =
    "Text editor / viewer\n"
    "\n"
    "Arrows        Move cursor\n"
    "Shift+Up/Dn   Page up / down\n"
    "Shift+Tab/Del Home / End of line\n"
    "Enter         Split line\n"
    "Bksp / Del    Delete character\n"
    "\n"
    "F2  / Ctrl+S  Save\n"
    "F4            Viewer: switch to edit\n"
    "F7  / Ctrl+F  Find (Ctrl+N: next)\n"
    "F8  / Ctrl+K  Delete line\n"
    "F9  / Ctrl+G  Go to line\n"
    "F10 / Esc     Close\n"
    "\n"
    "Line endings (LF / CRLF) are kept.\n"
    "Non-ASCII bytes are shown as '?'.\n";

int editor_open(const char *path, bool readonly) {
    ed_t *e = calloc(1, sizeof(ed_t));
    if (!e) return EDIT_ERROR;
    strncpy(e->path, path, sizeof(e->path) - 1);
    e->readonly = readonly;
    int r = load(e);
    if (r != EDIT_OK) {
        free_all(e);
        free(e);
        return r;
    }

    for (;;) {
        draw(e);
        int k = kbd_wait(NULL);
        int c = KEY_CODE(k);
        bool ctrl = k & KM_CTRL;
        line_t *l = &e->lines[e->cy];
        bool vmove = false;

        if (c == KEY_F10 || c == KEY_ESC) {
            if (e->modified) {
                static const char *const o[] = {"Save", "Discard", "Cancel"};
                int a = ui_choice("Close", "Save changes?", o, 3, 0, false);
                if (a == 0 && !save(e)) continue;
                if (a != 0 && a != 1) continue;
            }
            break;
        }
        if (c == KEY_F1) { ui_textpage("Editor help", editor_help); continue; }
        if (c == KEY_F7 || (ctrl && c == 'f')) { do_find(e, true); continue; }
        if (ctrl && c == 'n') { do_find(e, false); continue; }
        if (c == KEY_F9 || (ctrl && c == 'g')) { do_goto(e); continue; }

        switch (c) {
        case KEY_UP: if (e->cy > 0) e->cy--; vmove = true; break;
        case KEY_DOWN: if (e->cy < e->n - 1) e->cy++; vmove = true; break;
        case KEY_PGUP: e->cy = e->cy > TEXT_H ? e->cy - TEXT_H : 0; e->top = e->top > TEXT_H ? e->top - TEXT_H : 0; vmove = true; break;
        case KEY_PGDN: e->cy = e->cy + TEXT_H < e->n ? e->cy + TEXT_H : e->n - 1; e->top += TEXT_H; if (e->top > e->n - 1) e->top = e->n - 1; vmove = true; break;
        case KEY_LEFT:
            if (e->cx > 0) { e->cx--; while (e->cx > 0 && ((unsigned char)l->s[e->cx] & 0xC0) == 0x80) e->cx--; }
            else if (e->cy > 0) { e->cy--; e->cx = e->lines[e->cy].len; }
            e->want_col = disp_col(&e->lines[e->cy], e->cx);
            break;
        case KEY_RIGHT:
            if (e->cx < l->len) { e->cx++; while (e->cx < l->len && ((unsigned char)l->s[e->cx] & 0xC0) == 0x80) e->cx++; }
            else if (e->cy < e->n - 1) { e->cy++; e->cx = 0; }
            e->want_col = disp_col(&e->lines[e->cy], e->cx);
            break;
        case KEY_HOME: e->cx = 0; e->want_col = 0; break;
        case KEY_END: e->cx = l->len; e->want_col = disp_col(l, e->cx); break;
        default: break;
        }
        if (vmove) {
            e->cx = idx_for_col(&e->lines[e->cy], e->want_col);
            continue;
        }
        if (c == KEY_LEFT || c == KEY_RIGHT || c == KEY_HOME || c == KEY_END) continue;

        if (e->readonly) {
            if (c == KEY_F4) e->readonly = false;
            else if (c == ' ') { e->cy = e->cy + TEXT_H < e->n ? e->cy + TEXT_H : e->n - 1; e->top += TEXT_H; if (e->top > e->n - 1) e->top = e->n - 1; }
            continue;
        }

        if (c == KEY_F2 || (ctrl && c == 's')) { save(e); continue; }
        if (c == KEY_F8 || (ctrl && c == 'k')) {
            if (e->n > 1) delete_line(e, e->cy);
            else e->lines[0].len = 0;
            if (e->cy >= e->n) e->cy = e->n - 1;
            e->cx = idx_for_col(&e->lines[e->cy], e->want_col);
            e->modified = true;
            continue;
        }
        if (ctrl) continue;

        if (c == KEY_ENTER) {
            if (insert_line(e, e->cy + 1, l->s + e->cx, l->len - e->cx)) {
                e->lines[e->cy].len = e->cx;
                e->cy++;
                e->cx = 0;
                e->want_col = 0;
                e->modified = true;
            } else {
                ui_error("Edit", "Out of memory");
            }
        } else if (c == KEY_BACKSPACE) {
            if (e->cx > 0) {
                int p = e->cx - 1;
                while (p > 0 && ((unsigned char)l->s[p] & 0xC0) == 0x80) p--;
                memmove(l->s + p, l->s + e->cx, l->len - e->cx);
                l->len -= e->cx - p;
                e->cx = p;
                e->modified = true;
            } else if (e->cy > 0) {
                line_t *pl = &e->lines[e->cy - 1];
                if (line_reserve(pl, pl->len + l->len + 1)) {
                    int pos = pl->len;
                    memcpy(pl->s + pl->len, l->s, l->len);
                    pl->len += l->len;
                    delete_line(e, e->cy);
                    e->cy--;
                    e->cx = pos;
                    e->modified = true;
                }
            }
            e->want_col = disp_col(&e->lines[e->cy], e->cx);
        } else if (c == KEY_DEL) {
            if (e->cx < l->len) {
                int nx = e->cx + 1;
                while (nx < l->len && ((unsigned char)l->s[nx] & 0xC0) == 0x80) nx++;
                memmove(l->s + e->cx, l->s + nx, l->len - nx);
                l->len -= nx - e->cx;
                e->modified = true;
            } else if (e->cy < e->n - 1) {
                line_t *nl = &e->lines[e->cy + 1];
                if (line_reserve(l, l->len + nl->len + 1)) {
                    memcpy(l->s + l->len, nl->s, nl->len);
                    l->len += nl->len;
                    delete_line(e, e->cy + 1);
                    e->modified = true;
                }
            }
        } else if ((c >= 32 && c < 127) || c == KEY_TAB) {
            if (line_reserve(l, l->len + 2)) {
                memmove(l->s + e->cx + 1, l->s + e->cx, l->len - e->cx);
                l->s[e->cx++] = c == KEY_TAB ? '\t' : (char)c;
                l->len++;
                e->want_col = disp_col(l, e->cx);
                e->modified = true;
            }
        }
    }
    free_all(e);
    free(e);
    return EDIT_OK;
}
