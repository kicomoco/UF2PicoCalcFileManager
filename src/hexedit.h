#pragma once
#include <stdbool.h>

/* Hex editor/viewer working directly on the file (any size). Overwrite mode;
 * bytes may be appended at end of file. */
void hexedit_open(const char *path, bool readonly);
