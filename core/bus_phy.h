// ============================================================================
// bus_phy.h — southbound PHY seam: the 9-bit half-duplex RS-485 line.
//
// This is the portability boundary between the chip-independent protocol core
// and the per-chip electrical layer. The core (frame codec, scheduler, node
// logic) calls only these functions; a port supplies one implementation:
//
//   port/rp2350/phy_pio_rs485.c   PIO state machine + DMA, 9-bit, DE side-set   (now)
//   port/stm32/phy_usart_rs485.c  native 9-bit USART + address-mark mode        (later)
//
// The port is chosen at link time (one PHY per firmware image) — no runtime
// vtable. A future chip is a new directory implementing this same header, not a
// rewrite. The 9-bit word convention (bit8 = address marker) is in bus_addr.h.
//
// Contract:
//   * TX accepts an array of pre-framed 9-bit words (built by bus_frame_encode)
//     and drives DE for the duration, releasing it only after the last stop bit.
//   * RX is interrupt/DMA-driven into a ring the PHY owns; the core drains it
//     with bus_phy_rx_pop() and runs bus_asm_feed(). On a shared half-duplex
//     pair the node hears its own TX — the PHY discards that self-echo so the
//     core never sees it (see the port's shared-bus handling).
// ============================================================================
#pragma once

#include <stdint.h>
#include <stdbool.h>

// Bring up the line at `baud` (target: 400000). Idempotent re-init allowed.
void bus_phy_init(uint32_t baud);

// Change baud on the fly (peripheral briefly disabled). 0 = leave unchanged.
void bus_phy_set_baud(uint32_t baud);

// Transmit `n` pre-framed 9-bit words. Blocks until the line is idle and DE has
// released. Self-echo (shared bus) is consumed by the PHY, not returned on RX.
// Returns false if a transmit is already in flight (async path) or n is invalid.
bool bus_phy_send_words(const uint16_t *words, uint16_t n);

// True while a transmit is still draining (async path).
bool bus_phy_tx_busy(void);

// Pop one received 9-bit word from the PHY's RX ring. Returns false if empty.
// bit8 = address marker (see bus_addr.h). Call in a loop, then bus_asm_feed().
bool bus_phy_rx_pop(uint16_t *word9);

// Drop all buffered RX (ring + any half-word state). Call before listening for
// a reply so stale idle traffic can't complete a transaction early.
void bus_phy_rx_flush(void);

// --- Diagnostics ------------------------------------------------------------
uint32_t bus_phy_rx_overrun(void);   // ring-full / framing / overrun drops
uint32_t bus_phy_rx_words(void);     // good words heard (any bus traffic)
