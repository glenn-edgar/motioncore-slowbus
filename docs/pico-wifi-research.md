# Pico / Pico 2 W WiFi — known issues & workarounds (research 2026-06-25)

Deep web/forum research (pico-sdk + cyw43-driver GitHub issues, RPi forums, lwIP)
scoped to our stack: `pico_cyw43_arch_lwip_sys_freertos` on RP2040 Pico W under
FreeRTOS-SMP, lwIP BSD sockets, blocking TCP client, 4 s heartbeat watchdog.
16 findings survived 3-vote adversarial verification. Anchored to **pico-sdk ≤ 2.1.1**
(cyw43 driver rev `c1075d4b`) — re-verify against newer SDKs.

## Validates our W2c design
- **`recv()==0` on `SO_RCVTIMEO` timeout is a real lwIP behavior** (not a clean peer
  close). Our handling — treat `recv<=0` as "no data, keep looping"; let a failed
  `write` detect a dead link — is correct. `LWIP_SO_RCVTIMEO=1` is required (we set it).
  [esp-idf#2540; lwIP socket layer]
- **`rc=-2` join + blocking-join wedge** root cause: blocking `cyw43_arch_wifi_connect_*`
  uses `best_effort_wfe_or_timeout`, which **starves the lwIP tcpip task** so DHCP
  retries never transmit → join fails early; and the long block starves USB/BOOTSEL.
  Maintainer fix = **exactly our approach**: `cyw43_arch_wifi_connect_async` + poll
  `cyw43_tcpip_link_status`, non-blocking, heartbeats stay alive. [pico-sdk #917, #1054]

## TODO — hardening items for W3 / the WiFi supervisor
1. **Disable WiFi power-save** (HIGH value). Default PM silently disassociates while
   link status still says "connected" → periodic drops. After join:
   `cyw43_wifi_pm(&cyw43_state, CYW43_DEFAULT_PM & ~0xf);` (= `0xa11140`, NO_POWERSAVE).
   `CYW43_PERFORMANCE_PM` does NOT disable PM. [pico-sdk #2153, forum t=344058]
2. **Reconnect supervisor**: the CYW43 does NOT auto-reconnect after `CYW43_EV_DISASSOC`.
   Our re-dial loop rejoins only when there's no IP; also **rejoin on link-down even if
   the IP lingers** (poll `cyw43_wifi_link_status`/`cyw43_tcpip_link_status`). [#2316, #2153]
3. **`TCPIP_THREAD_PRIO` > app task priority** in lwipopts (belt-and-suspenders). [#917]
4. **SMP init fragility**: `cyw43_arch_init()` from inside a FreeRTOS task under SMP can
   hang at `multicore_fifo_pop_blocking` (#1718). Works for us; watch it. Respect
   `CYW43_TASK_PRIORITY` / be deliberate about init core/timing.

## lwIP memory tuning (throughput / pbuf exhaustion)
Example values (pico-examples `lwipopts_examples_common.h`): `MEM_SIZE 4000`,
`PBUF_POOL_SIZE 24`, `MEMP_NUM_TCP_SEG 32`, `TCP_MSS 1460`, `TCP_WND`/`TCP_SND_BUF`
`(8*TCP_MSS)`. Raise `TCP_WND`/`TCP_SND_BUF`/`MEM_SIZE`/`PBUF_POOL_SIZE` for throughput
and to avoid pbuf exhaustion. (Our lwipopts already in this ballpark.) Note: the official
HTTP-client example **leaks a pbuf** (no `pbuf_free` in the recv cb) → ~`PBUF_POOL_SIZE`
requests then stalls — don't copy that pattern. [pico-examples #605]

## Pico 2 W / RP2350
Same CYW43439 driver + behaviors as Pico W. FreeRTOS/RP2350 support was still in a
**submodule** (not an official FreeRTOS-Kernel release) as of pico-sdk 2.1.1 → build
FreeRTOS-Kernel from main + init submodules for the future Pico2 build. No confirmed
unique RP2350 *WiFi* silicon bug in these reports (several such theories were refuted) —
it's tooling maturity + shared-driver behavior. **pico-sdk 2.1.1** updated the cyw43
driver to `c1075d4b` and "fixed a rare firmware-load issue" — be on ≥2.1.1.

## Unresolved (research couldn't pin; matches our experience)
- Exact `recv()` SO_RCVTIMEO return (0 vs −1/EAGAIN/ETIMEDOUT) is version-dependent —
  handle empirically (we do).
- **Why non-blocking `connect()`+`select()` didn't detect completion (our symptom d)** —
  no source diagnosed it; our blocking-connect workaround stands.

## Refuted (do NOT act on)
- "async connect runs once and never retries" — false; cause is tcpip-task starvation.
- `MEMP_NUM_SYS_TIMEOUT` sys_timeout-panic fix; "detect crypto failures vs link status";
  several RP2350-specific init-fault theories (GPIO-IRQ assertion, VGA core contention).

Key sources: pico-sdk #917, #1054, #2153, #2316, #1718; pico-examples #605 +
`lwipopts_examples_common.h`; pico-sdk 2.1.1 release notes; RPi forums t=344058, t=346686.
