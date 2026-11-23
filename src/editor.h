#pragma once
#include <stdbool.h>

enum { EDIT_OK = 0, EDIT_TOO_LARGE = 1, EDIT_ERROR = -1 };

#define EDIT_MAX_FILE (192 * 1024)

/* Opens a text file for editing (or viewing when readonly). The file need not exist. */
int editor_open(const char *path, bool readonly);
