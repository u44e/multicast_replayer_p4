/* ── Ethernet manager — ESP32-P4 internal EMAC, RMII PHY, DHCP ── */
#include "eth_mgr.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "eth";

static volatile bool s_connected;
static char s_ip_str[16] = "-";

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == ETH_EVENT && id == ETHERNET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "link up");
    } else if (base == ETH_EVENT && id == ETHERNET_EVENT_DISCONNECTED) {
        s_connected = false;
        strcpy(s_ip_str, "-");
        ESP_LOGW(TAG, "link down");
    } else if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&ev->ip_info.ip));
        s_connected = true;
        ESP_LOGI(TAG, "got ip: %s", s_ip_str);
    }
}

static esp_eth_phy_t *new_phy(const eth_phy_config_t *cfg)
{
#if CONFIG_MCR_ETH_PHY_IP101
    return esp_eth_phy_new_ip101(cfg);
#elif CONFIG_MCR_ETH_PHY_LAN87XX
    return esp_eth_phy_new_lan87xx(cfg);
#elif CONFIG_MCR_ETH_PHY_RTL8201
    return esp_eth_phy_new_rtl8201(cfg);
#elif CONFIG_MCR_ETH_PHY_DP83848
    return esp_eth_phy_new_dp83848(cfg);
#elif CONFIG_MCR_ETH_PHY_KSZ80XX
    return esp_eth_phy_new_ksz80xx(cfg);
#else
#error "no PHY selected"
#endif
}

esp_err_t eth_mgr_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs init");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");

    /* MAC: internal EMAC with the IDF P4 default RMII pin set
     * (MDC=31 MDIO=27 REF_CLK_IN=50, TX_EN=49 TXD0=34 TXD1=35
     *  CRS_DV=28 RXD0=29 RXD1=30) */
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);
    ESP_RETURN_ON_FALSE(mac, ESP_FAIL, TAG, "mac create");

    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = CONFIG_MCR_ETH_PHY_ADDR;
    phy_cfg.reset_gpio_num = CONFIG_MCR_ETH_PHY_RST_GPIO;
    esp_eth_phy_t *phy = new_phy(&phy_cfg);
    ESP_RETURN_ON_FALSE(phy, ESP_FAIL, TAG, "phy create");

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth = NULL;
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&eth_cfg, &eth), TAG, "driver");

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);
    ESP_RETURN_ON_ERROR(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)),
                        TAG, "attach");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                                   event_handler, NULL), TAG, "evt eth");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                                   event_handler, NULL), TAG, "evt ip");

    ESP_RETURN_ON_ERROR(esp_eth_start(eth), TAG, "start");
    ESP_LOGI(TAG, "ethernet started (DHCP)");
    return ESP_OK;
}

bool eth_mgr_connected(void)
{
    return s_connected;
}

const char *eth_mgr_ip_str(void)
{
    return s_ip_str;
}
