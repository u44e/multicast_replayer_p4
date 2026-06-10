idf_component_register(
    SRCS "main.c" "eth_mgr.c"
         "pcap_reader.c" "stream_mgr.c" "net_tx.c" "replay.c"
    INCLUDE_DIRS "."
    REQUIRES esp_eth esp_netif nvs_flash esp_timer driver fatfs sdmmc console
)
