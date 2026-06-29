// ============================================================================
// il_pin_table.c — RP2040 interlock-DSL pin-name resolution. See il_pin_table.h.
// ============================================================================
#include "il_pin_table.h"

#define IL_NGPIO  30u   // RP2040 GPIO 0..29

// Per-GPIO static records; board_pin_lookup returns a stable pointer into this
// (single-threaded parser, but per-GPIO storage avoids any aliasing anyway).
static board_pin_t g_pins[IL_NGPIO];

// case-insensitive single char compare
static int ci_eq(char a, char b) {
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    return a == b;
}

// Parse `len` decimal digits at s into *out (0..999). Returns 0 on success.
static int parse_uint(const char* s, uint8_t len, unsigned* out) {
    if (len == 0 || len > 3) return -1;
    unsigned v = 0;
    for (uint8_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10u + (unsigned)(s[i] - '0');
    }
    *out = v;
    return 0;
}

static uint8_t adc_channel_for_gpio(uint8_t gpio) {
    if (gpio >= 26u && gpio <= 28u) return (uint8_t)(gpio - 26u);   // ADC0..2
    return BOARD_PIN_ADC_NONE;
}

const board_pin_t* board_pin_lookup(const char* label, uint8_t len) {
    if (len < 3) return 0;   // shortest is "gpN"

    // "adcK" / "ainK"  -> GP(26+K), channel K  (K in 0..2)
    if ((ci_eq(label[0], 'a')) &&
        ((ci_eq(label[1], 'd') && ci_eq(label[2], 'c')) ||
         (ci_eq(label[1], 'i') && ci_eq(label[2], 'n')))) {
        unsigned k;
        if (parse_uint(label + 3, (uint8_t)(len - 3), &k) != 0) return 0;
        if (k > 2u) return 0;
        uint8_t gpio = (uint8_t)(26u + k);
        g_pins[gpio].gpio = gpio;
        g_pins[gpio].adc_channel = (uint8_t)k;
        return &g_pins[gpio];
    }

    // "gpN" -> GPIO N
    if (ci_eq(label[0], 'g') && ci_eq(label[1], 'p')) {
        unsigned n;
        if (parse_uint(label + 2, (uint8_t)(len - 2), &n) != 0) return 0;
        if (n >= IL_NGPIO) return 0;
        g_pins[n].gpio = (uint8_t)n;
        g_pins[n].adc_channel = adc_channel_for_gpio((uint8_t)n);
        return &g_pins[n];
    }

    return 0;
}
