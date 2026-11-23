#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "ff.h"
#include "fileops.h"
#include "ui.h"
#include "util.h"

#define COPY_BUF 16384

void fo_ctx_init(fo_ctx_t *c) {
    memset(c, 0, sizeof(*c));
}

bool fo_stat(const char *path, FILINFO *fi) {
    if (strcmp(path, "/") == 0) {
        memset(fi, 0, sizeof(*fi));
        fi->fattrib = AM_DIR;
        return true;
    }
    return f_stat(path, fi) == FR_OK;
}

static void show_progress(fo_ctx_t *c, const char *verb, const char *name) {
    char l1[64], l2[64], a[16], b[16];
    snprintf(l1, sizeof(l1), "%s %s", verb, name);
    fmt_size(a, sizeof(a), c->bytes_done);
    if (c->bytes_total) {
        fmt_size(b, sizeof(b), c->bytes_total);
        snprintf(l2, sizeof(l2), "%s / %s  (%d files)", a, b, c->files_done);
    } else {
        snprintf(l2, sizeof(l2), "%s  (%d files)", a, c->files_done);
    }
    ui_progress_set(l1, l2);
}

static bool check_cancel(fo_ctx_t *c) {
    if (!c->cancelled && ui_progress_cancelled()) {
        if (ui_confirm("Cancel", "Abort the operation?")) c->cancelled = true;
    }
    return c->cancelled;
}

/* Reports an error; returns FO_ERROR (continue) or FO_CANCEL. */
static int report_error(fo_ctx_t *c, const char *what, const char *path, int fr) {
    static const char *const opts[] = {"Skip", "Cancel"};
    char msg[300];
    char name[200];
    utf8_to_disp(name, sizeof(name), path);
    snprintf(msg, sizeof(msg), "%s failed:\n%s\n%s", what, name, fr_str(fr));
    c->errors++;
    if (ui_choice("Error", msg, opts, 2, 0, true) != 0) {
        c->cancelled = true;
        return FO_CANCEL;
    }
    return FO_ERROR;
}

/* Returns 1 to overwrite, 0 to skip, -1 to cancel. */
static int ask_conflict(fo_ctx_t *c, const char *dst) {
    if (c->conflict == CONFLICT_OVERWRITE_ALL) return 1;
    if (c->conflict == CONFLICT_SKIP_ALL) return 0;
    static const char *const opts[] = {"Yes", "No", "All", "Skip all"};
    char msg[300], name[200];
    utf8_to_disp(name, sizeof(name), path_base(dst));
    snprintf(msg, sizeof(msg), "Target exists:\n%s\nOverwrite?", name);
    switch (ui_choice("File exists", msg, opts, 4, 0, false)) {
    case 0: return 1;
    case 1: return 0;
    case 2: c->conflict = CONFLICT_OVERWRITE_ALL; return 1;
    case 3: c->conflict = CONFLICT_SKIP_ALL; return 0;
    default: c->cancelled = true; return -1;
    }
}

uint64_t fo_tree_size(const char *path) {
    FILINFO fi;
    if (!fo_stat(path, &fi)) return 0;
    if (!(fi.fattrib & AM_DIR)) return fi.fsize;
    uint64_t total = 0;
    DIR *d = malloc(sizeof(DIR));
    char *child = malloc(MAX_PATH);
    if (d && child && f_opendir(d, path) == FR_OK) {
        while (f_readdir(d, &fi) == FR_OK && fi.fname[0]) {
            if (fi.fattrib & AM_DIR) {
                path_join(child, MAX_PATH, path, fi.fname);
                total += fo_tree_size(child);
            } else {
                total += fi.fsize;
            }
            ui_progress_tick();
        }
        f_closedir(d);
    }
    free(d);
    free(child);
    return total;
}

static int copy_file(fo_ctx_t *c, const char *src, const char *dst) {
    FIL *fs = malloc(sizeof(FIL)), *fd = malloc(sizeof(FIL));
    uint8_t *buf = malloc(COPY_BUF);
    char *tmp = malloc(MAX_PATH);
    int ret = FO_OK;
    FRESULT fr;
    if (!fs || !fd || !buf || !tmp) { ret = report_error(c, "Copy", src, FR_NOT_ENOUGH_CORE); goto out; }
    if (snprintf(tmp, MAX_PATH, "%s.~cp", dst) >= MAX_PATH) { ret = report_error(c, "Copy", dst, FR_INVALID_NAME); goto out; }

    if ((fr = f_open(fs, src, FA_READ)) != FR_OK) { ret = report_error(c, "Open", src, fr); goto out; }
    if ((fr = f_open(fd, tmp, FA_WRITE | FA_CREATE_ALWAYS)) != FR_OK) {
        f_close(fs);
        ret = report_error(c, "Create", dst, fr);
        goto out;
    }
    const char *name = path_base(src);
    show_progress(c, "Copy", name);
    for (;;) {
        UINT br = 0, bw = 0;
        if ((fr = f_read(fs, buf, COPY_BUF, &br)) != FR_OK) { ret = report_error(c, "Read", src, fr); break; }
        if (br == 0) break;
        if ((fr = f_write(fd, buf, br, &bw)) != FR_OK || bw < br) {
            ret = report_error(c, "Write", dst, fr == FR_OK ? FR_DENIED : fr);
            break;
        }
        c->bytes_done += br;
        show_progress(c, "Copy", name);
        if (check_cancel(c)) { ret = FO_CANCEL; break; }
    }
    f_close(fs);
    fr = f_close(fd);
    if (ret == FO_OK && fr != FR_OK) ret = report_error(c, "Write", dst, fr);
    if (ret == FO_OK) {
        /* Only now replace any existing destination. */
        FILINFO di;
        if (f_stat(dst, &di) == FR_OK) {
            if (di.fattrib & AM_RDO) f_chmod(dst, 0, AM_RDO);
            if ((fr = f_unlink(dst)) != FR_OK) ret = report_error(c, "Replace", dst, fr);
        }
    }
    if (ret == FO_OK && (fr = f_rename(tmp, dst)) != FR_OK) ret = report_error(c, "Rename", dst, fr);
    if (ret != FO_OK) {
        f_unlink(tmp);
    } else {
        FILINFO fi;
        if (f_stat(src, &fi) == FR_OK) f_utime(dst, &fi);
        c->files_done++;
    }
out:
    free(fs);
    free(fd);
    free(buf);
    free(tmp);
    return ret;
}

int fo_copy(fo_ctx_t *c, const char *src, const char *dst) {
    FILINFO si, di;
    if (c->cancelled) return FO_CANCEL;
    if (!fo_stat(src, &si)) return report_error(c, "Copy", src, FR_NO_FILE);
    if (strcasecmp(src, dst) == 0) return report_error(c, "Copy onto itself", src, FR_EXIST);
    bool dexists = fo_stat(dst, &di);

    if (!(si.fattrib & AM_DIR)) {
        if (dexists) {
            if (di.fattrib & AM_DIR) return report_error(c, "Copy (target is a directory)", dst, FR_EXIST);
            int a = ask_conflict(c, dst);
            if (a < 0) return FO_CANCEL;
            if (a == 0) { c->bytes_done += si.fsize; return FO_SKIPPED; }
        }
        return copy_file(c, src, dst);
    }

    if (path_is_within(dst, src)) return report_error(c, "Copy into itself", dst, FR_DENIED);
    if (dexists && !(di.fattrib & AM_DIR)) return report_error(c, "Copy (target is a file)", dst, FR_EXIST);
    if (!dexists) {
        FRESULT fr = f_mkdir(dst);
        if (fr != FR_OK) return report_error(c, "Create dir", dst, fr);
    }

    DIR *d = malloc(sizeof(DIR));
    FILINFO *fi = malloc(sizeof(FILINFO));
    char *s = malloc(MAX_PATH), *t = malloc(MAX_PATH);
    int ret = FO_OK;
    if (!d || !fi || !s || !t) { ret = report_error(c, "Copy", src, FR_NOT_ENOUGH_CORE); goto out; }
    FRESULT fr = f_opendir(d, src);
    if (fr != FR_OK) { ret = report_error(c, "Open dir", src, fr); goto out; }
    for (;;) {
        fr = f_readdir(d, fi);
        if (fr != FR_OK) {
            if (report_error(c, "Read dir", src, fr) == FO_CANCEL) ret = FO_CANCEL;
            else ret = FO_ERROR;
            break;
        }
        if (!fi->fname[0]) break;
        path_join(s, MAX_PATH, src, fi->fname);
        path_join(t, MAX_PATH, dst, fi->fname);
        int r = fo_copy(c, s, t);
        if (r == FO_CANCEL) { ret = FO_CANCEL; break; }
        if (r == FO_ERROR) ret = FO_ERROR;
    }
    f_closedir(d);
out:
    free(d);
    free(fi);
    free(s);
    free(t);
    return ret;
}

/* Snapshot the names inside a directory so it can be modified while processing. */
static int list_dir(const char *path, char ***out, int *n) {
    DIR *d = malloc(sizeof(DIR));
    FILINFO *fi = malloc(sizeof(FILINFO));
    char **names = NULL;
    int cnt = 0, cap = 0;
    FRESULT fr = FR_NOT_ENOUGH_CORE;
    if (d && fi && (fr = f_opendir(d, path)) == FR_OK) {
        while ((fr = f_readdir(d, fi)) == FR_OK && fi->fname[0]) {
            if (cnt == cap) {
                cap = cap ? cap * 2 : 16;
                char **nn = realloc(names, cap * sizeof(char *));
                if (!nn) { fr = FR_NOT_ENOUGH_CORE; break; }
                names = nn;
            }
            if (!(names[cnt] = strdup(fi->fname))) { fr = FR_NOT_ENOUGH_CORE; break; }
            cnt++;
        }
        f_closedir(d);
    }
    free(d);
    free(fi);
    *out = names;
    *n = cnt;
    return fr;
}

static void free_list(char **names, int n) {
    for (int i = 0; i < n; i++) free(names[i]);
    free(names);
}

int fo_delete(fo_ctx_t *c, const char *path) {
    FILINFO fi;
    if (c->cancelled) return FO_CANCEL;
    if (!fo_stat(path, &fi)) return report_error(c, "Delete", path, FR_NO_FILE);
    if (fi.fattrib & AM_RDO) f_chmod(path, 0, AM_RDO);

    if (fi.fattrib & AM_DIR) {
        char **names;
        int n, ret = FO_OK;
        int fr = list_dir(path, &names, &n);
        char *child = malloc(MAX_PATH);
        if (fr != FR_OK || !child) {
            ret = report_error(c, "Read dir", path, child ? fr : FR_NOT_ENOUGH_CORE);
        } else {
            for (int i = 0; i < n; i++) {
                path_join(child, MAX_PATH, path, names[i]);
                int r = fo_delete(c, child);
                if (r == FO_CANCEL) { ret = FO_CANCEL; break; }
                if (r != FO_OK) ret = FO_ERROR;
            }
        }
        free(child);
        free_list(names, n);
        if (ret != FO_OK) return ret;
    }

    show_progress(c, "Delete", path_base(path));
    FRESULT fr = f_unlink(path);
    if (fr != FR_OK) return report_error(c, "Delete", path, fr);
    c->files_done++;
    if (check_cancel(c)) return FO_CANCEL;
    return FO_OK;
}

int fo_move(fo_ctx_t *c, const char *src, const char *dst) {
    FILINFO si, di;
    if (c->cancelled) return FO_CANCEL;
    if (!fo_stat(src, &si)) return report_error(c, "Move", src, FR_NO_FILE);
    if (strcmp(src, dst) == 0) return FO_SKIPPED;
    bool same_obj = strcasecmp(src, dst) == 0; /* case-only rename */
    if ((si.fattrib & AM_DIR) && !same_obj && path_is_within(dst, src))
        return report_error(c, "Move into itself", dst, FR_DENIED);

    show_progress(c, "Move", path_base(src));
    bool dexists = !same_obj && fo_stat(dst, &di);
    if (dexists) {
        if ((si.fattrib & AM_DIR) && (di.fattrib & AM_DIR)) {
            /* Merge directory contents into the existing target. */
            char **names;
            int n, ret = FO_OK;
            int fr = list_dir(src, &names, &n);
            char *s = malloc(MAX_PATH), *t = malloc(MAX_PATH);
            if (fr != FR_OK || !s || !t) {
                ret = report_error(c, "Read dir", src, (s && t) ? fr : FR_NOT_ENOUGH_CORE);
            } else {
                for (int i = 0; i < n; i++) {
                    path_join(s, MAX_PATH, src, names[i]);
                    path_join(t, MAX_PATH, dst, names[i]);
                    int r = fo_move(c, s, t);
                    if (r == FO_CANCEL) { ret = FO_CANCEL; break; }
                    if (r != FO_OK) ret = FO_ERROR;
                }
                if (ret == FO_OK) f_unlink(src);
            }
            free(s);
            free(t);
            free_list(names, n);
            return ret;
        }
        if ((si.fattrib & AM_DIR) || (di.fattrib & AM_DIR))
            return report_error(c, "Move (type mismatch)", dst, FR_EXIST);
        int a = ask_conflict(c, dst);
        if (a < 0) return FO_CANCEL;
        if (a == 0) return FO_SKIPPED;
        char bak[MAX_PATH];
        if (snprintf(bak, sizeof(bak), "%s.~bak", dst) >= (int)sizeof(bak)) return report_error(c, "Replace", dst, FR_INVALID_NAME);
        f_unlink(bak);
        FRESULT fr = f_rename(dst, bak);
        if (fr != FR_OK) return report_error(c, "Replace", dst, fr);
        fr = f_rename(src, dst);
        if (fr != FR_OK) {
            f_rename(bak, dst);
            return report_error(c, "Move", src, fr);
        }
        if (di.fattrib & AM_RDO) f_chmod(bak, 0, AM_RDO);
        f_unlink(bak);
        c->files_done++;
        if (check_cancel(c)) return FO_CANCEL;
        return FO_OK;
    }

    FRESULT fr = f_rename(src, dst);
    if (fr != FR_OK) return report_error(c, "Move", src, fr);
    c->files_done++;
    if (check_cancel(c)) return FO_CANCEL;
    return FO_OK;
}
