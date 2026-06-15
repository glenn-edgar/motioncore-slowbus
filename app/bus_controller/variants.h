// ============================================================================
// variants.h — the shared product / variant enumeration.
//
// One generic firmware image per (chip x variant). The identity file's "vr"
// field is checked against BUILD_VARIANT at boot (see boot_identity.c), and the
// bus ROLE (master vs slave) is *derived* from the variant code — so the roster
// / uplink boot branch hangs off the variant, not a separate role field.
//
// This enum is the contract both the firmware build and the host config
// generator import: "this image is `vr`" must match "this config is for `vr`".
// Keep it append-only; codes are stamped into on-flash config files.
// ============================================================================
#pragma once

#include <stdint.h>
#include <stdbool.h>

enum {
    VARIANT_NONE          = 0,
    VARIANT_BUS_CTRL_USB  = 1,   // bus controller, USB uplink   (test #1)
    VARIANT_BUS_CTRL_WIFI = 2,   // bus controller, WiFi uplink  (later)
    VARIANT_SLAVE_RS485   = 3,   // peer-capable RS-485 slave    (later)
    // dual_motor and other products append here.
};

// Role derivation: controllers own the bus (master, addr 0x00); every other
// product is a unicast slave. Single source of truth for the boot branch.
static inline bool variant_is_master(uint8_t v) {
    return v == VARIANT_BUS_CTRL_USB || v == VARIANT_BUS_CTRL_WIFI;
}

// The product THIS image was built as. Override per build with -DBUILD_VARIANT;
// defaults to the USB bus controller (the test-#1 image).
#ifndef BUILD_VARIANT
#define BUILD_VARIANT VARIANT_BUS_CTRL_USB
#endif
