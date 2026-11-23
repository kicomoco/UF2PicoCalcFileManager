#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "ff.h"

enum { FO_OK = 0, FO_SKIPPED = 1, FO_ERROR = -1, FO_CANCEL = -2 };
enum { CONFLICT_ASK, CONFLICT_OVERWRITE_ALL, CONFLICT_SKIP_ALL };

typedef struct {
    int conflict;
    int files_done;
    int errors;
    uint64_t bytes_done;
    uint64_t bytes_total;
    bool cancelled;
} fo_ctx_t;

void fo_ctx_init(fo_ctx_t *c);
bool fo_stat(const char *path, FILINFO *fi);
/* dst is the full destination path (not the containing directory). */
int fo_copy(fo_ctx_t *c, const char *src, const char *dst);
int fo_move(fo_ctx_t *c, const char *src, const char *dst);
int fo_delete(fo_ctx_t *c, const char *path);
/* Recursively totals file bytes beneath path (for progress). */
uint64_t fo_tree_size(const char *path);
