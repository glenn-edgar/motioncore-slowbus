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
3. **DO NOT raise `TCPIP_THREAD_PRIO`** (the #917 "belt-and-suspenders" tip is WRONG for
   our safety architecture — it's the ESP32-style footgun). We use the async polled join,
   so we don't need it. Keep `TCPIP_THREAD_PRIO` at the lwIP default (1) so it stays BELOW
   the interlock (prio 4) and chain-tree engine (prio 3) — they always preempt it.
4. **Core affinity is the real guarantee (the deferred "core-affinity thread-review").**
   Interlock (4) + engine (3) are pinned to **core1**; our `wifi_uplink_task` (2) to
   **core0**. The CYW43 worker defaults to **prio 4 (== interlock!)** but the SDK pins it
   to whichever core ran `cyw43_arch_init()` — we init from the core0-pinned uplink task,
   so it lands on **core0**, off the safety core. W3: (a) confirm/pin the lwIP **tcpip
   thread** to core0 (default prio 1 makes it harmless even on core1, but pin for
   airtightness); (b) the CYW43-worker==interlock prio tie is safe only because they're on
   different cores — never let a WiFi thread onto core1 (or bump the interlock to 5).
5. **SMP init fragility**: `cyw43_arch_init()` from inside a FreeRTOS task under SMP can
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
Same CYW43439 driver + behaviors as Pico W. No confirmed unique RP2350 *WiFi* silicon bug
in these reports (several such theories were refuted) — it's tooling maturity + shared
driver behavior. **pico-sdk 2.1.1** updated the cyw43 driver to `c1075d4b` and "fixed a
rare firmware-load issue" — be on ≥2.1.1.

**RP2350 dual-core FreeRTOS is ALREADY WORKING in `~/xiao_blocks/firmware/rp2350` (branch
`pico2`)** — supersedes the research's "submodule-only/not-in-official-release" caveat for
us. Reuse it for the Pico 2 W build:
- `FreeRTOSConfig.h`: `configNUMBER_OF_CORES 2` + `configRUN_MULTIPLE_PRIORITIES 1` +
  `configUSE_CORE_AFFINITY 1` (full SMP + affinity — what our interlock/engine pinning needs).
- Kernel: **`RP2350_ARM_NTZ`** port — `include(${FREERTOS_KERNEL_PATH}/portable/ThirdParty/
  GCC/RP2350_ARM_NTZ/FreeRTOS_Kernel_import.cmake)`; `FREERTOS_KERNEL_PATH=~/pico/FreeRTOS-Kernel`
  on the Pi already has the port. `PICO_PLATFORM=rp2350-arm-s`. Has a `bringup.uf2` SMP diag.
- CAVEAT: XIAO RP2350 has **no CYW43/WiFi**. So this gives the RP2350 SMP FreeRTOS base only;
  Pico 2 W WiFi = that base + our Pico W CYW43/lwIP work + `PICO_BOARD=pico2_w`.

## Recovery ladder — no physical reset (W3) + SAMD51 prior-art
Why the Pico beats a bare ESP32: the **CYW43439 is a separate chip** (SPI/PIO), so a wedged
radio is reset in SOFTWARE (`cyw43_arch_deinit()` + `cyw43_arch_init()` reloads the CYW43
firmware) without an MCU reboot or human power-cycle. Build the WiFi supervisor as a ladder:

1. **Rejoin (software):** poll `cyw43_tcpip_link_status`; on link-down, async rejoin. Rejoin
   on **link-down even if the IP lingers** (CYW43 never auto-reconnects). Handles common drops.
2. **Radio chip-reset:** after N failed rejoins (grace window), `cyw43_arch_deinit()` +
   `cyw43_arch_init()` — resets just the radio, MCU keeps running. (Direct analog of the
   SAMD51 RTL `CHIP_PU` power-cycle: "RTL doesn't reliably re-associate after a clean drop →
   power-cycle the radio" — confirms this layer is necessary, not optional.)
3. **HW watchdog (we have it, 4s):** a true MCU wedge self-resets → boot → rejoin. **This is
   what removes the human-with-a-power-switch the ESP32 needed.**

**Gap:** the watchdog only fires if a heartbeat task stalls. The nasty case is "WiFi dead but
all tasks still heartbeating" → watchdog won't trip → layers 1–2 (the supervisor) must catch
it. So the supervisor + chip-reset are mandatory, not just nice-to-have.

**W3 acceptance test:** with the agent connected, kill WiFi at the AP (or `cyw43_arch_deinit`
it); verify it re-joins + re-dials the agent on its own — no power-cycle.

### SAMD51 RTL8720 port hardening that transfers (xiao_blocks, ~6mo field-tuned)
Same shape (separate radio + dial-agent + libcomm/TCP). Files:
`xiao_blocks/firmware/samd51/{app/bringup/main.c (rtl_task supervisor), app/bringup/gateway.c
(serve loop), port/rtl_wifi.c}`. Copy these patterns (adapt RTL/eRPC → CYW43/lwIP):
- **Grace window before chip-reset:** N transient failures (they used `WIFI_OPEN_FAILS_BEFORE_
  RECOVER=8`, 1s apart = 8s grace) BEFORE power-cycling the radio — don't power-cycle on every
  blip. Post-reset burst-retry (8× ~800ms). Distinguish transient vs persistent.
- **Idle backstop:** count consecutive idle recv timeouts (they used 40 × 3s = ~2min silence)
  → return + re-dial, to catch silent drops without false-tripping on brief gaps.
- **Don't retry after mid-frame data loss → re-dial fresh** (their -2000/-2001 lesson). Our
  analog: on a torn read/desync, close + re-dial; don't reuse the socket. (Our recv≤0 / write-
  failure handling already re-dials.)
- **Watchdog gating:** pet the WDT from a MEDIUM-prio task that time-slices with the WiFi task,
  never a lower-prio task (it starves). We match this (wifi_uplink prio 2, heartbeat-gated WDT).
- **Trace counters** for every exit path (their `g_serve_exit[]`: started/peer-closed/eRPC-broke/
  idle/abort/last-err). Add equivalents to our wifi_uplink — essential for field root-cause.
- **Multi-network fallback + runtime override:** built-in defaults + an SD `wifi.txt` (`ssid;pass`
  per line). Our analog: extend `neti` to hold an ordered list of {ssid,pass}; try until DHCP.
- **Host-side keep-alive:** the agent pinged the device every ~12s to keep the tunnel warm
  (cut their idle-break rate ~3×). **W3 agent TODO:** periodic keep-alive to the BC.

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
