idf_component_register(
    SRCS "main.cpp" "keccak256.cpp" "eth_rlp.cpp" "eth_rpc.cpp"
    INCLUDE_DIRS "."
    REQUIRES pn532 driver cryptnox_utils crypto_provider esp32_logger
             secure_channel pn532_nfc_transport
             esp_wifi esp_event esp_netif esp_http_client nvs_flash mbedtls
)
