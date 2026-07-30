/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file main.cpp
 * @ingroup app
 * @brief cryptnox-pos entry point: touchscreen USDC payment terminal.
 *
 * Drives the amount → confirm → sign → broadcast flow on the Cheap Yellow
 * Display, signing EIP-1559 USDC transfers on a Cryptnox card via PN532.
 */

/******************************************************************
 * 1. Included files
 ******************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "CryptnoxWallet.h"
#include "CW_Utils.h"
#include "Pn532NfcTransport.h"
#include "ESP32Logger.h"
#include "ESP32Platform.h"
#include "esp32_crypto_provider.h"
#include "settings.h"

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
#include "eth_addr.h"
#include "eth_rlp.h"
#include "eth_rpc.h"
#include "net.h"
#include "ui.h"
}

#include "config.h"

/******************************************************************
 * 2. Configuration guards and constants
 ******************************************************************/

/* The card PIN is entered by the operator on the touchscreen keypad at sign
 * time (see ui PIN screen) — it is no longer baked into config.h, so the old
 * demo-PIN build guard is gone. */

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
 * 3. UI ↔ main task message queue
 ******************************************************************/

typedef struct {
    ui_event_t event;
    uint64_t   payload;
} ui_msg_t;

static QueueHandle_t s_ui_queue = NULL;

/* Set from the UI task when the user taps Cancel during PLACE_CARD; checked
 * by the main task after sign_and_broadcast returns so we skip the FAILED
 * flash and stay on the amount-entry screen.  std::atomic (seq_cst) rather
 * than volatile so the cross-task access is well-defined per the C++ memory
 * model. */
static std::atomic<bool> s_user_cancelled{false};

/**
 * @brief UI-task callback: forward a touch event to the main task queue.
 *
 * Runs in the UI task context.  A Cancel tap also raises the atomic
 * @ref s_user_cancelled flag so the in-flight signing flow can abort
 * without waiting for the queue to drain.
 *
 * @param[in] event   UI event identifier.
 * @param[in] payload Event payload (amount in USDC base units, or 0).
 */
static void ui_event_dispatch(ui_event_t event, uint64_t payload) {
    if (event == UI_EVENT_CONFIRM_CANCEL) {
        s_user_cancelled = true;
    }
    ui_msg_t msg = { event, payload };
    (void)xQueueSend(s_ui_queue, &msg, 0);
}

/******************************************************************
 * 4. Helpers
 ******************************************************************/

/**
 * @brief Build the 68-byte ABI-encoded calldata for a USDC @c transfer call.
 *
 * Encodes the ERC-20 @c transfer(address,uint256) selector followed by the
 * ABI-encoded arguments:
 * @code
 * selector(4) | zeroes(12) | to(20) | zeroes(24) | amount_be(8)
 * @endcode
 *
 * @param[out] out    68-byte output buffer; contents are undefined on failure.
 * @param[in]  to_hex Recipient address as hex (with or without @c 0x prefix).
 * @param[in]  amount Transfer amount in USDC base units (6 decimals).
 * @return true on success, false if @p to_hex fails address validation.
 */
static bool build_usdc_calldata(uint8_t out[68], const char *to_hex, uint64_t amount)
{
    (void)memset(out, 0, 68U);
    (void)CW_Utils::safe_memcpy(out, 68U, TRANSFER_SELECTOR, 4U);
    uint8_t addr[20];
    if (!eth_addr_parse(to_hex, addr)) {
        return false;
    }
    (void)CW_Utils::safe_memcpy(out + 4U + 12U, 68U - (4U + 12U), addr, 20U);

    size_t j;
    for (j = 0U; j < 8U; j++) {
        out[67U - j] = static_cast<uint8_t>((amount >> (8U * j)) & 0xFFU);
    }
    return true;
}

/******************************************************************
 * 5. Sign + broadcast for a given amount
 ******************************************************************/

/**
 * @brief Scrubs a buffer with CW_Utils::secure_wipe when it leaves scope.
 *
 * sign_and_broadcast has many early-return paths; a guard declared next to
 * each sensitive artifact scrubs it on every exit (present or future) the way
 * the SDK examples wipe the hash and signature after use, without needing a
 * secure_wipe before each individual return.
 */
namespace {
struct WipeGuard {
    uint8_t *buf;
    size_t   len;
    explicit WipeGuard(uint8_t *b, size_t n) : buf(b), len(n) {}
    ~WipeGuard() { CW_Utils::secure_wipe(buf, len); }
    WipeGuard(const WipeGuard &) = delete;
    WipeGuard &operator=(const WipeGuard &) = delete;
};
}  // namespace

/**
 * @brief Sign a USDC transfer on the card and broadcast it via JSON-RPC.
 *
 * Full pipeline: build calldata → fetch nonce → RLP-encode + keccak256 →
 * card connect/sign (cancellable) → ecrecover parity → broadcast.  The card
 * PIN is copied into the sign request at the last moment and scrubbed with
 * @c CW_Utils::secure_wipe immediately after signing; the message hash,
 * signature and encoded transactions are scrubbed on every exit path via
 * @c WipeGuard.
 *
 * @param[in]  wallet       Initialised wallet instance.
 * @param[in]  transport    PN532 transport, used for the cancellable
 *                          connect loop.
 * @param[in]  amount_units Transfer amount in USDC base units (6 decimals).
 * @param[in]  pin          Operator-entered card PIN (scrubbed after signing).
 * @param[in]  pin_chars    Number of PIN characters in @p pin.
 * @param[out] tx_hash_out  "0x..."-prefixed tx hash on success.
 * @param[in]  tx_hash_max  Capacity of @p tx_hash_out (>= 68 bytes).
 * @param[out] err_out      Short UI-facing error message on failure.
 * @param[in]  err_max      Capacity of @p err_out.
 * @return true on successful broadcast; false on failure or user cancel
 *         (err_out is only meaningful when @ref s_user_cancelled is clear).
 */
static bool sign_and_broadcast(CryptnoxWallet &wallet,
                                Pn532NfcTransport &transport,
                                uint64_t amount_units,
                                const char *pin, size_t pin_chars,
                                char *tx_hash_out, size_t tx_hash_max,
                                char *err_out, size_t err_max)
{
    uint8_t calldata[68];
    if (!build_usdc_calldata(calldata, "0x" ADDR_TO, amount_units)) {
        (void)snprintf(err_out, err_max, "Bad ADDR_TO in config");
        return false;
    }

    uint64_t nonce = 0U;
    if (!eth_rpc_get_nonce(&nonce)) {
        (void)snprintf(err_out, err_max, "RPC: get nonce failed");
        return false;
    }

    eth_tx_t tx;
    (void)memset(&tx, 0, sizeof(tx));
    tx.chain_id          = CHAIN_ID_SEPOLIA;
    tx.nonce             = nonce;
    /* Fees come from the settings menu (defaulting to the config.h values on
     * first boot); config.h still owns the gas limit. The user edits Gwei, so
     * scale to wei. Keep the tip <= the cap or the tx is malformed. */
    uint64_t max_fee_wei  = (uint64_t)settings_get_max_fee_gwei()      * 1000000000ULL;
    uint64_t prio_fee_wei = (uint64_t)settings_get_priority_fee_gwei() * 1000000000ULL;
    if (prio_fee_wei > max_fee_wei) { prio_fee_wei = max_fee_wei; }
    tx.max_priority_fee  = prio_fee_wei;
    tx.max_fee           = max_fee_wei;
    tx.gas_limit         = GAS_LIMIT_ERC20;
    tx.eth_value         = 0U;
    tx.calldata          = calldata;
    tx.calldata_len      = sizeof(calldata);
    if (!eth_addr_parse("0x" ADDR_USDC, tx.to)) {
        (void)snprintf(err_out, err_max, "Bad ADDR_USDC in config");
        return false;
    }

    uint8_t unsigned_tx[TX_BUF_SIZE];
    size_t  unsigned_len = eth_rlp_encode_unsigned(&tx, unsigned_tx, sizeof(unsigned_tx));
    if (unsigned_len == 0U) {
        (void)snprintf(err_out, err_max, "RLP encode overflow");
        return false;
    }

    uint8_t hash[CW_HASH_SIZE];
    keccak256(unsigned_tx, unsigned_len, hash);

    /* From here the message hash and (soon) the signature live on the stack.
     * Scrub them and the encoded transactions on every exit path below. */
    WipeGuard g_unsigned(unsigned_tx, sizeof(unsigned_tx));
    WipeGuard g_hash(hash, sizeof(hash));

    if (!s_user_cancelled) {
        ui_show_tx_status(UI_TX_STATE_PLACE_CARD, "Hold card to reader");
    }

    /* Start from a clean reader state: a previous attempt that ended with the
     * card ripped away mid-exchange can leave a stale target selected, which
     * would make every InListPassiveTarget below time out. */
    transport.resetReader();

    /* Manual connect loop with cancel checks between PN532 polls — replaces
     * wallet.connect() so a Cancel from the user aborts within one PN532
     * timeout. Give the user up to 60 s to present the card. */
    const int64_t card_wait_us = 60LL * 1000000LL;   /* 60 seconds */
    const int64_t start_us     = esp_timer_get_time();
    CW_SecureSession session;
    bool connected = false;
    while (!connected) {
        if (s_user_cancelled) {
            return false;
        }
        if ((esp_timer_get_time() - start_us) > card_wait_us) {
            break;   /* timed out waiting for the card */
        }
        if (transport.inListPassiveTarget()) {
            /* Card tapped — show immediate feedback while the secure channel
             * comes up. */
            ui_show_tx_status(UI_TX_STATE_PROCESSING, NULL);
            vTaskDelay(pdMS_TO_TICKS(200));
            if (wallet.establishSecureChannel(session)) {
                connected = true;
                break;
            }
            /* Card pulled away (or channel failed) — the PN532 still holds the
             * now-dead target selected, which makes every following
             * InListPassiveTarget time out. Release it so the reader can see
             * the card again, then back to the tap prompt. */
            transport.resetReader();
            if (!s_user_cancelled) {
                ui_show_tx_status(UI_TX_STATE_PLACE_CARD, "Hold card to reader");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (s_user_cancelled) {
        return false;
    }
    if (!connected) {
        (void)snprintf(err_out, err_max, "Card not found");
        return false;
    }

    if (!s_user_cancelled) {
        ui_show_tx_status(UI_TX_STATE_SIGNING, NULL);
    }

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

    /* Copy the operator-entered PIN into the request as late as possible;
     * req.pin is zero-initialised by the CW_SignRequest constructor. */
    const size_t copy_len = (pin_chars < CW_MAX_PIN_LENGTH) ? pin_chars
                                                            : CW_MAX_PIN_LENGTH;
    (void)CW_Utils::safe_memcpy(req.pin, sizeof(req.pin),
                                reinterpret_cast<const uint8_t *>(pin),
                                copy_len);

    CW_SignResult result = wallet.sign(req);
    WipeGuard g_sig(result.signature, sizeof(result.signature));
    wallet.disconnect(session);

    /* scrub the PIN immediately after use (secure_wipe is not
     * dead-store-eliminated); ~CW_SignRequest wipes it again as a backstop. */
    CW_Utils::secure_wipe(req.pin, sizeof(req.pin));

    if (result.errorCode != CW_OK) {
        (void)snprintf(err_out, err_max, "Sign error 0x%02X",
                       static_cast<unsigned int>(result.errorCode));
        return false;
    }

    const uint8_t *sig_r = result.signature + CW_SIG_R_OFFSET;
    const uint8_t *sig_s = result.signature + CW_SIG_S_OFFSET;

    if (!s_user_cancelled) {
        ui_show_tx_status(UI_TX_STATE_SENDING, NULL);
    }

    /* the parity recovery now fails explicitly instead of silently
     * defaulting to v=0, so the operator sees the actual root cause. */
    uint8_t v = 0U;
    switch (eth_rpc_ecrecover_parity(hash, sig_r, sig_s, &v)) {
        case ETH_RPC_PARITY_OK:
            break;
        case ETH_RPC_PARITY_MISMATCH:
            (void)snprintf(err_out, err_max, "Card-address mismatch");
            return false;
        case ETH_RPC_PARITY_RPC_ERROR:
        default:
            (void)snprintf(err_out, err_max, "RPC error (parity)");
            return false;
    }

    uint8_t signed_tx[TX_BUF_SIZE];
    WipeGuard g_signed(signed_tx, sizeof(signed_tx));
    size_t  signed_len = eth_rlp_encode_signed(&tx, v, sig_r, sig_s,
                                               signed_tx, sizeof(signed_tx));
    if (signed_len == 0U) {
        (void)snprintf(err_out, err_max, "RLP signed overflow");
        return false;
    }

    /* last cancel check right before the irreversible broadcast. */
    if (s_user_cancelled) {
        return false;
    }

    if (!eth_rpc_send_raw_tx(signed_tx, signed_len, tx_hash_out, tx_hash_max)) {
        (void)snprintf(err_out, err_max, "Broadcast failed");
        return false;
    }

    return true;
}

/******************************************************************
 * 6. Entry point
 ******************************************************************/

#define APP_VERSION_TAG "ui-r27 (cancellable connect loop)"

/**
 * @brief ESP-IDF application entry point.
 *
 * Brings up the UI (splash), NVS, PN532 reader, wallet, WiFi, and a
 * blocking SNTP time sync, then services UI events in the main
 * interaction loop (amount → confirm → sign+broadcast).
 */
/**
 * @brief Bring up Wi-Fi: connect with saved credentials, or run the first-run
 *        network picker (scan → list → keyboard → connect) until connected.
 *
 * Blocks until a connection succeeds; the operator cannot leave setup without
 * one. config.h Wi-Fi credentials are intentionally not used (NVS only).
 */
static void ensure_wifi(void)
{
    char ssid[33] = { 0 };
    char pass[65] = { 0 };
    if (settings_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass))) {
        bool ok = net_wifi_connect(ssid, pass);
        CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(pass), sizeof(pass));
        if (ok) { return; }
    }

    /* No usable saved network — forced interactive setup. */
    net_wifi_ap_t aps[16];
    uint16_t n = net_wifi_scan(aps, 16);
    ui_show_wifi_list(aps, n);

    ui_msg_t msg;
    while (true) {
        if (xQueueReceive(s_ui_queue, &msg, portMAX_DELAY) != pdTRUE) { continue; }

        if (msg.event == UI_EVENT_WIFI_TRY) {
            char w_ssid[33] = { 0 };
            char w_pass[65] = { 0 };
            bool ok = false;
            if (ui_take_wifi_creds(w_ssid, sizeof(w_ssid),
                                   w_pass, sizeof(w_pass)) > 0U) {
                ui_show_wifi_connecting(w_ssid);
                ok = net_wifi_connect(w_ssid, w_pass);
                if (ok) { settings_set_wifi(w_ssid, w_pass); }
            }
            CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(w_pass), sizeof(w_pass));
            if (ok) { return; }
        }

        /* WIFI_SCAN, a failed connect, or any stray event: re-scan and show
         * the list again so the user stays in setup until connected. */
        n = net_wifi_scan(aps, 16);
        ui_show_wifi_list(aps, n);
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "===== cryptnox-pos boot =====");
#ifdef CRYPTNOX_POS_DEV_BUILD
    /* build timestamp helps firmware fingerprinting — dev builds only. */
    ESP_LOGI(TAG, "Build: %s %s", __DATE__, __TIME__);
#endif
    ESP_LOGI(TAG, "Tag:   %s", APP_VERSION_TAG);

    /* The SDK's per-APDU PN532 log flood was trimmed upstream (SDK main);
     * INFO now only emits a few init lines worth keeping ("I2C wake-up
     * trigger", "PN532 initialized"). Keep the adapters at WARN — their
     * per-APDU chatter is still verbose. */
    esp_log_level_set("pn532", ESP_LOG_INFO);
    esp_log_level_set("pn532_adapter", ESP_LOG_WARN);
    esp_log_level_set("Pn532NfcTransport", ESP_LOG_WARN);

    /* PN532 I2C busy-polling: the chip NACKs its own address while busy —
     * that is the normal PN532 I2C protocol, but IDF's i2c.master driver
     * logs a 4-line error burst for every poll. Mute the tag; real I2C
     * failures still propagate through the SDK's return codes. */
    esp_log_level_set("i2c.master", ESP_LOG_NONE);

    /* ── NVS first: required by the WiFi driver AND by the UI task, which
     * reads the saved backlight level / Wi-Fi credentials at startup. ── */
    esp_err_t nvs_ret = nvs_flash_init();
    if ((nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES) ||
        (nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);

    /* ── UI: splash visible while the rest boots ───────── */
    s_ui_queue = xQueueCreate(8, sizeof(ui_msg_t));
    ui_init(ui_event_dispatch);
    ui_set_addresses("0x" ADDR_USDC, "0x" ADDR_TO);
    ui_show_splash();

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
    ESP32Platform       platform;
    CryptnoxWallet      wallet(nfcTransport, logger, cryptoProvider, platform);

    if (!wallet.begin()) {
        ESP_LOGE(TAG, "Wallet begin failed");
        ui_show_tx_status(UI_TX_STATE_FAILED, "Wallet init failed");
        return;
    }

    /* ── WiFi + RPC ────────────────────────────────────────────── */
    eth_rpc_init(RPC_URL, "0x" ADDR_FROM);
#if defined(RPC_PROJECT_ID) && defined(RPC_API_SECRET)
    eth_rpc_set_auth(RPC_PROJECT_ID, RPC_API_SECRET);
#endif
#ifdef RPC_CA_CERT_PEM
    /* pin the RPC endpoint's certificate instead of the CA bundle. */
    eth_rpc_set_ca_cert(RPC_CA_CERT_PEM);
#endif
    net_wifi_init();
    /* Connect with saved creds, or run the first-run picker until connected
     * (config.h Wi-Fi is no longer used). */
    ensure_wifi();

    /* block on a first SNTP sync so TLS certificate validity-period
     * checks run against real time instead of the 1970 epoch. Retry a couple
     * of rounds — flaky uplinks (phone hotspots) often need a second try. */
    bool time_ok = false;
    for (int attempt = 0; (attempt < 3) && !time_ok; attempt++) {
        time_ok = net_time_sync(15000U);
    }
    if (!time_ok) {
        ESP_LOGE(TAG, "SNTP time sync failed");
        ui_show_tx_status(UI_TX_STATE_FAILED, "Time sync failed - check network");
        return;
    }

    /* One RPC round-trip at boot, for two reasons at once: it proves the
     * endpoint is reachable, and it is the first request whose authenticated
     * Date header can contradict the unauthenticated SNTP clock we just set.
     * Without it a clock wrong in the *forward* direction — the direction that
     * carries a certificate past its notAfter — would sail through boot and
     * only surface mid-payment, with a customer waiting.
     *
     * Retried like the sync above, since a single failure is more often a
     * flaky uplink than a hostile one. The payment path re-checks every
     * request regardless, so this is early warning, not the enforcement. */
    bool rpc_ok = false;
    for (int attempt = 0; (attempt < 3) && !rpc_ok; attempt++) {
        uint64_t boot_nonce = 0U;
        rpc_ok = eth_rpc_get_nonce(&boot_nonce);
    }
    if (!rpc_ok) {
        ESP_LOGE(TAG, "RPC unreachable or clock rejected at boot");
        ui_show_tx_status(UI_TX_STATE_FAILED, "RPC/clock check failed - see log");
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

            case UI_EVENT_PIN_ENTERED: {
                if (pending_amount == 0U) {
                    /* No fresh AMOUNT_CONFIRMED preceded this — likely a stale
                     * event from a stuck touch or queue replay. Drop it. */
                    ESP_LOGW(TAG, "stale PIN_ENTERED ignored");
                    break;
                }
                /* Fetch the keypad PIN (UI wipes its own copy on read). */
                char   pin[16]   = { 0 };
                size_t pin_chars = ui_take_pin(pin, sizeof(pin));

                s_user_cancelled = false;
                char tx_hash[68] = { 0 };
                char err_msg[64] = { 0 };
                bool ok = sign_and_broadcast(wallet, nfcTransport, pending_amount,
                                              pin, pin_chars,
                                              tx_hash, sizeof(tx_hash),
                                              err_msg, sizeof(err_msg));
                /* scrub our copy of the PIN as soon as signing is done. */
                CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(pin), sizeof(pin));

                pending_amount = 0U;  /* next sign requires fresh New Payment flow */
                if (s_user_cancelled) {
                    /* Cancel during PLACE_CARD — UI already on amount entry. */
                } else if (ok) {
                    /* Broadcast accepted only means "entered the mempool" — a
                     * POS must not claim Approved until the tx is mined with
                     * status 0x1. Poll the receipt (Sepolia block ~12 s). */
                    ui_show_tx_status(UI_TX_STATE_CONFIRMING, NULL);
                    eth_rpc_receipt_result_t rc = ETH_RPC_RECEIPT_PENDING;
                    const int64_t deadline =
                        esp_timer_get_time() + 120LL * 1000000LL;  /* 120 s */
                    while (esp_timer_get_time() < deadline) {
                        rc = eth_rpc_get_tx_receipt(tx_hash);
                        if ((rc == ETH_RPC_RECEIPT_SUCCESS) ||
                            (rc == ETH_RPC_RECEIPT_REVERTED)) {
                            break;
                        }
                        /* PENDING or transient RPC error — try again. */
                        vTaskDelay(pdMS_TO_TICKS(4000));
                    }
                    if (rc == ETH_RPC_RECEIPT_SUCCESS) {
                        ESP_LOGI(TAG, "Tx confirmed on-chain");
                        ui_show_tx_status(UI_TX_STATE_DONE, tx_hash);
                    } else if (rc == ETH_RPC_RECEIPT_REVERTED) {
                        ESP_LOGE(TAG, "Tx reverted on-chain: %s", tx_hash);
                        ui_show_tx_status(UI_TX_STATE_FAILED, "Payment reverted");
                    } else {
                        ESP_LOGE(TAG, "Tx not confirmed after 120 s: %s", tx_hash);
                        ui_show_tx_status(UI_TX_STATE_FAILED,
                                          "Confirmation timeout - check explorer");
                    }
                } else {
                    ui_show_tx_status(UI_TX_STATE_FAILED, err_msg);
                }
                break;
            }

            case UI_EVENT_TX_RETRY:
                ui_show_amount_entry();
                break;

            case UI_EVENT_WIFI_SCAN: {
                /* Scan runs in this task so the UI stays responsive. */
                net_wifi_ap_t aps[16];
                uint16_t n = net_wifi_scan(aps, 16);
                ui_show_wifi_list(aps, n);
                break;
            }

            case UI_EVENT_WIFI_TRY: {
                char w_ssid[33] = { 0 };
                char w_pass[65] = { 0 };
                if (ui_take_wifi_creds(w_ssid, sizeof(w_ssid),
                                       w_pass, sizeof(w_pass)) > 0U) {
                    ui_show_wifi_connecting(w_ssid);
                    if (net_wifi_connect(w_ssid, w_pass)) {
                        settings_set_wifi(w_ssid, w_pass);   /* persist for next boot */
                        ui_show_amount_entry();
                    } else {
                        /* Failed — rescan and show the list again to retry. */
                        net_wifi_ap_t aps[16];
                        uint16_t n = net_wifi_scan(aps, 16);
                        ui_show_wifi_list(aps, n);
                    }
                }
                CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(w_pass), sizeof(w_pass));
                break;
            }

            default:
                break;
        }
    }
}
