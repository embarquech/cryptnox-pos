idf_component_register(
    SRCS "main.cpp" "keccak256.cpp" "eth_addr.cpp" "eth_rlp.cpp"
         "eth_json.cpp" "eth_rpc.cpp" "net.cpp" "ui.cpp" "civil_time.cpp"
         "logo_img.c" "settings.cpp" "usdc_logo.c" "card_img.c" "logo_small.c"
    INCLUDE_DIRS "."
    REQUIRES pn532 driver esp_adc cryptnox_utils crypto_provider esp32_logger esp32_platform
             secure_channel pn532_nfc_transport
             esp_wifi esp_event esp_netif esp_http_client nvs_flash mbedtls
             json lwip
             arduino-esp32 TFT_eSPI XPT2046_Touchscreen lvgl
)

# LVGL 8 combines LV_PART_*/LV_STATE_* enums with '|', which C++20 flags as
# -Wdeprecated-enum-enum-conversion. Harmless and unavoidable — silence the noise.
# Scope to C++ only: the flag is invalid for the C image sources (cc1 warns).
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated-enum-enum-conversion>)
