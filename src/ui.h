#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Draws the Midnight Commander style function key bar on the last row.
 * labels[i] is the label for F(i+1); NULL leaves the slot blank. */
void ui_keybar(const char *const labels[10]);

void ui_message(const char *title, const char *msg);
void ui_error(const char *title, const char *msg);
bool ui_confirm(const char *title, const char *msg);
/* Returns chosen option index, or -1 on Esc. Hotkey = first letter of option. */
int ui_choice(const char *title, const char *msg, const char *const *opts, int n, int def, bool danger);
/* Single line text input. buf holds the initial value and receives the result. */
bool ui_input(const char *title, const char *prompt, char *buf, size_t bufsize, bool masked);
/* Items may contain '\t' to separate a right aligned shortcut hint. Returns index or -1. */
int ui_menu(const char *title, const char *const *items, int n, int sel);
void ui_textpage(const char *title, const char *text);

/* Progress dialog with a / - \ | spinner. */
void ui_progress_begin(const char *title);
void ui_progress_set(const char *line1, const char *line2);
void ui_progress_tick(void);
bool ui_progress_cancelled(void);
void ui_progress_end(void);
