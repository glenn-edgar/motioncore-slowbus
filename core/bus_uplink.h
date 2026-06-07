// ============================================================================
// bus_uplink.h — northbound uplink seam: what the bus controller exchanges with
// the outside world *besides* the bus itself.
//
// The fast-bus controller is an active node: it originates traffic to slaves,
// and it also bridges messages to/from an off-bus host. That host link is the
// second portability boundary. Link-time selected, same as bus_phy.h:
//
//   port/rp2350/uplink_usb_cdc.c   USB-CDC to a Linux/Pi host   (now)
//   port/nrf53/uplink_thread.c     OpenThread mesh bridge       (later, Nordic BC)
//
// Keeping this behind a seam means "the BC generates and routes messages" is
// decoupled from "where those messages come from" — a USB host today, a Thread
// network on the Nordic M33 BC down the line, with the same scheduler core.
//
// A node firmware (ROLE=slave) links a no-op uplink: slaves have no host.
// ============================================================================
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "bus_frame.h"

void bus_uplink_init(void);

// Pull the next host->bus message, if any. Fills *dest with the target node
// address and copies up to `max` payload bytes into `buf`. Returns the payload
// length, or 0 if nothing is pending (or -1 on a malformed host frame).
int bus_uplink_poll(uint8_t *dest, uint8_t *buf, int max);

// Push a bus->host message up to the host, tagged with the originating node
// address. Used to forward slave replies / events / peer traffic the BC mirrors.
void bus_uplink_send(uint8_t src, const uint8_t *payload, int len);

// Service the uplink transport (USB device task, etc.). Call from the uplink
// task each loop. No-op for ports that are fully interrupt-driven.
void bus_uplink_task(void);
