#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_PATH 512

void path_join(char *out, size_t n, const char *dir, const char *name);
void path_parent(char *path);
const char *path_base(const char *path);
/* True if path equals dir or lies beneath it. */
bool path_is_within(const char *path, const char *dir);
void fmt_size(char *out, size_t n, uint64_t sz);
/* Converts UTF-8 to single-byte display text ('?' for non-ASCII code points). */
void utf8_to_disp(char *out, size_t n, const char *in);
/* Same, but keeps newlines. */
void utf8_to_disp_ml(char *out, size_t n, const char *in);
/* Reboots into the UF2Loader menu. Does not return. */
void reboot_to_loader(void) __attribute__((noreturn));
int disp_len(const char *s);
/* Compares dotted versions ("v1.2.3" / "1.2.3"). Returns <0, 0, >0. */
int version_cmp(const char *a, const char *b);
const char *fr_str(int fr);
