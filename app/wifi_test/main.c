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
#include "FreeRTOS.h"
#include "task.h"
#include "boot_netcfg.h"

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

    printf("[wifi_test] joining \"%s\" ...\n", nc.ssid);
    int r = cyw43_arch_wifi_connect_timeout_ms(nc.ssid, nc.pass, CYW43_AUTH_WPA2_AES_PSK, 30000);
    if (r) { printf("[wifi_test] connect FAILED rc=%d\n", r); for (;;) vTaskDelay(1000); }

    struct netif *nif = &cyw43_state.netif[CYW43_ITF_STA];
    for (;;) {
        printf("[wifi_test] JOINED \"%s\"  ip=%s  link=%d\n",
               nc.ssid, ip4addr_ntoa(netif_ip4_addr(nif)), netif_is_link_up(nif));
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
