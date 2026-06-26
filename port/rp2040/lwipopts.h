// ============================================================================
// lwipopts.h — lwIP config for the Pico W (CYW43) WiFi uplink, FreeRTOS sys mode.
// Used only by WIFI=1 builds (pico_cyw43_arch_lwip_sys_freertos). Modeled on the
// canonical pico-examples FreeRTOS lwipopts: NO_SYS=0, BSD sockets enabled (the
// uplink dials the zenoh-agent with a blocking TCP client), DHCP for the address.
// ============================================================================
#ifndef _LWIPOPTS_H
#define _LWIPOPTS_H

// ---- core mode -------------------------------------------------------------
#define NO_SYS                      0    // FreeRTOS sys mode (threads + sockets)
#define LWIP_SOCKET                 1    // BSD socket API (TCP client)
#define LWIP_NETCONN                1
#define SYS_LIGHTWEIGHT_PROT        1
// Per-netconn sems (not per-thread) — avoids needing FreeRTOS thread-local storage
// (configNUM_THREAD_LOCAL_STORAGE_POINTERS). The uplink reads+writes from ONE task,
// so full-duplex (separate reader/writer threads) isn't needed either.
#define LWIP_NETCONN_SEM_PER_THREAD 0
#define LWIP_NETCONN_FULLDUPLEX     0
#define LWIP_SO_RCVTIMEO            1    // honor SO_RCVTIMEO (uplink loop relies on it)
#define LWIP_SO_SNDTIMEO            1
#define LWIP_TIMEVAL_PRIVATE        0    // use <sys/time.h> for struct timeval

// ---- memory ----------------------------------------------------------------
#define MEM_LIBC_MALLOC             0
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    8000
#define MEMP_NUM_TCP_SEG            32
#define MEMP_NUM_ARP_QUEUE          10
#define PBUF_POOL_SIZE              24

// ---- protocols -------------------------------------------------------------
#define LWIP_IPV4                   1
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_DNS                    1
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define LWIP_DHCP                   1
#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DHCP_DOES_ACD_CHECK    0

#define LWIP_TCP_KEEPALIVE          1
#define LWIP_NETIF_TX_SINGLE_PBUF   1
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1

#define TCP_MSS                     1460
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))

// ---- FreeRTOS sys layer ----------------------------------------------------
#define TCPIP_THREAD_STACKSIZE      1024
#define DEFAULT_THREAD_STACKSIZE    1024
#define DEFAULT_RAW_RECVMBOX_SIZE   8
#define TCPIP_MBOX_SIZE             8
#define DEFAULT_TCP_RECVMBOX_SIZE   8
#define DEFAULT_ACCEPTMBOX_SIZE     8
#define LWIP_TCPIP_CORE_LOCKING_INPUT 1

// ---- stats / debug off -----------------------------------------------------
#define MEM_STATS                   0
#define SYS_STATS                   0
#define MEMP_STATS                  0
#define LINK_STATS                  0
#define LWIP_STATS                  0
#define LWIP_DEBUG                  0

#endif // _LWIPOPTS_H
