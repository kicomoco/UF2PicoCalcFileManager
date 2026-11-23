#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "lcd.h"
#include "font8x8.h"

#define LCD_SPI   spi1
#define PIN_SCK   10
#define PIN_MOSI  11
#define PIN_MISO  12
#define PIN_CS    13
#define PIN_DC    14
#define PIN_RST   15
#define LCD_BAUD  25000000

static const uint8_t palette[16][3] = {
    {0x00, 0x00, 0x00}, {0x00, 0x00, 0xA8}, {0x00, 0xA8, 0x00}, {0x00, 0xA8, 0xA8},
    {0xA8, 0x00, 0x00}, {0xA8, 0x00, 0xA8}, {0xA8, 0x54, 0x00}, {0xA8, 0xA8, 0xA8},
    {0x54, 0x54, 0x54}, {0x54, 0x54, 0xFC}, {0x54, 0xFC, 0x54}, {0x54, 0xFC, 0xFC},
    {0xFC, 0x54, 0x54}, {0xFC, 0x54, 0xFC}, {0xFC, 0xFC, 0x54}, {0xFC, 0xFC, 0xFC},
};

static uint8_t font[128][CELL_H];

/* Cell = ch | fg << 8 | bg << 16 */
static uint32_t cells[SCR_ROWS][SCR_COLS];
static uint32_t shown[SCR_ROWS][SCR_COLS];
static int cur_x = -1, cur_y = -1;
static bool cur_vis;
static int shown_cur_x = -1, shown_cur_y = -1;

static inline uint32_t mkcell(uint8_t c, uint8_t fg, uint8_t bg) {
    return (uint32_t)c | ((uint32_t)(fg & 15) << 8) | ((uint32_t)(bg & 15) << 16);
}

static void build_font(void) {
    memset(font, 0, sizeof(font));
    for (int c = 32; c < 128; c++)
        for (int r = 0; r < 8; r++) font[c][r + 1] = font8x8_basic[c][r];

    const uint8_t V = 0x08, L = 0x0F, R = 0xF8;
    for (int r = 0; r < CELL_H; r++) {
        font[G_VLINE][r] = V;
        font[G_TEE_L][r] = V;
        font[G_TEE_R][r] = V;
        font[G_SHADE][r] = (r & 1) ? 0xAA : 0x55;
        font[G_BLOCK][r] = 0xFF;
        if (r < 4) { font[G_BL][r] = V; font[G_BR][r] = V; }
        if (r > 4) { font[G_TL][r] = V; font[G_TR][r] = V; }
    }
    font[G_HLINE][4] = 0xFF;
    font[G_TL][4] = R; font[G_TR][4] = L;
    font[G_BL][4] = R; font[G_BR][4] = L;
    font[G_TEE_L][4] = R; font[G_TEE_R][4] = L;

    font[G_TAB][3] = 0x20; font[G_TAB][4] = 0x7E; font[G_TAB][5] = 0x20;
    static const uint8_t up[] = {0, 0, 0x18, 0x3C, 0x7E, 0x18, 0x18, 0x18, 0, 0};
    for (int r = 0; r < CELL_H; r++) {
        font[G_UP][r] = up[r];
        font[G_DOWN][CELL_H - 1 - r] = up[r];
    }
    font[G_DOTS][7] = 0x49;
}

/* Sends a command byte and its parameters within one chip-select assertion. */
static void lcd_cmdd(uint8_t c, const uint8_t *d, size_t n) {
    gpio_put(PIN_CS, 0);
    gpio_put(PIN_DC, 0);
    spi_write_blocking(LCD_SPI, &c, 1);
    gpio_put(PIN_DC, 1);
    if (n) spi_write_blocking(LCD_SPI, d, n);
    gpio_put(PIN_CS, 1);
}

static void lcd_cmd(uint8_t c) {
    lcd_cmdd(c, NULL, 0);
}

#define CMDD(c, ...) do { static const uint8_t _d[] = {__VA_ARGS__}; lcd_cmdd(c, _d, sizeof(_d)); } while (0)

/* Sets the address window and starts a memory write. Chip select is left asserted
 * (with DC high) so pixel data can follow directly; the caller must raise CS when done.
 * The panel controller drops a memory write if CS is released after RAMWR. */
static void lcd_begin_write(int x0, int y0, int x1, int y1) {
    const uint8_t caset = 0x2A, paset = 0x2B, ramwr = 0x2C;
    uint8_t col[4] = {x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF};
    uint8_t row[4] = {y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF};
    gpio_put(PIN_CS, 0);
    gpio_put(PIN_DC, 0);
    spi_write_blocking(LCD_SPI, &caset, 1);
    gpio_put(PIN_DC, 1);
    spi_write_blocking(LCD_SPI, col, 4);
    gpio_put(PIN_DC, 0);
    spi_write_blocking(LCD_SPI, &paset, 1);
    gpio_put(PIN_DC, 1);
    spi_write_blocking(LCD_SPI, row, 4);
    gpio_put(PIN_DC, 0);
    spi_write_blocking(LCD_SPI, &ramwr, 1);
    gpio_put(PIN_DC, 1);
}

void lcd_init(void) {
    build_font();

    spi_init(LCD_SPI, LCD_BAUD);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    gpio_init(PIN_CS);
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_put(PIN_CS, 1);
    gpio_init(PIN_DC);
    gpio_set_dir(PIN_DC, GPIO_OUT);
    gpio_init(PIN_RST);
    gpio_set_dir(PIN_RST, GPIO_OUT);

    gpio_put(PIN_RST, 1);
    sleep_ms(10);
    gpio_put(PIN_RST, 0);
    sleep_ms(10);
    gpio_put(PIN_RST, 1);
    sleep_ms(200);

    /* Init sequence from the official ClockworkPi PicoCalc examples. */
    CMDD(0xE0, 0x00, 0x03, 0x09, 0x08, 0x16, 0x0A, 0x3F, 0x78, 0x4C, 0x09, 0x0A, 0x08, 0x16, 0x1A, 0x0F);
    CMDD(0xE1, 0x00, 0x16, 0x19, 0x03, 0x0F, 0x05, 0x32, 0x45, 0x46, 0x04, 0x0E, 0x0D, 0x35, 0x37, 0x0F);
    CMDD(0xC0, 0x17, 0x15);
    CMDD(0xC1, 0x41);
    CMDD(0xC5, 0x00, 0x12, 0x80);
    CMDD(0x36, 0x48);
    CMDD(0x3A, 0x66); /* 18-bit colour over SPI */
    CMDD(0xB0, 0x00);
    CMDD(0xB1, 0xA0);
    lcd_cmd(0x21);    /* inversion on */
    CMDD(0xB4, 0x02);
    CMDD(0xB6, 0x02, 0x02, 0x3B);
    CMDD(0xB7, 0xC6);
    CMDD(0xE9, 0x00);
    CMDD(0xF7, 0xA9, 0x51, 0x2C, 0x82);
    lcd_cmd(0x11);
    sleep_ms(120);

    /* Clear GRAM before enabling the display so no stale image is shown. */
    for (int y = 0; y < SCR_ROWS; y++)
        for (int x = 0; x < SCR_COLS; x++) shown[y][x] = 0xFFFFFFFF;
    scr_clear(C_WHITE, C_BLACK);
    scr_flush();

    lcd_cmd(0x29);
    sleep_ms(120);
    CMDD(0x36, 0x48);
}

static void draw_run(int y, int x0, int x1) {
    static uint8_t line[SCR_COLS * CELL_W * 3];
    lcd_begin_write(x0 * CELL_W, y * CELL_H, (x1 + 1) * CELL_W - 1, (y + 1) * CELL_H - 1);
    for (int r = 0; r < CELL_H; r++) {
        uint8_t *p = line;
        for (int x = x0; x <= x1; x++) {
            uint32_t cell = cells[y][x];
            uint8_t ch = cell & 0x7F;
            uint8_t fg = (cell >> 8) & 15, bg = (cell >> 16) & 15;
            if (cur_vis && x == cur_x && y == cur_y) { uint8_t t = fg; fg = bg; bg = t; }
            uint8_t bits = font[ch][r];
            const uint8_t *f = palette[fg], *b = palette[bg];
            for (int i = 0; i < CELL_W; i++) {
                const uint8_t *c = (bits & (1 << i)) ? f : b;
                *p++ = c[0];
                *p++ = c[1];
                *p++ = c[2];
            }
        }
        spi_write_blocking(LCD_SPI, line, p - line);
    }
    gpio_put(PIN_CS, 1);
}

void scr_flush(void) {
    bool cur_now = cur_vis;
    if (shown_cur_x >= 0 && shown_cur_y >= 0 && (shown_cur_x != cur_x || shown_cur_y != cur_y || !cur_now))
        shown[shown_cur_y][shown_cur_x] = 0xFFFFFFFF;
    if (cur_now && cur_x >= 0 && cur_y >= 0 && cur_x < SCR_COLS && cur_y < SCR_ROWS &&
        (shown_cur_x != cur_x || shown_cur_y != cur_y))
        shown[cur_y][cur_x] = 0xFFFFFFFF;

    for (int y = 0; y < SCR_ROWS; y++) {
        int x = 0;
        while (x < SCR_COLS) {
            if (cells[y][x] == shown[y][x]) { x++; continue; }
            int s = x;
            while (x < SCR_COLS && cells[y][x] != shown[y][x]) {
                shown[y][x] = cells[y][x];
                x++;
            }
            draw_run(y, s, x - 1);
        }
    }
    if (cur_now) { shown_cur_x = cur_x; shown_cur_y = cur_y; }
    else { shown_cur_x = shown_cur_y = -1; }
}

void scr_invalidate(void) {
    for (int y = 0; y < SCR_ROWS; y++)
        for (int x = 0; x < SCR_COLS; x++) shown[y][x] = 0xFFFFFFFF;
}

void scr_clear(uint8_t fg, uint8_t bg) {
    scr_fill(0, 0, SCR_COLS, SCR_ROWS, ' ', fg, bg);
}

void scr_putc(int x, int y, uint8_t c, uint8_t fg, uint8_t bg) {
    if (x < 0 || y < 0 || x >= SCR_COLS || y >= SCR_ROWS) return;
    if (c >= 128) c = '?';
    cells[y][x] = mkcell(c, fg, bg);
}

int scr_puts(int x, int y, const char *s, uint8_t fg, uint8_t bg, int maxw) {
    int n = 0;
    if (maxw < 0) maxw = SCR_COLS - x;
    while (*s && n < maxw) {
        scr_putc(x + n, y, (uint8_t)*s++, fg, bg);
        n++;
    }
    return n;
}

void scr_putsw(int x, int y, const char *s, uint8_t fg, uint8_t bg, int w) {
    int n = scr_puts(x, y, s, fg, bg, w);
    for (; n < w; n++) scr_putc(x + n, y, ' ', fg, bg);
}

void scr_printf(int x, int y, uint8_t fg, uint8_t bg, const char *fmt, ...) {
    char buf[SCR_COLS * 2 + 1];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    scr_puts(x, y, buf, fg, bg, -1);
}

void scr_fill(int x, int y, int w, int h, uint8_t c, uint8_t fg, uint8_t bg) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) scr_putc(i, j, c, fg, bg);
}

void scr_box(int x, int y, int w, int h, uint8_t fg, uint8_t bg) {
    scr_fill(x, y, w, h, ' ', fg, bg);
    for (int i = x + 1; i < x + w - 1; i++) {
        scr_putc(i, y, G_HLINE, fg, bg);
        scr_putc(i, y + h - 1, G_HLINE, fg, bg);
    }
    for (int j = y + 1; j < y + h - 1; j++) {
        scr_putc(x, j, G_VLINE, fg, bg);
        scr_putc(x + w - 1, j, G_VLINE, fg, bg);
    }
    scr_putc(x, y, G_TL, fg, bg);
    scr_putc(x + w - 1, y, G_TR, fg, bg);
    scr_putc(x, y + h - 1, G_BL, fg, bg);
    scr_putc(x + w - 1, y + h - 1, G_BR, fg, bg);
}

void scr_set_cursor(int x, int y, bool visible) {
    cur_x = x;
    cur_y = y;
    cur_vis = visible;
}

void *scr_save(void) {
    struct saved { uint32_t c[SCR_ROWS][SCR_COLS]; int x, y; bool v; } *s = malloc(sizeof(*s));
    if (!s) return NULL;
    memcpy(s->c, cells, sizeof(cells));
    s->x = cur_x; s->y = cur_y; s->v = cur_vis;
    return s;
}

void scr_restore(void *p) {
    struct saved { uint32_t c[SCR_ROWS][SCR_COLS]; int x, y; bool v; } *s = p;
    if (!s) return;
    memcpy(cells, s->c, sizeof(cells));
    cur_x = s->x; cur_y = s->y; cur_vis = s->v;
    free(s);
}
