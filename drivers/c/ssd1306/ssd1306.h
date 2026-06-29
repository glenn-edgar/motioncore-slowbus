#ifndef SLOW_BUS_SSD1306_H
#define SLOW_BUS_SSD1306_H
// SSD1306 monochrome OLED driver (C). Mirror of host-side `ssd1306.lua`.
//
// Mono OLED on I2C (XIAO expansion board: 128x64 @ 0x3C). Every transfer is a
// control byte then a stream: 0x00 = command, 0x40 = data. The display RAM is
// W cols x (H/8) pages, one byte = 8 vertical pixels (bit0 = top). The driver
// keeps a local framebuffer, draws into it, and show()s the whole thing.
#include "i2c_bus.h"

#define SSD1306_ADDR  0x3Cu

typedef struct {
    const i2c_bus_t *bus;
    uint8_t addr;
    uint8_t w, h, pages;
    // Data-stream buffer: buf[0] = the 0x40 control byte, buf[1..w*pages] = the
    // framebuffer. Kept contiguous so show() is a single i2c write with no copy
    // and no large stack buffer. Use the SSD1306_FB() accessor for pixel work.
    uint8_t buf[1 + 1024];   // sized for the 128x64 max
} ssd1306_t;

#define SSD1306_FB(d)  ((d)->buf + 1)

// config (defaults: addr 0x3C, 128x64 when args are 0) + clear + display on.
bool ssd1306_init(ssd1306_t *d, const i2c_bus_t *bus, uint8_t addr, uint8_t w, uint8_t h);
void ssd1306_clear(ssd1306_t *d);                          // framebuffer only
void ssd1306_pixel(ssd1306_t *d, int x, int y, bool on);
void ssd1306_char(ssd1306_t *d, int x, int y, char c);     // 6px advance, 0x20..0x7E
void ssd1306_text(ssd1306_t *d, int x, int y, const char *s);
bool ssd1306_show(ssd1306_t *d);                           // flush framebuffer -> RAM
bool ssd1306_display_on(ssd1306_t *d, bool on);
bool ssd1306_invert(ssd1306_t *d, bool inv);
bool ssd1306_contrast(ssd1306_t *d, uint8_t c);

#endif // SLOW_BUS_SSD1306_H
