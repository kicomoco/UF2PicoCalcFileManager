#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "util.h"
#include "ff.h"

void path_join(char *out, size_t n, const char *dir, const char *name) {
    size_t dl = strlen(dir);
    if (dl > 0 && dir[dl - 1] == '/')
        snprintf(out, n, "%s%s", dir, name);
    else
        snprintf(out, n, "%s/%s", dir, name);
}

void path_parent(char *path) {
    char *s = strrchr(path, '/');
    if (!s) { strcpy(path, "/"); return; }
    if (s == path) path[1] = 0;
    else *s = 0;
}

const char *path_base(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

bool path_is_within(const char *path, const char *dir) {
    size_t dl = strlen(dir);
    if (strcmp(dir, "/") == 0) return true;
    if (strncasecmp(path, dir, dl) != 0) return false;
    return path[dl] == 0 || path[dl] == '/';
}

void fmt_size(char *out, size_t n, uint64_t sz) {
    static const char units[] = "BKMGT";
    if (sz < 10000) { snprintf(out, n, "%u", (unsigned)sz); return; }
    double v = (double)sz;
    int u = 0;
    while (v >= 1000.0 && u < 4) { v /= 1024.0; u++; }
    if (v < 10.0) snprintf(out, n, "%.1f%c", v, units[u]);
    else snprintf(out, n, "%u%c", (unsigned)(v + 0.5), units[u]);
}

void utf8_to_disp(char *out, size_t n, const char *in) {
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)in;
    while (*p && o + 1 < n) {
        if (*p < 0x80) {
            out[o++] = (*p >= 32) ? (char)*p : '?';
            p++;
        } else {
            out[o++] = '?';
            p++;
            while ((*p & 0xC0) == 0x80) p++;
        }
    }
    out[o] = 0;
}

int disp_len(const char *s) {
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if ((*p & 0xC0) != 0x80) n++;
    return n;
}

int version_cmp(const char *a, const char *b) {
    while (*a && !isdigit((unsigned char)*a)) a++;
    while (*b && !isdigit((unsigned char)*b)) b++;
    for (int i = 0; i < 4; i++) {
        long x = strtol(a, (char **)&a, 10);
        long y = strtol(b, (char **)&b, 10);
        if (x != y) return x < y ? -1 : 1;
        if (*a == '.') a++;
        if (*b == '.') b++;
        if (!isdigit((unsigned char)*a) && !isdigit((unsigned char)*b)) break;
    }
    return 0;
}

const char *fr_str(int fr) {
    switch (fr) {
    case FR_OK: return "OK";
    case FR_DISK_ERR: return "Disk I/O error";
    case FR_INT_ERR: return "Internal error";
    case FR_NOT_READY: return "SD card not ready";
    case FR_NO_FILE: return "File not found";
    case FR_NO_PATH: return "Path not found";
    case FR_INVALID_NAME: return "Invalid name";
    case FR_DENIED: return "Access denied / full";
    case FR_EXIST: return "Already exists";
    case FR_INVALID_OBJECT: return "Invalid object";
    case FR_WRITE_PROTECTED: return "Write protected";
    case FR_NOT_ENABLED: return "Volume not mounted";
    case FR_NO_FILESYSTEM: return "No FAT filesystem";
    case FR_TIMEOUT: return "Timeout";
    case FR_LOCKED: return "File locked";
    case FR_NOT_ENOUGH_CORE: return "Out of memory";
    case FR_TOO_MANY_OPEN_FILES: return "Too many open files";
    default: return "Unknown error";
    }
}
