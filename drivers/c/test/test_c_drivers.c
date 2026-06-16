// test_c_drivers.c -- host equivalence harness for the C device drivers.
//
// Binds i2c_bus_t to a MOCK bus that records every transaction (and serves
// canned read responses), then drives pcf8563.c + ssd1306.c and asserts the
// emitted I2C byte streams. The expected bytes are exactly the wire the
// HW-verified Lua twins send (and the real chips accepted), so a pass proves
// the C logic -- init sequence, BCD, framebuffer layout, addressing -- is
// byte-for-byte identical to the known-good Lua side. No hardware needed.
//
//   (from drivers/c/)
//   cc -std=c11 -Wall -Wextra -Werror -Ii2c -Ipcf8563 -Issd1306 test/test_c_drivers.c pcf8563/pcf8563.c ssd1306/ssd1306.c -o /tmp/tcd && /tmp/tcd
#include "i2c_bus.h"
#include "pcf8563.h"
#include "ssd1306.h"
#include "font5x7.h"
#include <stdio.h>
#include <string.h>

// ---- mock bus ---------------------------------------------------------------
#define MAX_TX  8
#define TX_CAP  1100
typedef struct { char op; uint8_t addr; size_t len; uint8_t data[TX_CAP]; } tx_t;
static tx_t  g_tx[MAX_TX];
static int   g_ntx;
static uint8_t g_resp[64];          // canned bytes returned by the next read()
static size_t  g_resp_len, g_resp_pos;

static void mock_reset(void) { g_ntx = 0; g_resp_len = g_resp_pos = 0; }
static void mock_response(const uint8_t *d, size_t n) { memcpy(g_resp, d, n); g_resp_len = n; g_resp_pos = 0; }

static int mock_write(void *ctx, uint8_t addr, const uint8_t *buf, size_t len, bool nostop) {
    (void)ctx; (void)nostop;
    if (g_ntx >= MAX_TX || len > TX_CAP) return -1;
    tx_t *t = &g_tx[g_ntx++];
    t->op = 'W'; t->addr = addr; t->len = len; memcpy(t->data, buf, len);
    return 0;
}
static int mock_read(void *ctx, uint8_t addr, uint8_t *buf, size_t len) {
    (void)ctx;
    if (g_ntx < MAX_TX) { tx_t *t = &g_tx[g_ntx++]; t->op = 'R'; t->addr = addr; t->len = len; }
    for (size_t i = 0; i < len; i++) buf[i] = (g_resp_pos < g_resp_len) ? g_resp[g_resp_pos++] : 0;
    return 0;
}
static i2c_bus_t g_bus = { .ctx = NULL, .write = mock_write, .read = mock_read };

// ---- check framework --------------------------------------------------------
static int g_pass, g_fail;
static void ok(const char *name, int cond) {
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", name);
    if (cond) g_pass++; else g_fail++;
}
static int bytes_eq(const uint8_t *a, const uint8_t *b, size_t n) { return memcmp(a, b, n) == 0; }
static void dump(const char *tag, const uint8_t *d, size_t n) {
    printf("    %s:", tag); for (size_t i = 0; i < n; i++) printf(" %02X", d[i]); printf("\n");
}

int main(void) {
    printf("C driver equivalence harness (mock i2c bus)\n\n");

    // ===== PCF8563 ===========================================================
    printf("PCF8563:\n");
    // init -> single write [CTRL1=0x00, 0x00, 0x00]
    mock_reset();
    pcf8563_init(&g_bus, PCF8563_ADDR);
    { const uint8_t exp[] = { 0x00, 0x00, 0x00 };
      ok("init writes CTRL1/2 = 0", g_ntx == 1 && g_tx[0].op == 'W' &&
         g_tx[0].addr == 0x51 && g_tx[0].len == 3 && bytes_eq(g_tx[0].data, exp, 3)); }

    // set(2026-06-16 13:45:30, wday 2) -> [0x02, BCD...]
    mock_reset();
    pcf8563_time_t set = { .sec=30,.min=45,.hour=13,.mday=16,.wday=2,.month=6,.year=2026 };
    pcf8563_set(&g_bus, PCF8563_ADDR, &set);
    { const uint8_t exp[] = { 0x02, 0x30, 0x45, 0x13, 0x16, 0x02, 0x06, 0x26 }; // reg + BCD
      int good = g_ntx == 1 && g_tx[0].len == 8 && bytes_eq(g_tx[0].data, exp, 8);
      ok("set emits reg-ptr + 7 BCD bytes", good);
      if (!good) { dump("got", g_tx[0].data, g_tx[0].len); dump("exp", exp, 8); } }

    // get: write_read = write [0x02] (nostop) then read 7. Feed a known response.
    mock_reset();
    { const uint8_t resp[] = { 0xB0, 0x45, 0x13, 0x16, 0x02, 0x06, 0x26 }; // sec has VL bit7 set
      mock_response(resp, sizeof resp);
      pcf8563_time_t t; bool gok = pcf8563_get(&g_bus, PCF8563_ADDR, &t);
      ok("get does write[0x02] then read 7",
         g_ntx == 2 && g_tx[0].op == 'W' && g_tx[0].len == 1 && g_tx[0].data[0] == 0x02 &&
         g_tx[1].op == 'R' && g_tx[1].len == 7);
      ok("get decodes BCD (VL masked off seconds)",
         gok && t.sec==30 && t.min==45 && t.hour==13 && t.mday==16 &&
         t.wday==2 && t.month==6 && t.year==2026); }

    // lost_power reads the VL bit (bit7 of seconds)
    mock_reset();
    { uint8_t r1[] = { 0x80 }; mock_response(r1, 1); bool vl=false;
      pcf8563_lost_power(&g_bus, PCF8563_ADDR, &vl); ok("lost_power true when VL set", vl); }
    mock_reset();
    { uint8_t r0[] = { 0x30 }; mock_response(r0, 1); bool vl=true;
      pcf8563_lost_power(&g_bus, PCF8563_ADDR, &vl); ok("lost_power false when VL clear", !vl); }

    // ===== SSD1306 ===========================================================
    printf("\nSSD1306:\n");
    static ssd1306_t d;
    mock_reset();
    ssd1306_init(&d, &g_bus, 0, 0, 0);   // defaults -> 0x3C, 128x64
    ok("init defaults to 0x3C / 128x64 / 8 pages",
       d.addr == 0x3C && d.w == 128 && d.h == 64 && d.pages == 8);
    { const uint8_t exp[] = { 0x00, /* command stream */
        0xAE, 0xD5,0x80, 0xA8,0x3F, 0xD3,0x00, 0x40, 0x8D,0x14, 0x20,0x00,
        0xA1, 0xC8, 0xDA,0x12, 0x81,0xCF, 0xD9,0xF1, 0xDB,0x40, 0xA4, 0xA6, 0xAF };
      int good = g_ntx == 3 && g_tx[0].op == 'W' && g_tx[0].len == sizeof exp &&
                 bytes_eq(g_tx[0].data, exp, sizeof exp);
      ok("init command stream (mux 0x3F, compins 0x12)", good);
      if (!good) { dump("got", g_tx[0].data, g_tx[0].len); dump("exp", exp, sizeof exp); } }
    { const uint8_t addr_cmd[] = { 0x00, 0x21,0,127, 0x22,0,7 };   // tx[1]
      ok("show addressing window (cols 0..127, pages 0..7)",
         g_tx[1].len == sizeof addr_cmd && bytes_eq(g_tx[1].data, addr_cmd, sizeof addr_cmd)); }
    { int good = g_tx[2].op == 'W' && g_tx[2].len == 1025 && g_tx[2].data[0] == 0x40;
      for (size_t i = 1; i < 1025; i++) if (g_tx[2].data[i] != 0) good = 0;   // cleared fb
      ok("show data stream = 0x40 + 1024 blank bytes", good); }

    // framebuffer layout: drawing 'A' at (0,0) lands the font bytes verbatim in
    // fb[0..4] (proves the column-major / bit-is-row mapping + char() + pixel()).
    ssd1306_clear(&d);
    ssd1306_char(&d, 0, 0, 'A');
    { const uint8_t *fb = SSD1306_FB(&d);
      int good = bytes_eq(fb, font5x7['A' - 0x20], 5);
      ok("draw 'A' at (0,0) -> fb[0..4] == font glyph", good);
      if (!good) { dump("fb ", fb, 5); dump("'A'", font5x7['A'-0x20], 5); } }

    // and it ships in the next show() right after the 0x40 control byte
    mock_reset();
    ssd1306_show(&d);
    ok("show ships that glyph at GDDRAM col 0",
       g_tx[1].data[0] == 0x40 && bytes_eq(&g_tx[1].data[1], font5x7['A' - 0x20], 5));

    printf("\n%d/%d checks passed%s\n", g_pass, g_pass + g_fail,
           g_fail == 0 ? " -- C drivers emit the same wire as the Lua twins" : "");
    return g_fail == 0 ? 0 : 1;
}
