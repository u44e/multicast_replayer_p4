/* ── Ethernet manager (ESP32-P4 internal EMAC + RMII PHY) ── */
#ifndef ETH_MGR_H
#define ETH_MGR_H

#include <stdbool.h>
#include "esp_err.h"

/** Initialise NVS + netif + internal EMAC and start DHCP. */
esp_err_t eth_mgr_start(void);

/** True once the link is up and an IP address was obtained. */
bool eth_mgr_connected(void);

/** Current IPv4 address as text ("-" until connected). */
const char *eth_mgr_ip_str(void);

#endif /* ETH_MGR_H */
