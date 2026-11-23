#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "pico/stdlib.h"
#include "lcd.h"
#include "kbd.h"
#include "ui.h"
#include "util.h"

#define DLG_W 36
#define DLG_X ((SCR_COLS - DLG_W) / 2)
#define MAX_WRAP 16

void ui_keybar(const char *const labels[10]) {
    int y = SCR_ROWS - 1;
    int x = 0;
    for (int i = 0; i < 10; i++) {
        scr_putc(x++, y, i == 9 ? '0' : '1' + i, C_WHITE, C_BLACK);
        const char *l = labels[i] ? labels[i] : "";
        scr_putsw(x, y, l, C_BLACK, C_CYAN, 3);
        x += 3;
    }
}

/* Splits msg into display lines no wider than w. Returns number of lines. */
static int wrap_text(const char *msg, int w, char lines[][DLG_W + 1], int maxl) {
    int n = 0;
    char tmp[512];
    utf8_to_disp_ml(tmp, sizeof(tmp), msg ? msg : "");
    const char *p = tmp;
    while (*p && n < maxl) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        if (len > w) {
            int cut = w;
            for (int i = w; i > w / 2; i--)
                if (p[i] == ' ') { cut = i; break; }
            memcpy(lines[n], p, cut);
            lines[n++][cut] = 0;
            p += cut;
            if (*p == ' ') p++;
            continue;
        }
        memcpy(lines[n], p, len);
        lines[n++][len] = 0;
        p += len;
        if (*p == '\n') p++;
    }
    if (!n) { lines[0][0] = 0; n = 1; }
    return n;
}

static void draw_frame(int x, int y, int w, int h, const char *title, uint8_t fg, uint8_t bg) {
    scr_box(x, y, w, h, fg, bg);
    if (title && *title) {
        char t[DLG_W];
        utf8_to_disp(t, sizeof(t), title);
        int tl = strlen(t);
        if (tl > w - 4) tl = w - 4;
        int tx = x + (w - tl - 2) / 2;
        scr_putc(tx, y, ' ', fg, bg);
        scr_puts(tx + 1, y, t, fg, bg, tl);
        scr_putc(tx + 1 + tl, y, ' ', fg, bg);
    }
}

int ui_choice(const char *title, const char *msg, const char *const *opts, int n, int def, bool danger) {
    void *snap = scr_save();
    char lines[MAX_WRAP][DLG_W + 1];
    int nl = wrap_text(msg, DLG_W - 4, lines, MAX_WRAP);
    int h = nl + 4;
    int y = (SCR_ROWS - h) / 2;
    uint8_t fg = danger ? C_WHITE : C_BLACK, bg = danger ? C_RED : C_LGRAY;
    int sel = def, result = -1;

    for (;;) {
        draw_frame(DLG_X, y, DLG_W, h, title, fg, bg);
        for (int i = 0; i < nl; i++) scr_puts(DLG_X + 2, y + 1 + i, lines[i], fg, bg, DLG_W - 4);
        int total = 0;
        for (int i = 0; i < n; i++) total += strlen(opts[i]) + 4 + (i ? 1 : 0);
        int bx = DLG_X + (DLG_W - total) / 2;
        for (int i = 0; i < n; i++) {
            uint8_t bf = (i == sel) ? C_BLACK : fg, bb = (i == sel) ? C_CYAN : bg;
            char b[32];
            snprintf(b, sizeof(b), "[ %s ]", opts[i]);
            bx += scr_puts(bx, y + h - 2, b, bf, bb, -1) + 1;
        }
        scr_set_cursor(0, 0, false);
        scr_flush();

        int k = kbd_wait(NULL);
        int c = KEY_CODE(k);
        if (c == KEY_ESC) break;
        if (c == KEY_ENTER) { result = sel; break; }
        if (c == KEY_LEFT || c == KEY_UP) sel = (sel + n - 1) % n;
        else if (c == KEY_RIGHT || c == KEY_DOWN || c == KEY_TAB) sel = (sel + 1) % n;
        else if (c < 128 && isalpha(c)) {
            for (int i = 0; i < n; i++)
                if (tolower((unsigned char)opts[i][0]) == tolower(c)) { result = i; goto done; }
        }
    }
done:
    scr_restore(snap);
    return result;
}

void ui_message(const char *title, const char *msg) {
    static const char *const ok[] = {"OK"};
    ui_choice(title, msg, ok, 1, 0, false);
}

void ui_error(const char *title, const char *msg) {
    static const char *const ok[] = {"OK"};
    ui_choice(title, msg, ok, 1, 0, true);
}

bool ui_confirm(const char *title, const char *msg) {
    static const char *const yn[] = {"Yes", "No"};
    return ui_choice(title, msg, yn, 2, 0, false) == 0;
}

static int u8_next(const char *s, int i) {
    if (!s[i]) return i;
    i++;
    while ((s[i] & 0xC0) == 0x80) i++;
    return i;
}

static int u8_prev(const char *s, int i) {
    if (i <= 0) return 0;
    i--;
    while (i > 0 && (s[i] & 0xC0) == 0x80) i--;
    return i;
}

static int u8_cols(const char *s, int bytes) {
    int n = 0;
    for (int i = 0; i < bytes; i++)
        if ((s[i] & 0xC0) != 0x80) n++;
    return n;
}

bool ui_input(const char *title, const char *prompt, char *buf, size_t bufsize, bool masked) {
    void *snap = scr_save();
    char *work = malloc(bufsize);
    if (!work) { scr_restore(snap); return false; }
    strncpy(work, buf, bufsize - 1);
    work[bufsize - 1] = 0;

    char lines[MAX_WRAP][DLG_W + 1];
    int nl = wrap_text(prompt, DLG_W - 4, lines, 4);
    int h = nl + 5;
    int y = (SCR_ROWS - h) / 2;
    int fw = DLG_W - 4;
    int cur = strlen(work), scroll = 0;
    bool ok = false;

    for (;;) {
        draw_frame(DLG_X, y, DLG_W, h, title, C_BLACK, C_LGRAY);
        for (int i = 0; i < nl; i++) scr_puts(DLG_X + 2, y + 1 + i, lines[i], C_BLACK, C_LGRAY, fw);
        int fy = y + 1 + nl;
        int ccol = u8_cols(work, cur);
        if (ccol < scroll) scroll = ccol;
        if (ccol >= scroll + fw) scroll = ccol - fw + 1;
        scr_fill(DLG_X + 2, fy, fw, 1, ' ', C_BLACK, C_CYAN);
        int col = 0;
        for (int i = 0; work[i]; i = u8_next(work, i), col++) {
            if (col < scroll || col >= scroll + fw) continue;
            char ch = masked ? '*' : ((unsigned char)work[i] < 0x80 ? work[i] : '?');
            scr_putc(DLG_X + 2 + col - scroll, fy, ch, C_BLACK, C_CYAN);
        }
        scr_puts(DLG_X + 2, y + h - 2, "Enter=OK  Esc=Cancel", C_DGRAY, C_LGRAY, fw);
        scr_set_cursor(DLG_X + 2 + ccol - scroll, fy, true);
        scr_flush();

        int k = kbd_wait(NULL);
        int c = KEY_CODE(k);
        int len = strlen(work);
        if (c == KEY_ESC) break;
        if (c == KEY_ENTER) { ok = true; break; }
        if (k & KM_CTRL) {
            if (c == 'u' || c == 'y') { work[0] = 0; cur = 0; }
            continue;
        }
        switch (c) {
        case KEY_LEFT: cur = u8_prev(work, cur); break;
        case KEY_RIGHT: cur = u8_next(work, cur); break;
        case KEY_HOME: case KEY_UP: cur = 0; break;
        case KEY_END: case KEY_DOWN: cur = len; break;
        case KEY_BACKSPACE:
            if (cur > 0) {
                int p = u8_prev(work, cur);
                memmove(work + p, work + cur, len - cur + 1);
                cur = p;
            }
            break;
        case KEY_DEL:
            if (cur < len) {
                int nx = u8_next(work, cur);
                memmove(work + cur, work + nx, len - nx + 1);
            }
            break;
        default:
            if (c >= 32 && c < 127 && len + 1 < (int)bufsize) {
                memmove(work + cur + 1, work + cur, len - cur + 1);
                work[cur++] = (char)c;
            }
        }
    }
    if (ok) strcpy(buf, work);
    free(work);
    scr_restore(snap);
    return ok;
}

int ui_menu(const char *title, const char *const *items, int n, int sel) {
    void *snap = scr_save();
    int maxvis = SCR_ROWS - 6;
    int vis = n < maxvis ? n : maxvis;
    int w = 32, x = (SCR_COLS - w) / 2;
    int h = vis + 2;
    int y = (SCR_ROWS - h) / 2;
    int top = 0, result = -1;
    if (sel < 0 || sel >= n) sel = 0;

    for (;;) {
        if (sel < top) top = sel;
        if (sel >= top + vis) top = sel - vis + 1;
        draw_frame(x, y, w, h, title, C_BLACK, C_CYAN);
        for (int i = 0; i < vis; i++) {
            int idx = top + i;
            uint8_t fg = idx == sel ? C_WHITE : C_BLACK, bg = idx == sel ? C_BLACK : C_CYAN;
            char left[40] = "", right[16] = "";
            const char *tab = strchr(items[idx], '\t');
            if (tab) {
                int ll = tab - items[idx];
                if (ll > 39) ll = 39;
                memcpy(left, items[idx], ll);
                left[ll] = 0;
                snprintf(right, sizeof(right), "%s", tab + 1);
            } else {
                snprintf(left, sizeof(left), "%s", items[idx]);
            }
            scr_fill(x + 1, y + 1 + i, w - 2, 1, ' ', fg, bg);
            scr_puts(x + 2, y + 1 + i, left, fg, bg, w - 4);
            int rl = strlen(right);
            scr_puts(x + w - 2 - rl, y + 1 + i, right, fg, bg, rl);
        }
        if (top > 0) scr_putc(x + w - 1, y + 1, G_UP, C_BLACK, C_CYAN);
        if (top + vis < n) scr_putc(x + w - 1, y + h - 2, G_DOWN, C_BLACK, C_CYAN);
        scr_set_cursor(0, 0, false);
        scr_flush();

        int c = KEY_CODE(kbd_wait(NULL));
        if (c == KEY_ESC || c == KEY_F9 || c == KEY_BACKSPACE) break;
        if (c == KEY_ENTER) { result = sel; break; }
        if (c == KEY_UP) sel = (sel + n - 1) % n;
        else if (c == KEY_DOWN) sel = (sel + 1) % n;
        else if (c == KEY_PGUP) sel = sel - vis < 0 ? 0 : sel - vis;
        else if (c == KEY_PGDN) sel = sel + vis >= n ? n - 1 : sel + vis;
        else if (c == KEY_HOME) sel = 0;
        else if (c == KEY_END) sel = n - 1;
    }
    scr_restore(snap);
    return result;
}

void ui_textpage(const char *title, const char *text) {
    void *snap = scr_save();
    int nlines = 1;
    for (const char *p = text; *p; p++)
        if (*p == '\n') nlines++;
    const char **lines = malloc(sizeof(char *) * nlines);
    if (!lines) { scr_restore(snap); return; }
    int i = 0;
    lines[i++] = text;
    for (const char *p = text; *p; p++)
        if (*p == '\n') lines[i++] = p + 1;

    int vis = SCR_ROWS - 2, top = 0;
    for (;;) {
        scr_fill(0, 0, SCR_COLS, SCR_ROWS, ' ', C_BLACK, C_CYAN);
        draw_frame(0, 0, SCR_COLS, SCR_ROWS, title, C_BLACK, C_CYAN);
        for (int r = 0; r < vis && top + r < nlines; r++) {
            const char *s = lines[top + r];
            const char *e = strchr(s, '\n');
            int len = e ? (int)(e - s) : (int)strlen(s);
            if (len > SCR_COLS - 2) len = SCR_COLS - 2;
            for (int j = 0; j < len; j++) scr_putc(1 + j, 1 + r, s[j], C_BLACK, C_CYAN);
        }
        if (top > 0) scr_putc(SCR_COLS - 1, 1, G_UP, C_BLACK, C_CYAN);
        if (top + vis < nlines) scr_putc(SCR_COLS - 1, SCR_ROWS - 2, G_DOWN, C_BLACK, C_CYAN);
        scr_set_cursor(0, 0, false);
        scr_flush();
        int c = KEY_CODE(kbd_wait(NULL));
        int maxtop = nlines > vis ? nlines - vis : 0;
        if (c == KEY_UP && top > 0) top--;
        else if (c == KEY_DOWN && top < maxtop) top++;
        else if (c == KEY_PGUP) top = top > vis ? top - vis : 0;
        else if (c == KEY_PGDN || c == ' ') top = top + vis < maxtop ? top + vis : maxtop;
        else if (c == KEY_HOME) top = 0;
        else if (c == KEY_END) top = maxtop;
        else if (c == KEY_ESC || c == KEY_ENTER || c == KEY_F10 || c == KEY_F1 || c == 'q') break;
    }
    free(lines);
    scr_restore(snap);
}

/* ---- Progress dialog ---- */

static const char spinner[] = "/-\\|";
static void *prog_snap;
static int prog_phase;
static char prog_title[DLG_W];
static char prog_l1[DLG_W + 1], prog_l2[DLG_W + 1];
static absolute_time_t prog_next_draw, prog_next_key;
static bool prog_cancel;

#define PROG_H 7
#define PROG_Y ((SCR_ROWS - PROG_H) / 2)

static void prog_draw(void) {
    draw_frame(DLG_X, PROG_Y, DLG_W, PROG_H, prog_title, C_BLACK, C_LGRAY);
    scr_putsw(DLG_X + 2, PROG_Y + 1, prog_l1, C_BLACK, C_LGRAY, DLG_W - 4);
    scr_putsw(DLG_X + 2, PROG_Y + 2, prog_l2, C_BLACK, C_LGRAY, DLG_W - 4);
    scr_puts(DLG_X + 2, PROG_Y + 4, "Working ", C_BLACK, C_LGRAY, -1);
    scr_putc(DLG_X + 10, PROG_Y + 4, spinner[prog_phase & 3], C_BLUE, C_LGRAY);
    scr_puts(DLG_X + DLG_W - 12, PROG_Y + 4, "Esc=Cancel", C_DGRAY, C_LGRAY, -1);
    scr_set_cursor(0, 0, false);
    scr_flush();
}

void ui_progress_begin(const char *title) {
    prog_snap = scr_save();
    utf8_to_disp(prog_title, sizeof(prog_title), title);
    prog_l1[0] = prog_l2[0] = 0;
    prog_phase = 0;
    prog_cancel = false;
    prog_next_draw = make_timeout_time_ms(120);
    prog_next_key = make_timeout_time_ms(300);
    prog_draw();
}

void ui_progress_set(const char *line1, const char *line2) {
    if (line1) utf8_to_disp(prog_l1, sizeof(prog_l1), line1);
    if (line2) utf8_to_disp(prog_l2, sizeof(prog_l2), line2);
    ui_progress_tick();
}

void ui_progress_tick(void) {
    if (absolute_time_diff_us(get_absolute_time(), prog_next_draw) > 0) return;
    prog_phase++;
    prog_next_draw = make_timeout_time_ms(120);
    prog_draw();
}

bool ui_progress_cancelled(void) {
    if (prog_cancel) return true;
    if (absolute_time_diff_us(get_absolute_time(), prog_next_key) > 0) return false;
    prog_next_key = make_timeout_time_ms(300);
    if (kbd_esc_pressed()) prog_cancel = true;
    return prog_cancel;
}

void ui_progress_end(void) {
    scr_restore(prog_snap);
    prog_snap = NULL;
}
