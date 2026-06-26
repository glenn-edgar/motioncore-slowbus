// ============================================================================
// wifi_test/main.c — W2a standalone smoke test: bring up CYW43 + lwIP, join the
// AP from the 'neti' config file, get a DHCP address. Proves the WiFi stack
// builds and associates BEFORE it's integrated into bus_controller_wifi.
//
// Text stdio over USB-CDC (NOT the libcomm binary stream) so it reads with a
// plain serial monitor. Needs a 'neti' config flashed (idnt+neti is enough; the
// interlock/hwio config is irrelevant to this test).
// ============================================================================
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "boot_netcfg.h"

// W2b: dial the agent endpoint over TCP and round-trip a test string (proves the
// lwIP BSD socket client works over WiFi). Pair with a TCP echo server on the Pi.
static void tcp_echo_once(const netcfg_t *nc) {
    int s = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { printf("[wifi_test] socket() failed\n"); return; }
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port   = lwip_htons(nc->port);
    a.sin_addr.s_addr = (uint32_t)nc->ip[0] | ((uint32_t)nc->ip[1] << 8) |
                        ((uint32_t)nc->ip[2] << 16) | ((uint32_t)nc->ip[3] << 24);
    if (lwip_connect(s, (struct sockaddr *)&a, sizeof a) != 0) {
        printf("[wifi_test] connect %u.%u.%u.%u:%u FAILED\n",
               nc->ip[0], nc->ip[1], nc->ip[2], nc->ip[3], nc->port);
        lwip_close(s); return;
    }
    static const char msg[] = "PING-over-wifi";
    lwip_write(s, msg, sizeof msg - 1);
    char buf[64]; int n = lwip_read(s, buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = 0;
        printf("[wifi_test] TCP echo rx %d=\"%s\" %s\n", n, buf,
               (n == (int)(sizeof msg - 1) && memcmp(buf, msg, n) == 0) ? "OK ✓" : "MISMATCH");
    } else {
        printf("[wifi_test] TCP read n=%d\n", n);
    }
    lwip_close(s);
}

// UDP probe: mirrors the bus_controller UDP path with per-call prints, so a blocking
// lwIP call freezes the console at its line (no watchdog here to mask it).
static void udp_echo_once(const netcfg_t *nc) {
    printf("[udp] socket()...\n");
    int s = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    printf("[udp] socket -> %d\n", s);
    if (s < 0) return;
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port   = lwip_htons(nc->port);
    a.sin_addr.s_addr = (uint32_t)nc->ip[0] | ((uint32_t)nc->ip[1] << 8) |
                        ((uint32_t)nc->ip[2] << 16) | ((uint32_t)nc->ip[3] << 24);
    printf("[udp] connect %u.%u.%u.%u:%u ...\n", nc->ip[0],nc->ip[1],nc->ip[2],nc->ip[3],nc->port);
    int rc = lwip_connect(s, (struct sockaddr *)&a, sizeof a);
    printf("[udp] connect -> %d\n", rc);
    if (rc != 0) { lwip_close(s); return; }
    static const char msg[] = "UDP-over-wifi";
    printf("[udp] write %u ...\n", (unsigned)(sizeof msg - 1));
    int w = lwip_write(s, msg, sizeof msg - 1);
    printf("[udp] write -> %d\n", w);
    for (int i = 0; i < 20; i++) {        // poll ~1s for the echo, non-blocking
        char buf[64];
        int n = lwip_recv(s, buf, sizeof buf - 1, MSG_DONTWAIT);
        if (n > 0) { buf[n] = 0; printf("[udp] recv %d=\"%s\" OK\n", n, buf); break; }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    lwip_close(s);
    printf("[udp] closed\n");
}

static void wifi_task(void *arg) {
    (void)arg;
    // Give the USB-CDC console a moment to enumerate so the first lines aren't lost.
    vTaskDelay(pdMS_TO_TICKS(2500));

    netcfg_t nc;
    int rc = boot_read_netcfg(&nc);
    printf("[wifi_test] neti rc=%d ssid=\"%s\" agent=%u.%u.%u.%u:%u pwlen=%u\n",
           rc, nc.ssid, nc.ip[0], nc.ip[1], nc.ip[2], nc.ip[3], nc.port,
           (unsigned)strlen(nc.pass));
    if (rc != NETI_OK) { printf("[wifi_test] no usable 'neti' — abort\n"); for (;;) vTaskDelay(1000); }

    if (cyw43_arch_init()) { printf("[wifi_test] cyw43_arch_init FAILED\n"); for (;;) vTaskDelay(1000); }
    cyw43_arch_enable_sta_mode();

    int r = -1;
    for (int attempt = 1; attempt <= 10 && r; attempt++) {
        printf("[wifi_test] joining \"%s\" (attempt %d) ...\n", nc.ssid, attempt);
        r = cyw43_arch_wifi_connect_timeout_ms(nc.ssid, nc.pass, CYW43_AUTH_WPA2_AES_PSK, 20000);
        if (r) { printf("[wifi_test] join rc=%d, retrying\n", r); vTaskDelay(pdMS_TO_TICKS(2000)); }
    }
    if (r) { printf("[wifi_test] join gave up rc=%d\n", r); for (;;) vTaskDelay(1000); }

    struct netif *nif = &cyw43_state.netif[CYW43_ITF_STA];
    printf("[wifi_test] JOINED \"%s\"  ip=%s  link=%d\n",
           nc.ssid, ip4addr_ntoa(netif_ip4_addr(nif)), netif_is_link_up(nif));
    for (;;) {
        udp_echo_once(&nc);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

// ---- FreeRTOS hooks (standalone; bus_controller has its own in its main.c) --
void chassis_assert(int line) { (void)line; for (;;) tight_loop_contents(); }
void vApplicationMallocFailedHook(void) { for (;;) tight_loop_contents(); }
void vApplicationStackOverflowHook(TaskHandle_t t, char *n) { (void)t; (void)n; for (;;) tight_loop_contents(); }
void vApplicationGetIdleTaskMemory(StaticTask_t **tcb, StackType_t **stk, uint32_t *sz) {
    static StaticTask_t t; static StackType_t s[configMINIMAL_STACK_SIZE];
    *tcb = &t; *stk = s; *sz = configMINIMAL_STACK_SIZE;
}
void vApplicationGetPassiveIdleTaskMemory(StaticTask_t **tcb, StackType_t **stk, uint32_t *sz, BaseType_t core) {
    static StaticTask_t t[configNUMBER_OF_CORES - 1];
    static StackType_t  s[configNUMBER_OF_CORES - 1][configMINIMAL_STACK_SIZE];
    *tcb = &t[core]; *stk = s[core]; *sz = configMINIMAL_STACK_SIZE;
}
void vApplicationGetTimerTaskMemory(StaticTask_t **tcb, StackType_t **stk, uint32_t *sz) {
    static StaticTask_t t; static StackType_t s[configTIMER_TASK_STACK_DEPTH];
    *tcb = &t; *stk = s; *sz = configTIMER_TASK_STACK_DEPTH;
}

int main(void) {
    stdio_init_all();
    xTaskCreate(wifi_task, "wifi", 2048, NULL, tskIDLE_PRIORITY + 1, NULL);
    vTaskStartScheduler();
    for (;;) tight_loop_contents();
}
