// samd21_sync.h -- bounded peripheral-wait helper (defensive technique #7).
//
// Bare-metal SAMD21 code is full of register-sync spin loops:
//   while (GCLK->STATUS.bit.SYNCBUSY) { }
//   while (!ADC->INTFLAG.bit.RESRDY) { }
//   while (NVMCTRL->INTFLAG.bit.READY == 0) { }
// Unbounded, any one of these hangs the chip forever if its peripheral wedges
// (bad clock, brown-out, stuck I2C bus). The WDT eventually resets, but ~4 s
// later and with no record of WHICH wait hung.
//
// BOUNDED_SPIN() wraps the same condition in a tick-counted loop: it spins
// exactly as before in the normal case (the condition clears in a few
// iterations), but if the budget expires it panic()s with the source line as
// the arg. The post-mortem (panic_code=PANIC_PERIPHERAL_TIMEOUT, panic_arg=line,
// last_pc=caller) then names the exact wedged site.
//
// Budget sizing (SYNC_SPIN_BUDGET): must exceed the LONGEST legitimate wait and
// fall under the WDT period (PER_4K ~= 4 s):
//   - SYNCBUSY / RESRDY clear in << 10 us  -> never near the budget.
//   - NVM row-erase (the slowest legit wait) ~ 6 ms.
//   - At 2..8 cycles/iter @ 48 MHz, 4,000,000 iters = ~0.17..0.7 s: safely
//     above 6 ms (no false trip) and below 4 s (panic beats the WDT, so the
//     timeout is recorded rather than lost to an anonymous watchdog reset).
//
// Safe from earliest boot (the GCLK waits run before USB/main) and from ISRs:
// panic() only writes the .noinit crash slot and calls NVIC_SystemReset().

#ifndef SAMD21_SYNC_H
#define SAMD21_SYNC_H

#include <stdint.h>
#include "samd21_interlocks.h"   // panic(), PANIC_PERIPHERAL_TIMEOUT

#define SYNC_SPIN_BUDGET  4000000u

// Spin while (cond) is true, bounded. On budget expiry, panic with the call
// site's line number. __LINE__ in a function-like macro expands at the point of
// invocation, so each call records its own site.
#define BOUNDED_SPIN(cond)                                              \
    do {                                                                \
        uint32_t _spin_budget = SYNC_SPIN_BUDGET;                       \
        while (cond) {                                                  \
            if (__builtin_expect(--_spin_budget == 0u, 0)) {           \
                panic(PANIC_PERIPHERAL_TIMEOUT, (uint32_t)__LINE__);    \
            }                                                           \
        }                                                               \
    } while (0)

#endif // SAMD21_SYNC_H
