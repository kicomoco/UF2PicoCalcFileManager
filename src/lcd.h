#pragma once
#include <stdint.h>
#include <stdbool.h>

#define LCD_W 320
#define LCD_H 320
#define CELL_W 8
#define CELL_H 10
#define SCR_COLS (LCD_W / CELL_W)
#define SCR_ROWS (LCD_H / CELL_H)

/* CGA-style 16 colour palette (Midnight Commander look). */
enum {
    C_BLACK, C_BLUE, C_GREEN, C_CYAN, C_RED, C_MAGENTA, C_BROWN, C_LGRAY,
    C_DGRAY, C_LBLUE, C_LGREEN, C_LCYAN, C_LRED, C_LMAGENTA, C_YELLOW, C_WHITE
};

/* Custom glyphs in the control-character range. */
#define G_HLINE   0x01
#define G_VLINE   0x02
#define G_TL      0x03
#define G_TR      0x04
#define G_BL      0x05
#define G_BR      0x06
#define G_TEE_L   0x07
#define G_TEE_R   0x08
#define G_TAB     0x0B
#define G_UP      0x0E
#define G_DOWN    0x0F
#define G_SHADE   0x10
#define G_BLOCK   0x11
#define G_DOTS    0x12

void lcd_init(void);

void scr_clear(uint8_t fg, uint8_t bg);
void scr_putc(int x, int y, uint8_t c, uint8_t fg, uint8_t bg);
/* Writes at most maxw cells (maxw < 0 means until end of row). Returns cells written. */
int scr_puts(int x, int y, const char *s, uint8_t fg, uint8_t bg, int maxw);
/* Like scr_puts but pads with spaces to exactly w cells. */
void scr_putsw(int x, int y, const char *s, uint8_t fg, uint8_t bg, int w);
void scr_printf(int x, int y, uint8_t fg, uint8_t bg, const char *fmt, ...);
void scr_fill(int x, int y, int w, int h, uint8_t c, uint8_t fg, uint8_t bg);
void scr_box(int x, int y, int w, int h, uint8_t fg, uint8_t bg);
void scr_set_cursor(int x, int y, bool visible);
void scr_flush(void);
void scr_invalidate(void);
/* Snapshot / restore the whole text screen (restore frees the snapshot). */
void *scr_save(void);
void scr_restore(void *snap);
