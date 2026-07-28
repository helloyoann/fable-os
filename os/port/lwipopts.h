/* lwipopts.h — lwIP configuration for talk-os.
 * Bare-metal, single-threaded (NO_SYS), IPv4 + TCP/UDP + DNS, polled. */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H

/* No OS: use the raw/callback API, no threads, no sockets/netconn. */
#define NO_SYS                      1
#define SYS_LIGHTWEIGHT_PROT        0
#define LWIP_NETCONN                0
#define LWIP_SOCKET                 0
#define LWIP_NETIF_API              0

/* Single network interface. */
#define LWIP_SINGLE_NETIF           1

/* Memory: lwIP-managed pools/heap (no external malloc). TLS records are up to
 * 16 KiB, so the heap and pools are sized generously. */
#define MEM_LIBC_MALLOC             0
#define MEMP_MEM_MALLOC             0
#define MEM_ALIGNMENT               8
#define MEM_SIZE                    (512 * 1024)
#define PBUF_POOL_SIZE              48
#define MEMP_NUM_TCP_SEG            48
#define MEMP_NUM_TCP_PCB            5
#define MEMP_NUM_UDP_PCB            4
#define MEMP_NUM_SYS_TIMEOUT        10
#define MEMP_NUM_ALTCP_PCB          MEMP_NUM_TCP_PCB

/* Protocols. */
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    0
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_DNS                    1
#define DNS_MAX_SERVERS             1
#define LWIP_DHCP                   0
#define LWIP_AUTOIP                 0
#define LWIP_IGMP                   0

/* TCP sizing. */
#define TCP_MSS                     1460
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * TCP_SND_BUF) / TCP_MSS)

/* Checksums in software (the e1000 only handles the Ethernet CRC). */
#define LWIP_CHKSUM_ALGORITHM       2

/* Trim things we don't use. */
#define LWIP_STATS                  0
#define LWIP_NETIF_STATUS_CALLBACK  0
#define LWIP_NETIF_LINK_CALLBACK    0
#define LWIP_NETIF_HOSTNAME         0

/* Debugging off (we print our own progress). */
#define LWIP_DEBUG                  0

/* TLS via the altcp layer + mbedTLS port. Certificate verification is off
 * (VERIFY_NONE = 0): the kernel has no CA bundle or wall clock, so traffic is
 * encrypted but not authenticated. See README for the security caveat. */
#define LWIP_ALTCP                  1
#define LWIP_ALTCP_TLS              1
#define LWIP_ALTCP_TLS_MBEDTLS      1
#define ALTCP_MBEDTLS_AUTHMODE      0   /* MBEDTLS_SSL_VERIFY_NONE */

#endif /* LWIPOPTS_H */
