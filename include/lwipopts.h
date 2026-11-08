// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#ifndef LWIPOPTS_H
#define LWIPOPTS_H

/* lwIP configuration for the Open-K2 USB network link.
 * Bare metal: no OS, no sockets, polled from the main loop (sys_check_timeouts).
 * RAM is statically allocated; the pools below are what sets the .bss cost.
 */

#include <stdint.h>

#define NO_SYS                  1
#define LWIP_TIMERS             1
#define SYS_LIGHTWEIGHT_PROT    0
#define LWIP_NETCONN            0
#define LWIP_SOCKET             0
#define LWIP_NOASSERT           1

#define MEM_ALIGNMENT           4
#define MEM_SIZE                6144 /* tcp_write() copies of the reply in flight live here */

/* Protocols: IPv4 + ARP + ICMP echo + UDP (DHCP server) + TCP (the HTTP server on port 80). */
#define LWIP_IPV4               1
#define LWIP_IPV6               0
#define LWIP_ARP                1
#define LWIP_ETHERNET           1
#define LWIP_ICMP               1
#define LWIP_UDP                1
#define LWIP_TCP                1
#define LWIP_RAW                0
#define LWIP_DHCP               0 /* the K2 is the DHCP server (TinyUSB dhserver), not a client */
/* DHCP clients send from 0.0.0.0 to 255.255.255.255. Without this, ip4_input() drops the DISCOVER
 * (0.0.0.0 counts as a broadcast source) before UDP sees it. Accept link-layer-addressed UDP to
 * the DHCP server port (67) and allow the any-address source. */
#define LWIP_IP_ACCEPT_UDP_PORT(dst_port) ((dst_port) == PP_NTOHS(67))
#define LWIP_AUTOIP             0
#define LWIP_IGMP               0
#define LWIP_DNS                0
#define LWIP_STATS              0
#define IP_REASSEMBLY           0
#define IP_FRAG                 0
#define ARP_QUEUEING            0
#define ARP_TABLE_SIZE          4

/* Full-size Ethernet frames: the RNDIS host sends 1514-byte frames. */
#define TCP_MSS                 1460
/* Receive frames only: net.cpp copies each one into a pool pbuf and hands it to netif.input() synchronously, and the HTTP/UDP/ICMP
 * handlers free or consume it before returning (no reassembly, no out-of-order queue, no ARP queue). One is outstanding at a time;
 * three leave margin. A failed alloc is safe: recv_cb returns false and the driver re-arms the endpoint. */
#define PBUF_POOL_SIZE          3
#define MEMP_NUM_PBUF           8
#define MEMP_NUM_UDP_PCB        4

/* TCP: one listener and three connections (src/net/http_server.cpp), one reply in flight at a time.
 * A small window is enough for request lines; the send buffer is two segments. */
#define TCP_WND                 TCP_MSS
#define TCP_SND_BUF             (2 * TCP_MSS)
#define MEMP_NUM_TCP_PCB        4
#define MEMP_NUM_TCP_PCB_LISTEN 1
#define MEMP_NUM_TCP_SEG        12
#define TCP_QUEUE_OOSEQ         0
#define LWIP_TCP_SACK_OUT       0

/* PRNG for UDP ports and TCP sequence numbers, seeded from the SysTick clock. */
#ifdef __cplusplus
extern "C"
#endif
uint32_t net_rand(void); /* net.cpp */
#define LWIP_RAND()             ((u32_t)net_rand())

#endif /* LWIPOPTS_H */
