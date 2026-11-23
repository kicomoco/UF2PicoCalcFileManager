#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/watchdog.h"
#include "ff.h"
#include "lcd.h"
#include "kbd.h"
#include "sdcard.h"
#include "ui.h"
#include "util.h"
#include "panel.h"
#include "fileops.h"
#include "editor.h"
#include "hexedit.h"
#include "config.h"
#include "net.h"
#include "updater.h"

#ifndef FM_VERSION
#define FM_VERSION "0.0.0"
#endif
#ifndef FM_GITHUB_REPO
#define FM_GITHUB_REPO "kicomoco/UF2PicoCalcFileManager"
#endif

static FATFS g_fs;
static panel_t panels[2];
static int active;
static bool cfg_dirty;
static absolute_time_t last_key, next_status;
static int batt_pct = -1;
static bool batt_chg;

#define ACT (&panels[active])
#define OTH (&panels[!active])

/* ------------------------------------------------------------ helpers */

static void mark_paths_dirty(void) {
    strcpy(g_cfg.path_a, panels[0].path);
    strcpy(g_cfg.path_b, panels[1].path);
    cfg_dirty = true;
}

static void save_config_now(void) {
    mark_paths_dirty();
    g_cfg.sort_mode = g_sort_mode;
    g_cfg.show_hidden = g_show_hidden;
    config_save();
    cfg_dirty = false;
}

/* Loads the panel directory, walking up to the root if it no longer exists. */
static void panel_reload(panel_t *p, const char *keep) {
    while (panel_load(p, keep) != FR_OK && !panel_is_root(p)) {
        path_parent(p->path);
        keep = NULL;
    }
}

static void reload_all(void) {
    panel_reload(&panels[0], NULL);
    panel_reload(&panels[1], NULL);
}

/* Resolves `in` (absolute or relative to base) into a normalised absolute path. */
static bool resolve_path(const char *base, const char *in, char *out, size_t n) {
    char tmp[MAX_PATH * 2];
    if (in[0] == '/') snprintf(tmp, sizeof(tmp), "%s", in);
    else snprintf(tmp, sizeof(tmp), "%s/%s", base, in);
    size_t o = 0;
    out[0] = 0;
    char *save = NULL;
    for (char *tok = strtok_r(tmp, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        if (!strcmp(tok, ".")) continue;
        if (!strcmp(tok, "..")) {
            while (o > 0 && out[o - 1] != '/') o--;
            if (o > 0) o--;
            out[o] = 0;
            continue;
        }
        size_t l = strlen(tok);
        if (o + l + 2 > n) return false;
        out[o++] = '/';
        memcpy(out + o, tok, l);
        o += l;
        out[o] = 0;
    }
    if (o == 0) { out[0] = '/'; out[1] = 0; }
    return true;
}

static void disp_name(char *out, size_t n, const char *name) {
    utf8_to_disp(out, n, name);
}

static bool name_valid(const char *s) {
    if (!*s || !strcmp(s, ".") || !strcmp(s, "..")) return false;
    for (; *s; s++)
        if (strchr("/\\:*?\"<>|", *s)) return false;
    return true;
}

/* -------------------------------------------------------------- drawing */

static void fmt_date(char *out, size_t n, uint16_t d, uint16_t t) {
    snprintf(out, n, "%04d-%02d-%02d %02d:%02d", 1980 + (d >> 9), (d >> 5) & 15, d & 31, t >> 11, (t >> 5) & 63);
}

static void draw_title(void) {
    char t[SCR_COLS + 1];
    snprintf(t, sizeof(t), " PicoCalc FM v%s", FM_VERSION);
    scr_putsw(0, 0, t, C_BLACK, C_CYAN, SCR_COLS);
    char b[16];
    if (!sd_card_present()) snprintf(b, sizeof(b), "NO SD ");
    else if (batt_pct >= 0) snprintf(b, sizeof(b), "%s%d%% ", batt_chg ? "+" : "", batt_pct);
    else b[0] = 0;
    int l = strlen(b);
    scr_puts(SCR_COLS - l, 0, b, (!sd_card_present() || (batt_pct >= 0 && batt_pct < 15)) ? C_RED : C_BLACK, C_CYAN, l);
}

static void draw_path_label(int y, const panel_t *p, int idx, bool act) {
    char path[MAX_PATH], lbl[SCR_COLS + 1];
    disp_name(path, sizeof(path), p->path);
    int max = SCR_COLS - 8;
    int pl = strlen(path);
    const char *ps = path;
    if (pl > max) ps = path + pl - max + 2;
    snprintf(lbl, sizeof(lbl), " %c:%s%s ", 'A' + idx, pl > max ? ".." : "", ps);
    scr_puts(2, y, lbl, act ? C_BLACK : C_WHITE, act ? C_CYAN : C_BLUE, SCR_COLS - 4);
}

static void draw_entry_row(int y, const entry_t *e, bool cursor) {
    char name[SCR_COLS + 1], sz[12];
    uint8_t fg, bg = C_BLUE;
    if (e->tagged) fg = C_YELLOW;
    else if (e->is_dir) fg = C_WHITE;
    else {
        const char *x = strrchr(e->name, '.');
        fg = (x && !strcasecmp(x, ".uf2")) ? C_LGREEN : C_LGRAY;
    }
    if (cursor) { bg = C_CYAN; fg = e->tagged ? C_YELLOW : C_BLACK; }
    int w = SCR_COLS - 2;
    if (e->is_dir) {
        name[0] = '/';
        disp_name(name + 1, sizeof(name) - 1, e->name);
        snprintf(sz, sizeof(sz), !strcmp(e->name, "..") ? "UP--DIR" : "<DIR>");
    } else {
        disp_name(name, sizeof(name), e->name);
        fmt_size(sz, sizeof(sz), e->size);
    }
    int szw = strlen(sz);
    int nw = w - 8;
    scr_fill(1, y, w, 1, ' ', fg, bg);
    int nl = strlen(name);
    if (nl > nw) {
        scr_puts(1, y, name, fg, bg, nw - 1);
        scr_putc(nw, y, '~', C_LRED, bg);
    } else scr_puts(1, y, name, fg, bg, nw);
    scr_puts(1 + w - szw, y, sz, fg, bg, szw);
    scr_putc(1 + nw, y, G_VLINE, C_LGRAY, bg);
}

/* Draws a full panel box at rows y..y+h-1. */
static void draw_panel_box(int y, int h, panel_t *p, int idx, bool act) {
    uint8_t bfg = act ? C_WHITE : C_LGRAY;
    scr_box(0, y, SCR_COLS, h, bfg, C_BLUE);
    draw_path_label(y, p, idx, act);
    int rows = h - 2;
    if (p->sel < p->top) p->top = p->sel;
    if (p->sel >= p->top + rows) p->top = p->sel - rows + 1;
    if (p->top > 0 && p->top > p->count - rows) p->top = p->count - rows > 0 ? p->count - rows : 0;
    for (int r = 0; r < rows; r++) {
        int i = p->top + r;
        int yy = y + 1 + r;
        if (i >= p->count) {
            scr_fill(1, yy, SCR_COLS - 2, 1, ' ', C_LGRAY, C_BLUE);
            scr_putc(1 + SCR_COLS - 2 - 8, yy, G_VLINE, C_LGRAY, C_BLUE);
            continue;
        }
        draw_entry_row(yy, &p->ents[i], act && i == p->sel);
    }
    if (p->count == 0 && p->err) {
        char m[40];
        snprintf(m, sizeof(m), "Error: %s", fr_str(p->err));
        scr_puts(2, y + 1, m, C_LRED, C_BLUE, SCR_COLS - 4);
    }
    if (p->top > 0) scr_putc(SCR_COLS - 1, y + 1, G_UP, C_YELLOW, C_BLUE);
    if (p->top + rows < p->count) scr_putc(SCR_COLS - 1, y + h - 2, G_DOWN, C_YELLOW, C_BLUE);

    /* Bottom border: info line. */
    char info[SCR_COLS + 1];
    uint8_t ifg = C_WHITE;
    if (p->ntagged) {
        char b[12];
        fmt_size(b, sizeof(b), panel_tagged_bytes(p));
        snprintf(info, sizeof(info), " %d tagged, %s ", p->ntagged, b);
        ifg = C_YELLOW;
    } else if (act) {
        entry_t *e = panel_cur(p);
        if (e && strcmp(e->name, "..")) {
            char d[20];
            fmt_date(d, sizeof(d), e->fdate, e->ftime);
            if (e->is_dir) snprintf(info, sizeof(info), " %s ", d);
            else {
                char b[24];
                snprintf(b, sizeof(b), "%llu", (unsigned long long)e->size);
                snprintf(info, sizeof(info), " %s  %s%s ", b, d, (e->attr & AM_RDO) ? " RO" : "");
            }
        } else snprintf(info, sizeof(info), " %d items ", p->count - (panel_is_root(p) ? 0 : 1));
    } else snprintf(info, sizeof(info), " %d items ", p->count - (panel_is_root(p) ? 0 : 1));
    int il = strlen(info);
    if (il > SCR_COLS - 4) il = SCR_COLS - 4;
    scr_puts((SCR_COLS - il) / 2, y + h - 1, info, ifg, C_BLUE, il);
}

static void draw_collapsed_summary(int y, panel_t *p, int idx) {
    char path[MAX_PATH], s[SCR_COLS + 1];
    disp_name(path, sizeof(path), p->path);
    int max = SCR_COLS - 12;
    int pl = strlen(path);
    snprintf(s, sizeof(s), "%c%c:%s%s", G_DOWN, 'A' + idx, pl > max ? ".." : "", pl > max ? path + pl - max + 2 : path);
    scr_putsw(0, y, s, C_LGRAY, C_DGRAY, SCR_COLS);
    char t[12];
    snprintf(t, sizeof(t), "Tab%c", G_TAB);
    scr_puts(SCR_COLS - 4, y, t, C_LCYAN, C_DGRAY, 4);
}

static const char *const main_keys[10] = {"Hlp", "Ren", "Viw", "Edt", "Cpy", "Mov", "Dir", "Del", "Mnu", "Hex"};

static void draw_main(void) {
    draw_title();
    if (g_cfg.split_view) {
        int h = (SCR_ROWS - 2) / 2;
        draw_panel_box(1, h, &panels[0], 0, active == 0);
        draw_panel_box(1 + h, SCR_ROWS - 2 - h, &panels[1], 1, active == 1);
    } else {
        draw_collapsed_summary(1, OTH, !active);
        draw_panel_box(2, SCR_ROWS - 3, ACT, active, true);
    }
    ui_keybar(main_keys);
    scr_set_cursor(0, 0, false);
    scr_flush();
}

static int panel_rows(void) {
    return g_cfg.split_view ? (SCR_ROWS - 2) / 2 - 2 : SCR_ROWS - 5;
}

static void idle(void) {
    if (time_reached(next_status)) {
        next_status = make_timeout_time_ms(5000);
        batt_pct = kbd_battery(&batt_chg);
        draw_title();
        scr_flush();
    }
    if (cfg_dirty && absolute_time_diff_us(last_key, get_absolute_time()) > 3000000) {
        save_config_now();
    }
}

/* -------------------------------------------------------------- mounting */

static void mount_card(void) {
    for (;;) {
        FRESULT fr = FR_NOT_READY;
        if (sd_card_present()) fr = f_mount(&g_fs, "", 1);
        if (fr == FR_OK) return;
        scr_clear(C_LGRAY, C_BLUE);
        draw_title();
        char m[120];
        if (!sd_card_present()) snprintf(m, sizeof(m), "No SD card inserted.\n\nInsert a card and press any key.");
        else snprintf(m, sizeof(m), "Could not mount SD card:\n%s\n\nFAT32/exFAT required.\nPress any key to retry.", fr_str(fr));
        ui_message("SD card", m);
    }
}

static void remount(void) {
    f_unmount("");
    mount_card();
    reload_all();
}

/* ---------------------------------------------------------- operations */

/* Collects the names to operate on: tagged entries or the current one. */
static int collect_sources(panel_t *p, char ***out) {
    int n = 0;
    char **v = malloc(sizeof(char *) * (p->ntagged ? p->ntagged : 1));
    if (!v) return 0;
    if (p->ntagged) {
        for (int i = 0; i < p->count; i++)
            if (p->ents[i].tagged) v[n++] = p->ents[i].name;
    } else {
        entry_t *e = panel_cur(p);
        if (e && strcmp(e->name, "..")) v[n++] = e->name;
    }
    if (!n) { free(v); v = NULL; }
    *out = v;
    return n;
}

static void describe_sources(char *out, size_t n, char **names, int cnt) {
    if (cnt == 1) {
        char d[40];
        disp_name(d, sizeof(d), names[0]);
        if (strlen(d) > 30) strcpy(d + 27, "...");
        snprintf(out, n, "\"%s\"", d);
    } else snprintf(out, n, "%d items", cnt);
}

static void op_copy_move(bool move) {
    panel_t *src = ACT;
    char **names;
    int cnt = collect_sources(src, &names);
    if (!cnt) return;
    char what[60], prompt[80], dest[MAX_PATH];
    describe_sources(what, sizeof(what), names, cnt);
    snprintf(prompt, sizeof(prompt), "%s %s to:", move ? "Move" : "Copy", what);
    strcpy(dest, OTH->path);
    if (!ui_input(move ? "Move" : "Copy", prompt, dest, sizeof(dest), false) || !dest[0]) { free(names); return; }

    char target[MAX_PATH];
    if (!resolve_path(src->path, dest, target, sizeof(target))) { ui_error("Error", "Path too long"); free(names); return; }
    FILINFO fi;
    bool is_dir = fo_stat(target, &fi) && (fi.fattrib & AM_DIR);
    size_t dl = strlen(dest);
    if (!is_dir && (cnt > 1 || dest[dl - 1] == '/')) {
        char m[MAX_PATH + 40], dn[60];
        disp_name(dn, sizeof(dn), target);
        snprintf(m, sizeof(m), "Directory does not exist:\n%s\nCreate it?", dn);
        if (!ui_confirm("Target", m)) { free(names); return; }
        FRESULT fr = f_mkdir(target);
        if (fr != FR_OK) { ui_error("Create directory", fr_str(fr)); free(names); return; }
        is_dir = true;
    }

    fo_ctx_t ctx;
    fo_ctx_init(&ctx);
    ui_progress_begin(move ? "Move" : "Copy");
    char sp[MAX_PATH], dp[MAX_PATH];
    if (!move) {
        ui_progress_set("Counting...", "");
        for (int i = 0; i < cnt; i++) {
            path_join(sp, sizeof(sp), src->path, names[i]);
            ctx.bytes_total += fo_tree_size(sp);
        }
    }
    for (int i = 0; i < cnt && !ctx.cancelled; i++) {
        path_join(sp, sizeof(sp), src->path, names[i]);
        if (is_dir) path_join(dp, sizeof(dp), target, names[i]);
        else strcpy(dp, target);
        int r = move ? fo_move(&ctx, sp, dp) : fo_copy(&ctx, sp, dp);
        if (r == FO_CANCEL) break;
    }
    ui_progress_end();
    free(names);
    if (!move && !ctx.cancelled && !ctx.errors) panel_tag_all(src, 0);
    reload_all();
}

static void op_delete(void) {
    panel_t *p = ACT;
    char **names;
    int cnt = collect_sources(p, &names);
    if (!cnt) return;
    char what[60], msg[120];
    describe_sources(what, sizeof(what), names, cnt);
    bool any_dir = false;
    for (int i = 0; i < p->count; i++)
        if ((p->ntagged ? p->ents[i].tagged : i == p->sel) && p->ents[i].is_dir) any_dir = true;
    snprintf(msg, sizeof(msg), "Delete %s?%s", what, any_dir ? "\n\nDirectories are deleted\nwith ALL their contents!" : "");
    static const char *const o[] = {"Delete", "Cancel"};
    if (ui_choice("Delete", msg, o, 2, 1, true) != 0) { free(names); return; }
    fo_ctx_t ctx;
    fo_ctx_init(&ctx);
    ui_progress_begin("Delete");
    char sp[MAX_PATH];
    for (int i = 0; i < cnt && !ctx.cancelled; i++) {
        path_join(sp, sizeof(sp), p->path, names[i]);
        if (fo_delete(&ctx, sp) == FO_CANCEL) break;
    }
    ui_progress_end();
    free(names);
    reload_all();
}

static void op_rename(void) {
    panel_t *p = ACT;
    entry_t *e = panel_cur(p);
    if (!e || !strcmp(e->name, "..")) return;
    char nn[256], dn[40];
    strncpy(nn, e->name, sizeof(nn) - 1);
    nn[sizeof(nn) - 1] = 0;
    disp_name(dn, sizeof(dn), e->name);
    char prompt[80];
    snprintf(prompt, sizeof(prompt), "Rename \"%.28s\" to:", dn);
    if (!ui_input("Rename", prompt, nn, sizeof(nn), false) || !strcmp(nn, e->name)) return;
    if (!name_valid(nn)) { ui_error("Rename", "Invalid name"); return; }
    char from[MAX_PATH], to[MAX_PATH];
    path_join(from, sizeof(from), p->path, e->name);
    path_join(to, sizeof(to), p->path, nn);
    FRESULT fr = f_rename(from, to);
    if (fr != FR_OK) { ui_error("Rename", fr == FR_EXIST ? "A file with that name exists" : fr_str(fr)); return; }
    panel_reload(p, nn);
    panel_reload(OTH, NULL);
}

static void op_mkdir(void) {
    char nn[256] = "";
    if (!ui_input("Make directory", "New directory name:", nn, sizeof(nn), false) || !nn[0]) return;
    if (!name_valid(nn)) { ui_error("Make directory", "Invalid name"); return; }
    char path[MAX_PATH];
    path_join(path, sizeof(path), ACT->path, nn);
    FRESULT fr = f_mkdir(path);
    if (fr != FR_OK) { ui_error("Make directory", fr == FR_EXIST ? "Already exists" : fr_str(fr)); return; }
    panel_reload(ACT, nn);
    panel_reload(OTH, NULL);
}

static void op_newfile(void) {
    char nn[256] = "";
    if (!ui_input("New file", "New file name:", nn, sizeof(nn), false) || !nn[0]) return;
    if (!name_valid(nn)) { ui_error("New file", "Invalid name"); return; }
    char path[MAX_PATH];
    path_join(path, sizeof(path), ACT->path, nn);
    FILINFO fi;
    if (fo_stat(path, &fi)) { ui_error("New file", "Already exists"); return; }
    static const char *const o[] = {"Text", "Hex", "Cancel"};
    int k = ui_choice("New file", "Open it in which editor?", o, 3, 0, false);
    if (k == 0) editor_open(path, false);
    else if (k == 1) {
        FIL f;
        FRESULT fr = f_open(&f, path, FA_WRITE | FA_CREATE_NEW);
        if (fr != FR_OK) { ui_error("New file", fr_str(fr)); return; }
        f_close(&f);
        hexedit_open(path, false);
    } else return;
    scr_invalidate();
    panel_reload(ACT, nn);
    panel_reload(OTH, NULL);
}

static bool looks_binary(const char *path) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return false;
    uint8_t buf[512];
    UINT br = 0;
    f_read(&f, buf, sizeof(buf), &br);
    f_close(&f);
    for (UINT i = 0; i < br; i++)
        if (buf[i] == 0) return true;
    return false;
}

static void op_open_file(bool edit, bool force_hex) {
    entry_t *e = panel_cur(ACT);
    if (!e || e->is_dir) return;
    char path[MAX_PATH], keep[256];
    path_join(path, sizeof(path), ACT->path, e->name);
    strncpy(keep, e->name, sizeof(keep) - 1);
    keep[sizeof(keep) - 1] = 0;
    if (force_hex || (!edit && looks_binary(path))) hexedit_open(path, !edit);
    else {
        int r = editor_open(path, !edit);
        if (r == EDIT_TOO_LARGE) {
            if (!edit || ui_confirm("Too large", "File is too large for the text\neditor. Open in hex editor?"))
                hexedit_open(path, !edit);
        }
    }
    scr_invalidate();
    panel_reload(ACT, keep);
    panel_reload(OTH, NULL);
}

static void enter_dir(panel_t *p, const char *name) {
    char keep[256] = "";
    if (!strcmp(name, "..")) {
        if (panel_is_root(p)) return;
        strncpy(keep, path_base(p->path), sizeof(keep) - 1);
        path_parent(p->path);
    } else {
        char np[MAX_PATH];
        path_join(np, sizeof(np), p->path, name);
        if (strlen(np) >= MAX_PATH - 1) { ui_error("Error", "Path too long"); return; }
        strcpy(p->path, np);
    }
    p->sel = p->top = 0;
    panel_reload(p, keep);
    mark_paths_dirty();
}

/* ------------------------------------------------------------ settings */

static void wifi_settings(void) {
    static const char *const items[] = {"Scan for networks", "Enter network name", "Set password", "Forget network"};
    char msg[64];
    for (;;) {
        snprintf(msg, sizeof(msg), "Wi-Fi: %s", g_cfg.wifi_ssid[0] ? g_cfg.wifi_ssid : "(not set)");
        int k = ui_menu(msg, items, 4, 0);
        if (k < 0) return;
        bool ask_pass = false;
        if (k == 0) {
            char (*ss)[33] = calloc(24, 33);
            if (!ss) return;
            char err[64] = "";
            ui_progress_begin("Wi-Fi");
            int n = net_wifi_scan(ss, 24, err, sizeof(err));
            ui_progress_end();
            net_wifi_off();
            if (n < 0) { if (n != -2) ui_error("Scan", err); free(ss); continue; }
            if (n == 0) { ui_message("Scan", "No networks found"); free(ss); continue; }
            const char *list[24];
            char disp[24][33];
            for (int i = 0; i < n; i++) { disp_name(disp[i], 33, ss[i]); list[i] = disp[i]; }
            int s = ui_menu("Select network", list, n, 0);
            if (s >= 0) {
                strncpy(g_cfg.wifi_ssid, ss[s], sizeof(g_cfg.wifi_ssid) - 1);
                ask_pass = true;
            }
            free(ss);
        } else if (k == 1) {
            char b[64];
            strcpy(b, g_cfg.wifi_ssid);
            if (ui_input("Wi-Fi", "Network name (SSID):", b, sizeof(b), false)) {
                strcpy(g_cfg.wifi_ssid, b);
                ask_pass = true;
            }
        } else if (k == 2) ask_pass = true;
        else if (k == 3) {
            g_cfg.wifi_ssid[0] = g_cfg.wifi_pass[0] = 0;
            save_config_now();
        }
        if (ask_pass) {
            char b[80] = "";
            if (ui_input("Wi-Fi", "Password (empty for open network):", b, sizeof(b), true)) strcpy(g_cfg.wifi_pass, b);
            save_config_now();
            ui_message("Wi-Fi", "Settings saved.\n\nNote: stored in plain text in\n" CONFIG_PATH);
        }
    }
}

static const char help_text[] =
    "PicoCalc File Manager v" FM_VERSION "\n"
    "Midnight Commander style file manager\n"
    "\n"
    "NAVIGATION\n"
    "Up/Down       Move cursor\n"
    "Shift+Up/Dn   Page up / down\n"
    "Shift+Tab     First entry (Home)\n"
    "Shift+Del     Last entry (End)\n"
    "Enter         Open directory / view file\n"
    "Right         Enter directory\n"
    "Left/BkSpc    Parent directory\n"
    "Tab           Switch to other panel\n"
    "a-z 0-9       Jump to name starting with\n"
    "\n"
    "TAGGING (multi-select)\n"
    "Space         Tag / untag + move down\n"
    "+             Tag all files\n"
    "-             Untag all\n"
    "*             Invert tags\n"
    "\n"
    "FUNCTION KEYS (Shift+F1..F5=F6..F10)\n"
    "F1  Help        F6  Move / rename\n"
    "F2  Rename      F7  Make directory\n"
    "F3  View        F8  Delete\n"
    "F4  Edit text   F9  Menu\n"
    "F5  Copy        F10 Hex editor\n"
    "\n"
    "Copy/Move ask for a target. The other\n"
    "panel's path is filled in. An existing\n"
    "directory means 'into it'; otherwise\n"
    "it is the new name. Relative paths are\n"
    "relative to the current directory.\n"
    "\n"
    "CTRL KEYS\n"
    "Ctrl+N        New file\n"
    "Ctrl+L        Toggle split / single view\n"
    "Ctrl+U        Swap panels\n"
    "Ctrl+O        Other panel = this dir\n"
    "Ctrl+R        Refresh (remount SD)\n"
    "\n"
    "UPDATES\n"
    "F9 > Check for updates downloads the\n"
    "latest release from GitHub to\n"
    "/pico2-apps/picocalc_file_manager.uf2\n"
    "Configure Wi-Fi in the F9 menu first.\n"
    "\n"
    "UF2Loader: hold Up/F1/F5 while powering\n"
    "on to return to the loader menu.\n";

static void show_about(void) {
    ui_message("About",
               "PicoCalc File Manager\n"
               "Version " FM_VERSION "\n\n"
               "github.com/" FM_GITHUB_REPO "\n\n"
               "FatFs (c) ChaN, lwIP, Mbed TLS");
}

static void toggle_split(void) {
    g_cfg.split_view = !g_cfg.split_view;
    save_config_now();
}

static void swap_panels(void) {
    panel_t t = panels[0];
    panels[0] = panels[1];
    panels[1] = t;
    active = !active;
    mark_paths_dirty();
}

static void other_to_this(void) {
    strcpy(OTH->path, ACT->path);
    OTH->sel = OTH->top = 0;
    panel_reload(OTH, NULL);
    mark_paths_dirty();
}

static void main_menu(void) {
    static const char *const sort_names[] = {"Name", "Extension", "Size", "Date"};
    char sort_item[40], hid_item[40], split_item[40];
    snprintf(sort_item, sizeof(sort_item), "Sort order: %s", sort_names[g_sort_mode]);
    snprintf(hid_item, sizeof(hid_item), "Show hidden files: %s", g_show_hidden ? "on" : "off");
    snprintf(split_item, sizeof(split_item), "Split view: %s\tC-L", g_cfg.split_view ? "on" : "off");
    const char *items[] = {
        "New file\tC-N", "Hex edit file\tF10", split_item, "Swap panels\tC-U", "Other panel = this dir\tC-O",
        sort_item, hid_item, "Refresh / remount SD\tC-R", "Wi-Fi settings", "Check for updates",
        "Help\tF1", "About", "Restart device",
    };
    int k = ui_menu("Menu", items, sizeof(items) / sizeof(items[0]), 0);
    switch (k) {
    case 0: op_newfile(); break;
    case 1: op_open_file(true, true); break;
    case 2: toggle_split(); break;
    case 3: swap_panels(); break;
    case 4: other_to_this(); break;
    case 5: {
        int s = ui_menu("Sort by", sort_names, SORT_COUNT, g_sort_mode);
        if (s >= 0) {
            g_sort_mode = s;
            reload_all();
            save_config_now();
        }
        break;
    }
    case 6:
        g_show_hidden = !g_show_hidden;
        reload_all();
        save_config_now();
        break;
    case 7: remount(); break;
    case 8: wifi_settings(); break;
    case 9:
        save_config_now();
        updater_check();
        reload_all();
        break;
    case 10: ui_textpage("Help", help_text); break;
    case 11: show_about(); break;
    case 12:
        if (ui_confirm("Restart", "Restart the PicoCalc?")) {
            save_config_now();
            watchdog_reboot(0, 0, 10);
            for (;;) tight_loop_contents();
        }
        break;
    }
}

/* ------------------------------------------------------------ main loop */

static void jump_to_char(panel_t *p, int c) {
    c = tolower(c);
    for (int k = 1; k <= p->count; k++) {
        int i = (p->sel + k) % p->count;
        if (tolower((unsigned char)p->ents[i].name[0]) == c) { p->sel = i; return; }
    }
}

static void app_main(void) {
    lcd_init();
    scr_clear(C_LGRAY, C_BLUE);
    kbd_init();
    sd_hw_init();
    batt_pct = kbd_battery(&batt_chg);
    mount_card();
    config_load();
    g_sort_mode = (g_cfg.sort_mode >= 0 && g_cfg.sort_mode < SORT_COUNT) ? g_cfg.sort_mode : SORT_NAME;
    g_show_hidden = g_cfg.show_hidden;
    panel_init(&panels[0], g_cfg.path_a);
    panel_init(&panels[1], g_cfg.path_b);
    reload_all();
    next_status = make_timeout_time_ms(5000);
    last_key = get_absolute_time();

    for (;;) {
        draw_main();
        int k = kbd_wait(idle);
        last_key = get_absolute_time();
        int c = KEY_CODE(k);
        bool ctrl = k & KM_CTRL;
        panel_t *p = ACT;
        int rows = panel_rows();

        if (!sd_card_present()) {
            remount();
            continue;
        }

        if (ctrl) {
            switch (c) {
            case 'n': op_newfile(); break;
            case 'l': toggle_split(); break;
            case 'u': swap_panels(); break;
            case 'o': other_to_this(); break;
            case 'r': remount(); break;
            }
            continue;
        }

        switch (c) {
        case KEY_UP: if (p->sel > 0) p->sel--; break;
        case KEY_DOWN: if (p->sel < p->count - 1) p->sel++; break;
        case KEY_PGUP: p->sel -= rows - 1; if (p->sel < 0) p->sel = 0; break;
        case KEY_PGDN: p->sel += rows - 1; if (p->sel >= p->count) p->sel = p->count - 1; break;
        case KEY_HOME: p->sel = 0; break;
        case KEY_END: p->sel = p->count - 1; break;
        case KEY_TAB: active = !active; break;
        case KEY_ENTER: {
            entry_t *e = panel_cur(p);
            if (!e) break;
            if (e->is_dir) enter_dir(p, e->name);
            else op_open_file(false, false);
            break;
        }
        case KEY_RIGHT: {
            entry_t *e = panel_cur(p);
            if (e && e->is_dir && strcmp(e->name, "..")) enter_dir(p, e->name);
            break;
        }
        case KEY_LEFT:
        case KEY_BACKSPACE:
            enter_dir(p, "..");
            break;
        case ' ':
        case KEY_INSERT:
            panel_tag_toggle(p, p->sel);
            if (p->sel < p->count - 1) p->sel++;
            break;
        case '+': panel_tag_all(p, 1); break;
        case '-': panel_tag_all(p, 0); break;
        case '*': panel_tag_all(p, 2); break;
        case KEY_F1: ui_textpage("Help", help_text); break;
        case KEY_F2: op_rename(); break;
        case KEY_F3: op_open_file(false, false); break;
        case KEY_F4: op_open_file(true, false); break;
        case KEY_F5: op_copy_move(false); break;
        case KEY_F6: op_copy_move(true); break;
        case KEY_F7: op_mkdir(); break;
        case KEY_F8:
        case KEY_DEL: op_delete(); break;
        case KEY_F9: main_menu(); break;
        case KEY_F10: op_open_file(true, true); break;
        default:
            if (c < 128 && isalnum(c)) jump_to_char(p, c);
            else if (c == '.' || c == '_') jump_to_char(p, c);
            break;
        }
    }
}

/* The default RP2350 core 0 stack is small (4KB in SCRATCH_Y). TLS needs much more,
 * so the application runs on core 1 with a large static stack. */
static uint32_t core1_stack[32 * 1024 / 4];

static void core1_entry(void) {
    app_main();
}

int main(void) {
    multicore_launch_core1_with_stack(core1_entry, core1_stack, sizeof(core1_stack));
    for (;;) __wfi();
}
