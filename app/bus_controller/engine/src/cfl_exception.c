/* Embedded chain_tree exception handler (replaces the host one, which uses
 * execinfo/backtrace). The contract (cfl_exception.h): print + spin until the
 * watchdog fires. Here: hand attribution to a weak firmware hook, disable IRQ,
 * and spin so the core's hardware watchdog resets the chip. */
#include "cfl_exception.h"

/* Firmware override: record {file,func,line,msg} to the crash slot before reset.
 * Default no-op; the WDT-spin below is the actual recovery. */
__attribute__((weak)) void cfl_embed_panic(const char *file, const char *func,
                                           uint16_t line, const char *msg) {
    (void)file; (void)func; (void)line; (void)msg;
}

void cfl_exception_handler(const char *file, const char *func, uint16_t line, const char *msg) {
    cfl_embed_panic(file, func, line, msg);
    __asm volatile ("cpsid i" ::: "memory");   /* disable IRQ; HW watchdog resets us */
    for (;;) { }
}
