#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "CryptnoxWallet.h"
#include "Pn532NfcTransport.h"
#include "ESP32Logger.h"
#include "esp32_crypto_provider.h"

extern "C" {
#include "pn532.h"
#include "keccak256.h"
#include "eth_rlp.h"
#include "eth_rpc.h"
}

#include "config.h"

static const char *const TAG = "usdc_signing";

/* ── Hardware pin assignments — Cheap Yellow Display (ESP32) ───── */
/* PN532 on I²C, wired to the CYD CN1 connector. */
#define PN532_I2C_PORT      0          /* I2C_NUM_0 */
#define PN532_SDA           27         /* CN1 SDA */
#define PN532_SCL           22         /* CN1 SCL */
#define PN532_IRQ           (-1)       /* unused */
#define PN532_RST           (-1)       /* unused — drain in pn532_init handles soft-reboot recovery */
#define PN532_I2C_HZ        100000U

/* ── USDC ERC-20 transfer(address,uint256) selector ──────────── */
static const uint8_t TRANSFER_SELECTOR[4] = { 0xa9U, 0x05U, 0x9cU, 0xbbU };

/* ── Unsigned and signed tx buffers (EIP-1559 type 2) ─────────── */
#define TX_BUF_SIZE 300U

/******************************************************************
 * Helpers
 ******************************************************************/

/*
 * Parse a "0x..."-prefixed (or unprefixed) hex string of exactly 20 bytes
 * into 'out'.  Silently truncates/pads if the source length differs.
 */
static void parse_address(const char *hex, uint8_t out[20])
{
    const char *p = hex;
    if ((p[0] == '0') && ((p[1] == 'x') || (p[1] == 'X'))) {
        p += 2;
    }
    (void)memset(out, 0, 20U);
    size_t i;
    for (i = 0U; (i < 20U) && (p[0] != '\0') && (p[1] != '\0'); i++) {
        uint8_t hi = (uint8_t)((*p >= 'a') ? (*p - 'a' + 10) :
                               (*p >= 'A') ? (*p - 'A' + 10) : (*p - '0'));
        p++;
        uint8_t lo = (uint8_t)((*p >= 'a') ? (*p - 'a' + 10) :
                               (*p >= 'A') ? (*p - 'A' + 10) : (*p - '0'));
        p++;
        out[i] = (uint8_t)((hi << 4U) | lo);
    }
}

/*
 * Build the 68-byte calldata for USDC transfer(to, amount).
 * Layout: selector(4) | zeroes(12) | to(20) | zeroes(24) | amount_be(8)
 */
static void build_usdc_calldata(uint8_t out[68], const char *to_hex, uint64_t amount)
{
    (void)memset(out, 0, 68U);

    /* Function selector */
    (void)memcpy(out, TRANSFER_SELECTOR, 4U);

    /* 'to' address: right-aligned in 32-byte slot starting at offset 4 */
    uint8_t addr[20];
    parse_address(to_hex, addr);
    (void)memcpy(out + 4U + 12U, addr, 20U);

    /* amount: right-aligned uint256, 64-bit value fits in the last 8 bytes */
    size_t j;
    for (j = 0U; j < 8U; j++) {
        out[67U - j] = (uint8_t)((amount >> (8U * j)) & 0xFFU);
    }
}

/******************************************************************
 * Signing loop
 ******************************************************************/

static void signing_loop(CryptnoxWallet &wallet)
{
    /* CARD_PIN is a string literal ("000000000"); copy into the pin array. */
    uint8_t card_pin[CW_MAX_PIN_LENGTH];
    (void)memset(card_pin, 0U, sizeof(card_pin));
    (void)memcpy(card_pin, CARD_PIN,
                 (CARD_PIN_LEN < CW_MAX_PIN_LENGTH) ? CARD_PIN_LEN : CW_MAX_PIN_LENGTH);

    uint8_t calldata[68];
    build_usdc_calldata(calldata, "0x" ADDR_TO, AMOUNT_USDC);

    while (true) {
        /* ── 1. Get fresh nonce ────────────────────────────────── */
        uint64_t nonce = 0U;
        if (!eth_rpc_get_nonce(&nonce)) {
            ESP_LOGE(TAG, "Failed to get nonce — retrying in 5 s");
            vTaskDelay(pdMS_TO_TICKS(5000U));
            continue;
        }

        /* ── 2. Build tx ───────────────────────────────────────── */
        eth_tx_t tx;
        (void)memset(&tx, 0, sizeof(tx));
        tx.chain_id          = CHAIN_ID_SEPOLIA;
        tx.nonce             = nonce;
        tx.max_priority_fee  = MAX_PRIORITY_FEE;
        tx.max_fee           = MAX_FEE;
        tx.gas_limit         = GAS_LIMIT_ERC20;
        tx.eth_value         = 0U;
        tx.calldata          = calldata;
        tx.calldata_len      = sizeof(calldata);
        parse_address("0x" ADDR_USDC, tx.to);

        /* ── 3. Encode unsigned tx and hash it ─────────────────── */
        uint8_t unsigned_tx[TX_BUF_SIZE];
        size_t  unsigned_len = eth_rlp_encode_unsigned(&tx, unsigned_tx, sizeof(unsigned_tx));
        if (unsigned_len == 0U) {
            ESP_LOGE(TAG, "RLP encode unsigned overflow");
            vTaskDelay(pdMS_TO_TICKS(2000U));
            continue;
        }

        uint8_t hash[CW_HASH_SIZE];
        keccak256(unsigned_tx, unsigned_len, hash);

        ESP_LOGI(TAG, "Hash to sign:");
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, hash, CW_HASH_SIZE, ESP_LOG_INFO);

        /* ── 4. Wait for card and sign ─────────────────────────── */
        ESP_LOGI(TAG, "Hold Cryptnox card to reader to sign...");

        CW_SecureSession session;
        bool connected = wallet.connect(session);
        if (!connected) {
            vTaskDelay(pdMS_TO_TICKS(500U));
            continue;
        }

        /* BIP32 Ethereum derivation path: m/44'/60'/0'/0/0
         * Each level is a 4-byte big-endian uint32; hardened levels have the
         * high bit set. */
        static const uint8_t eth_path[20] = {
            0x80U, 0x00U, 0x00U, 0x2CU,   /* 44' */
            0x80U, 0x00U, 0x00U, 0x3CU,   /* 60' */
            0x80U, 0x00U, 0x00U, 0x00U,   /* 0'  */
            0x00U, 0x00U, 0x00U, 0x00U,   /* 0   */
            0x00U, 0x00U, 0x00U, 0x00U,   /* 0   */
        };

        CW_SignRequest req(session,
                          CW_SIGN_DERIVE_K1,
                          CW_SIGN_SIG_ECDSA_LOW_S,
                          CW_SIGN_WITH_PIN);
        req.hash             = hash;
        req.hashLength       = static_cast<uint8_t>(CW_HASH_SIZE);
        req.derivePath       = eth_path;
        req.derivePathLength = static_cast<uint8_t>(sizeof(eth_path));
        (void)memcpy(req.pin, card_pin, CW_MAX_PIN_LENGTH);

        CW_SignResult result = wallet.sign(req);
        wallet.disconnect(session);

        if (result.errorCode != CW_OK) {
            ESP_LOGE(TAG, "Sign failed: 0x%02X",
                     static_cast<unsigned int>(result.errorCode));
            vTaskDelay(pdMS_TO_TICKS(2000U));
            continue;
        }

        const uint8_t *sig_r = result.signature + CW_SIG_R_OFFSET;
        const uint8_t *sig_s = result.signature + CW_SIG_S_OFFSET;

        ESP_LOGI(TAG, "r:");
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, sig_r, CW_HASH_SIZE, ESP_LOG_INFO);
        ESP_LOGI(TAG, "s:");
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, sig_s, CW_HASH_SIZE, ESP_LOG_INFO);

        /* ── 5. Determine v parity via ecrecover ───────────────── */
        uint8_t v = eth_rpc_ecrecover_parity(hash, sig_r, sig_s);
        ESP_LOGI(TAG, "v = %u", static_cast<unsigned int>(v));

        /* ── 6. Encode signed tx ───────────────────────────────── */
        uint8_t signed_tx[TX_BUF_SIZE];
        size_t  signed_len = eth_rlp_encode_signed(&tx, v, sig_r, sig_s,
                                                   signed_tx, sizeof(signed_tx));
        if (signed_len == 0U) {
            ESP_LOGE(TAG, "RLP encode signed overflow");
            vTaskDelay(pdMS_TO_TICKS(2000U));
            continue;
        }

        ESP_LOGI(TAG, "Signed tx (%u bytes):", (unsigned int)signed_len);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, signed_tx, signed_len, ESP_LOG_INFO);

        /* ── 7. Broadcast ──────────────────────────────────────── */
        char tx_hash[68] = { 0 };
        if (eth_rpc_send_raw_tx(signed_tx, signed_len,
                                tx_hash, sizeof(tx_hash))) {
            ESP_LOGI(TAG, "TX broadcast OK: %s", tx_hash);
        } else {
            ESP_LOGE(TAG, "TX broadcast failed");
        }

        /* Wait before next iteration so nonce advances on-chain. */
        vTaskDelay(pdMS_TO_TICKS(15000U));
    }
}

/******************************************************************
 * Entry point
 ******************************************************************/

extern "C" void app_main(void)
{
    /* ── NVS (required by WiFi driver) ────────────────────────── */
    esp_err_t nvs_ret = nvs_flash_init();
    if ((nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES) ||
        (nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);

    /* ── PN532 NFC reader (I²C on the CYD CN1 connector) ──────── */
    pn532_t nfc;
    (void)memset(&nfc, 0, sizeof(nfc));

    pn532_config_t nfc_cfg;
    (void)memset(&nfc_cfg, 0, sizeof(nfc_cfg));
    nfc_cfg.transport     = PN532_TRANSPORT_I2C;
    nfc_cfg.i2c_port      = PN532_I2C_PORT;
    nfc_cfg.pin_sda       = PN532_SDA;
    nfc_cfg.pin_scl       = PN532_SCL;
    nfc_cfg.pin_irq       = PN532_IRQ;
    nfc_cfg.pin_rst       = PN532_RST;
    nfc_cfg.i2c_clock_hz  = PN532_I2C_HZ;

    esp_err_t nfc_ret = pn532_init(&nfc, &nfc_cfg);
    if (nfc_ret != ESP_OK) {
        ESP_LOGE(TAG, "PN532 init failed");
        return;
    }

    /* ── Wallet setup ──────────────────────────────────────────── */
    ESP32Logger logger;
    (void)logger.begin(115200UL);

    ESP32CryptoProvider cryptoProvider;
    Pn532NfcTransport   nfcTransport(&nfc, logger);
    CryptnoxWallet      wallet(nfcTransport, logger, cryptoProvider);

    if (!wallet.begin()) {
        ESP_LOGE(TAG, "Wallet begin (SAMConfig) failed");
        return;
    }
    /* wallet.begin() already prints the PN532 firmware version internally. */

    /* ── WiFi + RPC ────────────────────────────────────────────── */
    eth_rpc_init(RPC_URL, "0x" ADDR_FROM);

    if (!eth_rpc_wifi_connect(WIFI_SSID, WIFI_PASSWORD)) {
        ESP_LOGE(TAG, "WiFi connect failed — check config.h credentials");
        return;
    }

    ESP_LOGI(TAG, "Ready — will sign USDC transfer each card tap");

    signing_loop(wallet);
}
