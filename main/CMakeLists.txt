idf_component_register(
    SRCS "hardening.cpp" "main.cpp" "keccak256.cpp" "eth_addr.cpp" "eth_rlp.cpp"
         "eth_json.cpp" "eth_rpc.cpp" "https_post.cpp" "net.cpp" "ui.cpp"
         "civil_time.cpp" "tron_tx.cpp" "tron_rpc.cpp"
         "ota.cpp" "provision.cpp" "settings.cpp"
         "logo_img.c" "card_img.c" "logo_small.c" "chain_icons.c"
    INCLUDE_DIRS "."
    REQUIRES pn532 driver cryptnox_utils crypto_provider esp32_logger esp32_platform
             secure_channel pn532_nfc_transport
             esp_wifi esp_event esp_netif esp_http_client esp_http_server
             app_update esp_timer
             nvs_flash mbedtls
             json lwip
             arduino-esp32 TFT_eSPI XPT2046_Touchscreen lvgl
)

# LVGL 8 combines LV_PART_*/LV_STATE_* enums with '|', which C++20 flags as
# -Wdeprecated-enum-enum-conversion. Harmless and unavoidable — silence the noise.
# Scope to C++ only: the flag is invalid for the C image sources (cc1 warns).
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated-enum-enum-conversion>)
