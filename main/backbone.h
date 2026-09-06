/*
 * The border router's backbone link, whichever transport carries it.
 *
 * Three implementations select on CONFIG_THBR_TRANSPORT_* (main/CMakeLists.txt
 * compiles exactly one):
 *
 *   ppp_link.c  PPP over USB-Serial/JTAG.  Carries IP perfectly well, but the
 *               prebuilt libopenthread_br.a never initialises its
 *               infrastructure-interface side for a point-to-point netif —
 *               measured 2026-08-20, see PLAN.md.  Kept because it is the
 *               proven transport for plain data (NCP over TCP) and because it
 *               is the control case for the finding.
 *
 *   eth_link.c  Ethernet frames over the same serial port, SLIP-framed, with
 *               a TAP device on the host (tools/tap_pump.py).  A real L2 netif
 *               with a MAC and a broadcast domain, which is what the border
 *               router expects — and what HAOS can run, since it ships
 *               CONFIG_TUN=y but no kernel PPP on x86_64.
 *
 *   w5500_link.c  Physical SPI Ethernet on the ESP32-C6-Bug Eth-Bug-PoE
 *                 shield. DHCP and IPv6 run directly on the ESP, while USB
 *                 remains the diagnostic console.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bring the backbone up.  Non-blocking; the netif exists on return. */
esp_err_t backbone_start(void);

/* True once the link carries IP. */
bool backbone_is_up(void);

/* Block until the link is up. ESP_ERR_TIMEOUT on expiry. */
esp_err_t backbone_wait_up(uint32_t timeout_ms);

/* The netif to hand to esp_openthread_set_backbone_netif(). */
esp_netif_t *backbone_netif(void);

/* Fixed host peer for point-to-point links; zero on a switched LAN. */
uint32_t backbone_peer_ipv4(void);

/* Announce the backbone's address as an IP_EVENT_ETH_GOT_IP.
 *
 * The physical netif posts this after DHCP, but late consumers may not yet be
 * registered. The USB/TAP address is static and never posts it. Call this
 * after esp_br_web_start() so the REST server sees the current address. */
esp_err_t backbone_announce_got_ip(void);

#ifdef __cplusplus
}
#endif
