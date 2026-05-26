#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "CryptnoxWallet.h"
#include "Pn532NfcTransport.h"
#include "ESP32Logger.h"
#include "esp32_crypto_provider.h"

/* Quiet CW_Logger — swallows the SDK's verbose connection/retry chatter that
 * was showing up as 'a a a...' on the UART. Keep ESP_LOGI for our own logs. */
class NullLogger : public CW_Logger {
public:
    bool begin(unsigned long) override { return true; }
    void print(const __FlashStringHelper*) override {}
    void print(const char*) override {}
    void print(char) override {}
    void print(uint8_t, int) override {}
    void print(uint16_t, int) override {}
    void print(uint32_t, int) override {}
    void print(int, int) override {}
    void println() override {}
    void println(const __FlashStringHelper*) override {}
    void println(const char*) override {}
    void println(char) override {}
    void println(uint8_t, int) override {}
    void println(uint16_t, int) override {}
    void println(uint32_t, int) override {}
    void println(int, int) override {}
};

extern "C" {
#include "pn532.h"
#include "keccak256.h"
#include "eth_rlp.h"
#include "eth_rpc.h"
#include "ui.h"
}

#include "config.h"

static const char *const TAG = "cryptnox_pos";

/* ── Hardware pin assignments — Cheap Yellow Display (ESP32) ───── */
/* PN532 on I²C, wired to the CYD CN1 connector. */
#define PN532_I2C_PORT      0
#define PN532_SDA           27
#define PN532_SCL           22
#define PN532_IRQ           (-1)
#define PN532_RST           (-1)
#define PN532_I2C_HZ        100000U

/* ── USDC ERC-20 transfer(address,uint256) selector ──────────── */
static const uint8_t TRANSFER_SELECTOR[4] = { 0xa9U, 0x05U, 0x9cU, 0xbbU };

/* ── Unsigned and signed tx buffers (EIP-1559 type 2) ─────────── */
#define TX_BUF_SIZE 300U

/******************************************************************
 * UI ↔ main task message queue
 ******************************************************************/

typedef struct {
    ui_event_t event;
    uint64_t   payload;
} ui_msg_t;

static QueueHandle_t s_ui_queue = NULL;

/* Set from the UI task when the user taps Cancel during PLACE_CARD; checked
 * by the main task after sign_and_broadcast returns so we skip the FAILED
 * flash and stay on the amount-entry screen. */
static volatile bool s_user_cancelled = false;

static void ui_event_dispatch(ui_event_t event, uint64_t payload) {
    if (event == UI_EVENT_CONFIRM_CANCEL) {
        s_user_cancelled = true;
    }
    ui_msg_t msg = { event, payload };
    (void)xQueueSend(s_ui_queue, &msg, 0);
}

/******************************************************************
 * Helpers
 ******************************************************************/

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

static void build_usdc_calldata(uint8_t out[68], const char *to_hex, uint64_t amount)
{
    (void)memset(out, 0, 68U);
    (void)memcpy(out, TRANSFER_SELECTOR, 4U);
    uint8_t addr[20];
    parse_address(to_hex, addr);
    (void)memcpy(out + 4U + 12U, addr, 20U);

    size_t j;
    for (j = 0U; j < 8U; j++) {
        out[67U - j] = (uint8_t)((amount >> (8U * j)) & 0xFFU);
    }
}

/******************************************************************
 * Sign + broadcast for a given amount
 *
 * Returns true on success and writes the tx hash (with 0x prefix) to
 * tx_hash_out.  On failure, writes a short error message to err_out.
 ******************************************************************/

static bool sign_and_broadcast(CryptnoxWallet &wallet, uint64_t amount_units,
                                char *tx_hash_out, size_t tx_hash_max,
                                char *err_out, size_t err_max)
{
    uint8_t card_pin[CW_MAX_PIN_LENGTH];
    (void)memset(card_pin, 0U, sizeof(card_pin));
    (void)memcpy(card_pin, CARD_PIN,
                 (CARD_PIN_LEN < CW_MAX_PIN_LENGTH) ? CARD_PIN_LEN : CW_MAX_PIN_LENGTH);

    uint8_t calldata[68];
    build_usdc_calldata(calldata, "0x" ADDR_TO, amount_units);

    uint64_t nonce = 0U;
    if (!eth_rpc_get_nonce(&nonce)) {
        (void)snprintf(err_out, err_max, "RPC: get nonce failed");
        return false;
    }

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

    uint8_t unsigned_tx[TX_BUF_SIZE];
    size_t  unsigned_len = eth_rlp_encode_unsigned(&tx, unsigned_tx, sizeof(unsigned_tx));
    if (unsigned_len == 0U) {
        (void)snprintf(err_out, err_max, "RLP encode overflow");
        return false;
    }

    uint8_t hash[CW_HASH_SIZE];
    keccak256(unsigned_tx, unsigned_len, hash);

    ui_show_tx_status(UI_TX_STATE_PLACE_CARD, "Hold card to reader");

    CW_SecureSession session;
    if (!wallet.connect(session)) {
        (void)snprintf(err_out, err_max, "Card not found");
        return false;
    }

    ui_show_tx_status(UI_TX_STATE_SIGNING, NULL);

    /* BIP32 Ethereum derivation path: m/44'/60'/0'/0/0 */
    static const uint8_t eth_path[20] = {
        0x80U, 0x00U, 0x00U, 0x2CU,
        0x80U, 0x00U, 0x00U, 0x3CU,
        0x80U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U,
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
        (void)snprintf(err_out, err_max, "Sign error 0x%02X",
                       static_cast<unsigned int>(result.errorCode));
        return false;
    }

    const uint8_t *sig_r = result.signature + CW_SIG_R_OFFSET;
    const uint8_t *sig_s = result.signature + CW_SIG_S_OFFSET;

    ui_show_tx_status(UI_TX_STATE_SENDING, NULL);

    uint8_t v = eth_rpc_ecrecover_parity(hash, sig_r, sig_s);

    uint8_t signed_tx[TX_BUF_SIZE];
    size_t  signed_len = eth_rlp_encode_signed(&tx, v, sig_r, sig_s,
                                               signed_tx, sizeof(signed_tx));
    if (signed_len == 0U) {
        (void)snprintf(err_out, err_max, "RLP signed overflow");
        return false;
    }

    if (!eth_rpc_send_raw_tx(signed_tx, signed_len, tx_hash_out, tx_hash_max)) {
        (void)snprintf(err_out, err_max, "Broadcast failed");
        return false;
    }

    return true;
}

/******************************************************************
 * Entry point
 ******************************************************************/

#define APP_VERSION_TAG "ui-r23 (drain queue + reset pending_amount after sign)"

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "===== cryptnox-pos boot =====");
    ESP_LOGI(TAG, "Build: %s %s", __DATE__, __TIME__);
    ESP_LOGI(TAG, "Tag:   %s", APP_VERSION_TAG);

    /* Silence the verbose per-APDU PN532 logs that flood UART during
     * wallet.connect retries (the "a a a"-looking chatter). */
    esp_log_level_set("pn532", ESP_LOG_WARN);
    esp_log_level_set("pn532_adapter", ESP_LOG_WARN);
    esp_log_level_set("Pn532NfcTransport", ESP_LOG_WARN);

    /* ── UI first: splash visible while the rest boots ───────── */
    s_ui_queue = xQueueCreate(8, sizeof(ui_msg_t));
    ui_init(ui_event_dispatch);
    ui_show_splash();

    /* ── NVS (required by WiFi driver) ────────────────────────── */
    esp_err_t nvs_ret = nvs_flash_init();
    if ((nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES) ||
        (nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);

    /* ── PN532 NFC reader ──────────────────────────────────────── */
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

    if (pn532_init(&nfc, &nfc_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "PN532 init failed");
        ui_show_tx_status(UI_TX_STATE_FAILED, "PN532 not found");
        return;
    }

    /* ── Wallet ────────────────────────────────────────────────── */
    NullLogger logger;
    (void)logger.begin(115200UL);
    ESP32CryptoProvider cryptoProvider;
    Pn532NfcTransport   nfcTransport(&nfc, logger);
    CryptnoxWallet      wallet(nfcTransport, logger, cryptoProvider);

    if (!wallet.begin()) {
        ESP_LOGE(TAG, "Wallet begin failed");
        ui_show_tx_status(UI_TX_STATE_FAILED, "Wallet init failed");
        return;
    }

    /* ── WiFi + RPC ────────────────────────────────────────────── */
    eth_rpc_init(RPC_URL, "0x" ADDR_FROM);
    if (!eth_rpc_wifi_connect(WIFI_SSID, WIFI_PASSWORD)) {
        ESP_LOGE(TAG, "WiFi connect failed");
        ui_show_tx_status(UI_TX_STATE_FAILED, "WiFi connect failed");
        return;
    }

    ESP_LOGI(TAG, "Ready");

    /* ── Main interaction loop ────────────────────────────────── */
    ui_show_amount_entry();

    uint64_t pending_amount = 0U;
    ui_msg_t msg;

    while (true) {
        if (xQueueReceive(s_ui_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (msg.event) {
            case UI_EVENT_AMOUNT_CONFIRMED:
                pending_amount = msg.payload;
                ui_show_confirm(pending_amount, "0x" ADDR_TO);
                break;

            case UI_EVENT_CONFIRM_CANCEL:
                ui_show_amount_entry();
                break;

            case UI_EVENT_CONFIRM_OK: {
                if (pending_amount == 0U) {
                    /* No fresh AMOUNT_CONFIRMED preceded this — likely a stale
                     * event from a stuck touch or queue replay. Drop it. */
                    ESP_LOGW(TAG, "stale CONFIRM_OK ignored");
                    break;
                }
                s_user_cancelled = false;
                char tx_hash[68] = { 0 };
                char err_msg[64] = { 0 };
                bool ok = sign_and_broadcast(wallet, pending_amount,
                                              tx_hash, sizeof(tx_hash),
                                              err_msg, sizeof(err_msg));
                pending_amount = 0U;  /* next sign requires fresh New Payment flow */
                /* Drain any UI events that were queued during the long sign
                 * (held touch, resistive screen noise, etc.). */
                {
                    ui_msg_t dump;
                    while (xQueueReceive(s_ui_queue, &dump, 0) == pdTRUE) { }
                }
                if (s_user_cancelled) {
                    /* Cancel during PLACE_CARD — UI already on amount entry. */
                } else if (ok) {
                    ui_show_tx_status(UI_TX_STATE_DONE, tx_hash);
                } else {
                    ui_show_tx_status(UI_TX_STATE_FAILED, err_msg);
                }
                break;
            }

            case UI_EVENT_TX_RETRY:
                ui_show_amount_entry();
                break;

            default:
                break;
        }
    }
}
