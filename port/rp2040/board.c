// ============================================================================
// board.c — RP2040 / Pico W board glue. See board.h.
// ============================================================================
#include "board.h"
#include "pico/stdlib.h"

uint32_t board_millis(void) {
    return to_ms_since_boot(get_absolute_time());
}

void board_init(void) {
    // stdio (USB) is brought up by the app; nothing else needed yet.
}
