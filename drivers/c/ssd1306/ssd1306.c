// SSD1306 OLED driver (C). Mirror of `ssd1306.lua` -- identical draw logic;
// only the transport (i2c_bus_t) and the show() chunking differ. See the note
// on show() for why this side does NOT chunk.
#include "ssd1306.h"
#include "font5x7.h"
#include <string.h>

#define CTRL_CMD   0x00u
#define CTRL_DATA  0x40u

// command stream: [0x00][cmd bytes...]. The init list is short, so one write.
static bool cmd(ssd1306_t *d, const uint8_t *c, size_t n) {
    uint8_t b[40];
    if (n + 1 > sizeof b) return false;
    b[0] = CTRL_CMD;
    memcpy(b + 1, c, n);
    return i2c_bus_write(d->bus, d->addr, b, n + 1) == 0;
}

bool ssd1306_init(ssd1306_t *d, const i2c_bus_t *bus, uint8_t addr, uint8_t w, uint8_t h) {
    d->bus  = bus;
    d->addr = addr ? addr : SSD1306_ADDR;
    d->w    = w ? w : 128u;
    d->h    = h ? h : 64u;
    d->pages = (uint8_t)(d->h / 8u);
    const uint8_t mux     = (uint8_t)(d->h - 1u);
    const uint8_t compins = (d->h == 32u) ? 0x02u : 0x12u;
    const uint8_t init[] = {
        0xAE,                 // display off
        0xD5, 0x80,           // clock divide
        0xA8, mux,            // multiplex = H-1
        0xD3, 0x00,           // display offset 0
        0x40,                 // start line 0
        0x8D, 0x14,           // charge pump on
        0x20, 0x00,           // horizontal addressing mode
        0xA1,                 // segment remap (column 127 -> SEG0)
        0xC8,                 // COM scan direction reversed -> upright
        0xDA, compins,        // COM pins config
        0x81, 0xCF,           // contrast
        0xD9, 0xF1,           // pre-charge
        0xDB, 0x40,           // VCOMH deselect
        0xA4,                 // resume to RAM content
        0xA6,                 // normal (not inverted)
        0xAF,                 // display on
    };
    if (!cmd(d, init, sizeof init)) return false;
    ssd1306_clear(d);
    return ssd1306_show(d);
}

void ssd1306_clear(ssd1306_t *d) {
    memset(SSD1306_FB(d), 0, (size_t)d->w * d->pages);
}

void ssd1306_pixel(ssd1306_t *d, int x, int y, bool on) {
    if (x < 0 || y < 0 || x >= d->w || y >= d->h) return;
    uint8_t *fb = SSD1306_FB(d);
    unsigned idx = (unsigned)(y >> 3) * d->w + (unsigned)x;
    uint8_t bit = (uint8_t)(1u << (y & 7));
    if (on) fb[idx] |= bit;
    else    fb[idx] = (uint8_t)(fb[idx] & ~bit);
}

void ssd1306_char(ssd1306_t *d, int x, int y, char c) {
    unsigned ch = (unsigned char)c;
    if (ch < 0x20u || ch > 0x7Eu) ch = 0x20u;
    const uint8_t *g = font5x7[ch - 0x20u];
    for (int col = 0; col < 5; col++)
        for (int row = 0; row < 7; row++)
            if (g[col] & (1u << row)) ssd1306_pixel(d, x + col, y + row, true);
}

void ssd1306_text(ssd1306_t *d, int x, int y, const char *s) {
    for (; *s; s++, x += 6) ssd1306_char(d, x, y, *s);
}

bool ssd1306_show(ssd1306_t *d) {
    const uint8_t addr_cmd[] = { 0x21, 0x00, (uint8_t)(d->w - 1u),       // column range
                                 0x22, 0x00, (uint8_t)(d->pages - 1u) }; // page range
    if (!cmd(d, addr_cmd, sizeof addr_cmd)) return false;
    // The framebuffer ships in ONE write: buf[0] is already the 0x40 data
    // control byte. A chip-native I2C master has no transfer cap (the 32/64 B
    // limit is a host USB-shell artifact, handled on the Lua side), so unlike
    // ssd1306.lua this does not chunk.
    d->buf[0] = CTRL_DATA;
    return i2c_bus_write(d->bus, d->addr, d->buf, 1u + (size_t)d->w * d->pages) == 0;
}

bool ssd1306_display_on(ssd1306_t *d, bool on) {
    uint8_t c = on ? 0xAFu : 0xAEu;
    return cmd(d, &c, 1);
}
bool ssd1306_invert(ssd1306_t *d, bool inv) {
    uint8_t c = inv ? 0xA7u : 0xA6u;
    return cmd(d, &c, 1);
}
bool ssd1306_contrast(ssd1306_t *d, uint8_t c) {
    uint8_t b[2] = { 0x81u, c };
    return cmd(d, b, sizeof b);
}
