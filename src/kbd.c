#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "kbd.h"

#define KBD_I2C   i2c1
#define KBD_SDA   6
#define KBD_SCL   7
#define KBD_ADDR  0x1F
#define KBD_BAUD  10000

#define REG_FIFO  0x09
#define REG_BAT   0x0B

#define MOD_ALT   0xA1
#define MOD_SHL   0xA2
#define MOD_SHR   0xA3
#define MOD_SYM   0xA4
#define MOD_CTRL  0xA5

#define ST_PRESSED  1
#define ST_RELEASED 3

static bool ctrl_down, alt_down;
static absolute_time_t next_poll;

void kbd_init(void) {
    i2c_init(KBD_I2C, KBD_BAUD);
    gpio_set_function(KBD_SCL, GPIO_FUNC_I2C);
    gpio_set_function(KBD_SDA, GPIO_FUNC_I2C);
    gpio_pull_up(KBD_SCL);
    gpio_pull_up(KBD_SDA);
    next_poll = get_absolute_time();
}

static int read_reg16(uint8_t reg, uint16_t *out) {
    uint8_t buf[2];
    if (i2c_write_timeout_us(KBD_I2C, KBD_ADDR, &reg, 1, false, 100000) != 1) return -1;
    /* The keyboard MCU needs time to prepare the response (matches the official drivers). */
    sleep_ms(16);
    if (i2c_read_timeout_us(KBD_I2C, KBD_ADDR, buf, 2, false, 100000) != 2) return -1;
    *out = buf[0] | (buf[1] << 8);
    return 0;
}

int kbd_get(void) {
    if (absolute_time_diff_us(get_absolute_time(), next_poll) > 0) return -1;
    for (int i = 0; i < 8; i++) {
        uint16_t v;
        if (read_reg16(REG_FIFO, &v) < 0 || v == 0) {
            next_poll = make_timeout_time_ms(4);
            return -1;
        }
        uint8_t state = v & 0xFF, key = v >> 8;
        if (key == MOD_CTRL) { ctrl_down = (state != ST_RELEASED); continue; }
        if (key == MOD_ALT) { alt_down = (state != ST_RELEASED); continue; }
        if (key == MOD_SHL || key == MOD_SHR || key == MOD_SYM) continue;
        /* Firmware already turns auto-repeat of text/navigation keys into PRESSED events. */
        if (state != ST_PRESSED) continue;
        int k = key;
        if (k == 0x0D) k = KEY_ENTER;
        if (ctrl_down) k |= KM_CTRL;
        if (alt_down) k |= KM_ALT;
        return k;
    }
    return -1;
}

int kbd_wait(void (*idle)(void)) {
    for (;;) {
        int k = kbd_get();
        if (k >= 0) return k;
        if (idle) idle();
        sleep_ms(2);
    }
}

bool kbd_esc_pressed(void) {
    int k;
    while ((k = kbd_get()) >= 0)
        if (KEY_CODE(k) == KEY_ESC) return true;
    return false;
}

int kbd_battery(bool *charging) {
    uint16_t v;
    if (read_reg16(REG_BAT, &v) < 0) return -1;
    uint8_t b = v >> 8;
    if (charging) *charging = (b & 0x80) != 0;
    int pct = b & 0x7F;
    return pct > 100 ? 100 : pct;
}
