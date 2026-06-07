// ============================================================================
// phy_pio_rs485.c — RP2040 PIO implementation of bus_phy.h (9-bit RS-485 PHY).
//
// Two PIO state machines on pio0: uart_tx9 (TX, side-set drives the line) and
// uart_rx9 (RX, autopush a right-justified 9-bit word). Fixed 115200, LSB-first,
// 8 cycles/bit. No DE (auto-direction transceivers / bare-TTL); half-duplex
// self-echo is handled by the caller's skip discipline if a shared bus is used.
//
// RX pin defaults to BUS_PIN_RO (GP21). Define PHY_RX_PIN = BUS_PIN_DI to make
// the RX SM sample the TX pin (GP20) for an internal loopback self-test with no
// external wiring.
// ============================================================================
#include "bus_phy.h"
#include "board.h"
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "rs485.pio.h"

#ifndef PHY_RX_PIN
#define PHY_RX_PIN  BUS_PIN_RO
#endif

#define PHY_PIO     pio0
#define BITS_PER    8u          // PIO cycles per bit

static uint     s_sm_tx, s_sm_rx;
static uint     s_off_tx, s_off_rx;
static uint32_t s_baud = BUS_DEFAULT_BAUD;
static uint32_t s_rx_words, s_rx_overrun;

static void apply_clkdiv(uint32_t baud) {
    float div = (float)clock_get_hz(clk_sys) / (float)(BITS_PER * baud);
    pio_sm_set_clkdiv(PHY_PIO, s_sm_tx, div);
    pio_sm_set_clkdiv(PHY_PIO, s_sm_rx, div);
}

void bus_phy_init(uint32_t baud) {
    if (baud) s_baud = baud;
    s_rx_words = s_rx_overrun = 0;

    s_sm_tx = (uint)pio_claim_unused_sm(PHY_PIO, true);
    s_sm_rx = (uint)pio_claim_unused_sm(PHY_PIO, true);
    s_off_tx = pio_add_program(PHY_PIO, &uart_tx9_program);
    s_off_rx = pio_add_program(PHY_PIO, &uart_rx9_program);

    // ---- TX SM: out + side-set on BUS_PIN_DI (GP20), idle high -------------
    pio_sm_set_pins_with_mask(PHY_PIO, s_sm_tx, 1u << BUS_PIN_DI, 1u << BUS_PIN_DI);
    pio_sm_set_pindirs_with_mask(PHY_PIO, s_sm_tx, 1u << BUS_PIN_DI, 1u << BUS_PIN_DI);
    pio_gpio_init(PHY_PIO, BUS_PIN_DI);
    pio_sm_config ct = uart_tx9_program_get_default_config(s_off_tx);
    sm_config_set_out_pins(&ct, BUS_PIN_DI, 1);
    sm_config_set_sideset_pins(&ct, BUS_PIN_DI);
    sm_config_set_out_shift(&ct, true, false, 32);   // shift right (LSB first), manual pull
    sm_config_set_fifo_join(&ct, PIO_FIFO_JOIN_TX);
    pio_sm_init(PHY_PIO, s_sm_tx, s_off_tx, &ct);

    // ---- RX SM: sample PHY_RX_PIN, autopush 9-bit words -------------------
    pio_gpio_init(PHY_PIO, PHY_RX_PIN);
    gpio_pull_up(PHY_RX_PIN);   // idle-high: no spurious RX on a floating/idle line
    // In loopback (RX pin == TX pin) the pad must stay an OUTPUT so TX can drive
    // it — the input synchronizer reads the driven value regardless. Only force
    // the pad to input when RX is a distinct pin (the real bus case).
    if (PHY_RX_PIN != BUS_PIN_DI)
        pio_sm_set_consecutive_pindirs(PHY_PIO, s_sm_rx, PHY_RX_PIN, 1, false);
    pio_sm_config cr = uart_rx9_program_get_default_config(s_off_rx);
    sm_config_set_in_pins(&cr, PHY_RX_PIN);          // 'wait 0 pin 0' + 'in pins'
    sm_config_set_in_shift(&cr, true, true, 9);      // shift right, autopush at 9 bits
    sm_config_set_fifo_join(&cr, PIO_FIFO_JOIN_RX);
    pio_sm_init(PHY_PIO, s_sm_rx, s_off_rx, &cr);

    apply_clkdiv(s_baud);
    pio_sm_set_enabled(PHY_PIO, s_sm_tx, true);
    pio_sm_set_enabled(PHY_PIO, s_sm_rx, true);
}

void bus_phy_set_baud(uint32_t baud) {
    if (!baud) return;
    s_baud = baud;
    apply_clkdiv(s_baud);
}

bool bus_phy_send_words(const uint16_t *words, uint16_t n) {
    if (!words || n == 0) return false;
    for (uint16_t i = 0; i < n; i++)
        pio_sm_put_blocking(PHY_PIO, s_sm_tx, (uint32_t)(words[i] & 0x1FFu));
    return true;
}

bool bus_phy_tx_busy(void) {
    // Busy until the TX FIFO drains and the SM is back to the stalling pull.
    return !pio_sm_is_tx_fifo_empty(PHY_PIO, s_sm_tx);
}

bool bus_phy_rx_pop(uint16_t *word9) {
    if (pio_sm_is_rx_fifo_empty(PHY_PIO, s_sm_rx)) return false;
    uint32_t w = pio_sm_get(PHY_PIO, s_sm_rx);
    s_rx_words++;
    // shift-right ISR autopushed at 9 bits -> the word sits in the TOP 9 bits
    // [31:23], LSB-first order preserved. Right-justify it.
    if (word9) *word9 = (uint16_t)((w >> 23) & 0x1FFu);
    return true;
}

void bus_phy_rx_flush(void) {
    pio_sm_clear_fifos(PHY_PIO, s_sm_rx);
}

uint32_t bus_phy_rx_overrun(void) {
    // PIO RX-FIFO overflow shows in FDEBUG RXSTALL for this SM.
    if (PHY_PIO->fdebug & (1u << (PIO_FDEBUG_RXSTALL_LSB + s_sm_rx))) s_rx_overrun++;
    PHY_PIO->fdebug = (1u << (PIO_FDEBUG_RXSTALL_LSB + s_sm_rx));
    return s_rx_overrun;
}
uint32_t bus_phy_rx_words(void) { return s_rx_words; }
