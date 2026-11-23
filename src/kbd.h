#pragma once
#include <stdbool.h>

#define KEY_BACKSPACE 0x08
#define KEY_TAB       0x09
#define KEY_ENTER     0x0A
#define KEY_ESC       0xB1
#define KEY_LEFT      0xB4
#define KEY_UP        0xB5
#define KEY_DOWN      0xB6
#define KEY_RIGHT     0xB7
#define KEY_BREAK     0xD0
#define KEY_INSERT    0xD1
#define KEY_HOME      0xD2
#define KEY_DEL       0xD4
#define KEY_END       0xD5
#define KEY_PGUP      0xD6
#define KEY_PGDN      0xD7
#define KEY_F1        0x81
#define KEY_F2        0x82
#define KEY_F3        0x83
#define KEY_F4        0x84
#define KEY_F5        0x85
#define KEY_F6        0x86
#define KEY_F7        0x87
#define KEY_F8        0x88
#define KEY_F9        0x89
#define KEY_F10       0x90

#define KM_CTRL   0x100
#define KM_ALT    0x200
#define KEY_CODE(k) ((k) & 0xFF)

void kbd_init(void);
/* Returns next key press (code | modifier flags) or -1 if none pending. */
int kbd_get(void);
/* Blocks until a key is pressed. idle() (may be NULL) is called while waiting. */
int kbd_wait(void (*idle)(void));
/* Battery percent (0-100), or -1 on error. *charging set if non-NULL. */
int kbd_battery(bool *charging);
bool kbd_esc_pressed(void);
