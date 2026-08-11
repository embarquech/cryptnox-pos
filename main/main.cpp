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
#include "esp_err.h"      /* esp_err_to_name for the startup fault screen */
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
#include "recip_store.h"
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
 * @param[out] out    68-byte output buffer.
 * @param[in]  to20   Recipient, already decoded and verified by recip_store —
 *                    this function does not parse, so there is nothing here
 *                    that could accept an address the checks rejected.
 * @param[in]  amount Transfer amount in USDC base units (6 decimals).
 */
static void build_usdc_calldata(uint8_t out[68],
                                const uint8_t to20[RECIP_ADDR_BYTES],
                                uint64_t amount)
{
    (void)memset(out, 0, 68U);
    (void)CW_Utils::safe_memcpy(out, 68U, TRANSFER_SELECTOR, 4U);
    (void)CW_Utils::safe_memcpy(out + 4U + 12U, 68U - (4U + 12U),
                                to20, RECIP_ADDR_BYTES);

    size_t j;
    for (j = 0U; j < 8U; j++) {
        out[67U - j] = static_cast<uint8_t>((amount >> (8U * j)) & 0xFFU);
    }
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
                                const uint8_t to20[RECIP_ADDR_BYTES],
                                const char *pin, size_t pin_chars,
                                char *tx_hash_out, size_t tx_hash_max,
                                char *err_out, size_t err_max)
{
    /* The recipient arrives already decoded, out of the second of the two NVS
     * reads (recip_store.h). Nothing here re-derives it from a config literal:
     * that would quietly bypass everything the store just verified. */
    uint8_t calldata[68];
    build_usdc_calldata(calldata, to20, amount_units);

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
/* Startup retry budgets. The effort is spent out here rather than inside
 * net_wifi_connect(), since each call resets the association properly:
 * 3 x (1 + WIFI_MAX_RETRY) associations, 45 s worst case. */
#define WIFI_SAVED_ATTEMPTS  3U
#define TIME_SYNC_ATTEMPTS   3U

/* Picker notes, shared by the boot bring-up and the settings Wi-Fi change so the
 * two cannot drift apart. */
static const char *const NOTE_JOIN_FAILED =
    "Could not join that network - check the password";
static const char *const NOTE_NO_TIME =
    "No network time - this Wi-Fi has no usable internet";

/* Credentials the picker just joined with, held until a clock sync proves the
 * network usable end to end (see wifi_keep_or_drop). Associating is not enough:
 * a captive-portal or offline AP joins fine and would then be reached for on
 * every boot. Deferring the write also means a transient NTP outage never
 * erases a saved network that does work. */
static char s_join_ssid[33] = { 0 };
static char s_join_pass[65] = { 0 };

/**
 * @brief Persist or discard the pending picker credentials, then scrub them.
 *
 * @param[in] keep true once the clock is set — the only proof the network is
 *                 actually usable; false to drop them unpersisted.
 */
static void wifi_keep_or_drop(bool keep)
{
    if (keep && (s_join_ssid[0] != '\0')) {
        settings_set_wifi(s_join_ssid, s_join_pass);
        ESP_LOGI(TAG, "saved network '%s' (clock synced)", s_join_ssid);
    }
    CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(s_join_pass), sizeof(s_join_pass));
    (void)memset(s_join_ssid, 0, sizeof(s_join_ssid));
}

/**
 * @brief Block until the UI reports @p want, discarding anything else.
 *
 * For the first-run steps, which are modal by design: nothing else the operator
 * can tap matters until the step is done. The queue is flushed on the way out —
 * a repeated tap would otherwise be read by the next stage, and ensure_wifi()
 * treats an event it does not recognise as "rescan and reopen the picker",
 * which would throw away a password half typed.
 */
static void wait_for_ui_event(ui_event_t want)
{
    ui_msg_t msg;
    bool     got = false;
    while (!got) {
        if (xQueueReceive(s_ui_queue, &msg, portMAX_DELAY) != pdTRUE) { continue; }
        got = (msg.event == want);
    }
    (void)xQueueReset(s_ui_queue);
}

/**
 * @brief Bring up Wi-Fi: retry the saved credentials, then run the network
 *        picker (scan → list → keyboard → connect) until connected.
 *
 * Blocks until a connection succeeds; the operator cannot leave setup without
 * one. config.h Wi-Fi credentials are intentionally not used (NVS only).
 *
 * A network joined through the picker is not persisted here — the credentials
 * are staged for @ref wifi_keep_or_drop, which the caller invokes once the
 * clock proves the uplink usable.
 *
 * @param[in] try_saved Try the saved credentials first; false goes straight to
 *                      the picker, for a saved network already proven unusable.
 * @param[in] note      One-line reason shown above the picker, or NULL.
 * @return true if the picker ran and took the screen, so the caller must
 *         restore the splash; false if the splash was never replaced.
 */
static bool ensure_wifi(bool try_saved, const char *note)
{
    char ssid[33] = { 0 };
    char pass[65] = { 0 };
    if (try_saved &&
        settings_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass))) {
        /* Unattended: report on the splash rather than flashing up a setup
         * screen the operator never asked for. */
        ui_set_boot_status("Connecting to Wi-Fi");
        bool ok = false;
        for (uint32_t a = 1U; (a <= WIFI_SAVED_ATTEMPTS) && !ok; a++) {
            ESP_LOGI(TAG, "Wi-Fi '%s': attempt %" PRIu32 "/%u",
                     ssid, a, WIFI_SAVED_ATTEMPTS);
            ok = net_wifi_connect(ssid, pass);
        }
        CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(pass), sizeof(pass));
        if (ok) { return false; }   /* splash never left the screen */
        ESP_LOGW(TAG, "saved network '%s' failed %u times - opening the picker",
                 ssid, WIFI_SAVED_ATTEMPTS);
        note = "Could not join the saved network";
    }

    /* No usable saved network — forced interactive setup. */
    net_wifi_ap_t aps[16];
    uint16_t n = net_wifi_scan(aps, 16);
    ui_show_wifi_list(aps, n, note);

    ui_msg_t msg;
    while (true) {
        if (xQueueReceive(s_ui_queue, &msg, portMAX_DELAY) != pdTRUE) { continue; }

        if (msg.event == UI_EVENT_WIFI_TRY) {
            char w_ssid[33] = { 0 };
            char w_pass[65] = { 0 };
            bool ok = false;
            if (ui_take_wifi_creds(w_ssid, sizeof(w_ssid),
                                   w_pass, sizeof(w_pass)) > 0U) {
                /* Interactive: the operator expects to see the attempt. */
                ui_show_wifi_connecting(w_ssid);
                ok = net_wifi_connect(w_ssid, w_pass);
                if (ok) {
                    /* Staged, not saved — wifi_keep_or_drop() decides once the
                     * clock has proven this network carries real internet. */
                    (void)snprintf(s_join_ssid, sizeof(s_join_ssid), "%s", w_ssid);
                    (void)snprintf(s_join_pass, sizeof(s_join_pass), "%s", w_pass);
                }
            }
            CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(w_pass), sizeof(w_pass));
            if (ok) { return true; }   /* the picker owns the screen */
            note = NOTE_JOIN_FAILED;
        } else if (msg.event == UI_EVENT_WIFI_SCAN) {
            note = NULL;   /* rescan asked for — the old reason is stale */
        }

        /* WIFI_SCAN, a failed connect, or any stray event: re-scan and show
         * the list again so the user stays in setup until connected. */
        n = net_wifi_scan(aps, 16);
        ui_show_wifi_list(aps, n, note);
    }
}

/**
 * @brief Block on an SNTP sync so TLS certificate validity-period checks run
 *        against real time instead of the 1970 epoch.
 *
 * @return true once the clock is set, false after @ref TIME_SYNC_ATTEMPTS
 *         rounds — flaky uplinks often need a second try.
 */
static bool sync_time(void)
{
    for (uint32_t a = 1U; a <= TIME_SYNC_ATTEMPTS; a++) {
        ESP_LOGI(TAG, "SNTP sync: attempt %" PRIu32 "/%u", a, TIME_SYNC_ATTEMPTS);
        if (net_time_sync(15000U)) { return true; }
    }
    return false;
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

    /* Recipients before the UI: the settings tab reads them the moment it is
     * built, and an un-provisioned pair has to fall back to the config literal
     * here rather than show "Not set" on a terminal that works fine. */
    recip_store_init();

    /* ── UI: splash visible while the rest boots ───────── */
    s_ui_queue = xQueueCreate(8, sizeof(ui_msg_t));
    ui_init(ui_event_dispatch);
    /* No ui_show_splash() here — ui_init() already selects it, and asking twice
     * races the UI task into rebuilding the screen and replaying the logo. */

    /* ── PN532 NFC reader ──────────────────────────────────────── */
    ui_set_boot_status("Starting NFC reader");
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

    /* Keep the error code — unplugged reader and misconfigured bus look
     * identical on screen otherwise. */
    esp_err_t nfc_ret = pn532_init(&nfc, &nfc_cfg);
    if (nfc_ret != ESP_OK) {
        ESP_LOGE(TAG, "PN532 bus init failed: %s", esp_err_to_name(nfc_ret));
        ui_show_boot_error(UI_BOOT_ERR_NFC, esp_err_to_name(nfc_ret));
        return;
    }

    /* pn532_init() only brings up the bus and ignores its own probe results
     * (pn532.h), so it returns ESP_OK with no reader attached. Probe here —
     * 0 means no answer — or an absent reader is reported as a wallet fault. */
    uint32_t nfc_fw = pn532_get_firmware_version(&nfc);
    if (nfc_fw == 0U) {
        ESP_LOGE(TAG, "PN532 did not answer GetFirmwareVersion - reader absent?");
        ui_show_boot_error(UI_BOOT_ERR_NFC, "No answer to GetFirmwareVersion");
        return;
    }
    ESP_LOGI(TAG, "PN532 firmware: IC 0x%02X, version %u.%u",
             (unsigned)((nfc_fw >> 24) & 0xFFU),
             (unsigned)((nfc_fw >> 16) & 0xFFU),
             (unsigned)((nfc_fw >> 8) & 0xFFU));

    /* ── Wallet ────────────────────────────────────────────────── */
    ui_set_boot_status("Opening wallet");
    NullLogger logger;
    (void)logger.begin(115200UL);
    ESP32CryptoProvider cryptoProvider;
    Pn532NfcTransport   nfcTransport(&nfc, logger);
    ESP32Platform       platform;
    CryptnoxWallet      wallet(nfcTransport, logger, cryptoProvider, platform);

    if (!wallet.begin()) {
        ESP_LOGE(TAG, "Wallet begin failed");
        ui_show_boot_error(UI_BOOT_ERR_WALLET, NULL);
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
    /* ── First run: greet, then set up ────────────────────────── */
    /* A missing admin code means a virgin or factory-reset terminal, since the
     * reset erases it too. Greet before the setup steps start asking for a
     * network and a code — it is the one moment we have the operator's attention
     * and nothing to demand of them yet. */
    const bool first_run = !settings_has_admin_code();
    if (first_run) {
        ui_show_welcome();
        wait_for_ui_event(UI_EVENT_WELCOME_DONE);

        /* The code comes BEFORE the network, and not for tidiness: the Wi-Fi
         * picker carries a back arrow that the UI task honours on its own, which
         * drops the operator on the amount screen — burger included — while main
         * is still blocked here. With no code stored yet, that burger opened the
         * settings freely. Creating the code first closes that window; the
         * creation screen itself has no way out. */
        ESP_LOGI(TAG, "no admin code - first-run setup");
        ui_show_admin_set();
        wait_for_ui_event(UI_EVENT_ADMIN_SET);
    }

    ui_set_boot_status("Starting network");
    net_wifi_init();

    /* Wi-Fi and a valid clock are one bring-up step, since TLS needs both: a
     * failed sync sends the operator back to the picker with the reason rather
     * than stranding the terminal on an error screen. */
    bool        try_saved = true;
    const char *net_note  = NULL;
    while (true) {
        if (ensure_wifi(try_saved, net_note)) {
            ui_show_splash();   /* only when the picker took the screen */
        }
        ui_set_boot_status("Syncing clock");
        if (sync_time()) {
            wifi_keep_or_drop(true);    /* proven usable — safe to persist */
            break;
        }
        ESP_LOGE(TAG, "SNTP time sync failed on this network");
        /* Drop the staged credentials, but leave an already-saved network alone:
         * it may well work again after a reboot, and erasing it would cost the
         * operator the password for what is often a transient outage. */
        wifi_keep_or_drop(false);
        /* Force the picker: retrying the same network loops straight back here. */
        try_saved = false;
        net_note  = NOTE_NO_TIME;
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

    /* The recipient in flight, plus the step counter that proves it went
     * through display and confirm before it reached signing. Scoped to one
     * payment: reset on every new amount, so nothing survives into the next. */
    char           pending_recip[RECIP_ADDR_MAX] = { 0 };
    recip_steps_t  recip_steps;
    recip_steps_reset(&recip_steps);
    const recip_pair_t pair = RECIP_PAIR_ETH_USDC;

    while (true) {
        if (xQueueReceive(s_ui_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (msg.event) {
            case UI_EVENT_AMOUNT_CONFIRMED:
                pending_amount = msg.payload;
                /* First of the two reads. Start a fresh step sequence: whatever
                 * a previous, abandoned payment left behind must not count
                 * towards this one's gates. */
                recip_steps_reset(&recip_steps);
                if (!RECIP_IS_OK(recip_read_display(pair, pending_recip,
                                                    sizeof(pending_recip),
                                                    &recip_steps))) {
                    ESP_LOGE(TAG, "recipient failed verification before display");
                    ui_show_tx_status(UI_TX_STATE_FAILED,
                                      "Recipient check failed - see settings");
                    pending_amount = 0U;
                    break;
                }
                ui_show_confirm(pending_amount, pending_recip);
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

                /* The PIN keypad is the confirmation: the customer has now seen
                 * the address and the operator has committed to it. Then the
                 * second read — a different NVS key, at a later moment — which
                 * also re-decodes and cross-checks the step counter. */
                uint8_t to20[RECIP_ADDR_BYTES];
                bool ok = RECIP_IS_OK(recip_checkpoint(pair, RECIP_STEP_CONFIRM,
                                                       &recip_steps)) &&
                          RECIP_IS_OK(recip_read_signing(pair, pending_recip,
                                                         to20, &recip_steps));
                if (!ok) {
                    /* Fail closed: no arbitration between the two copies, no
                     * retry with the other one. */
                    ESP_LOGE(TAG, "recipient failed verification before signing");
                    (void)snprintf(err_msg, sizeof(err_msg),
                                   "Recipient check failed");
                } else {
                    ok = sign_and_broadcast(wallet, nfcTransport, pending_amount,
                                            to20, pin, pin_chars,
                                            tx_hash, sizeof(tx_hash),
                                            err_msg, sizeof(err_msg));
                }
                CW_Utils::secure_wipe(to20, sizeof(to20));
                /* scrub our copy of the PIN as soon as signing is done. */
                CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(pin), sizeof(pin));

                pending_amount = 0U;  /* next sign requires fresh New Payment flow */
                recip_steps_reset(&recip_steps);
                (void)memset(pending_recip, 0, sizeof(pending_recip));
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
                /* Opened from settings, not after a failure — no note. */
                ui_show_wifi_list(aps, n, NULL);
                break;
            }

            case UI_EVENT_WIFI_TRY: {
                char w_ssid[33] = { 0 };
                char w_pass[65] = { 0 };
                if (ui_take_wifi_creds(w_ssid, sizeof(w_ssid),
                                       w_pass, sizeof(w_pass)) > 0U) {
                    ui_show_wifi_connecting(w_ssid);

                    /* Same rule as boot: associating proves nothing, so make the
                     * clock prove the uplink before overwriting saved credentials
                     * that may well be working. One round only — the operator is
                     * standing there and can tap again, where boot has to be
                     * patient on its own. */
                    const char *why = NULL;
                    if (!net_wifi_connect(w_ssid, w_pass)) {
                        why = NOTE_JOIN_FAILED;
                    } else if (!net_time_sync(15000U)) {
                        ESP_LOGW(TAG, "'%s' joined but has no network time -"
                                      " not saved", w_ssid);
                        why = NOTE_NO_TIME;
                    } else {
                        settings_set_wifi(w_ssid, w_pass);   /* persist for next boot */
                        ui_show_amount_entry();
                    }

                    if (why != NULL) {
                        /* Refusing to persist is not enough: the radio is still
                         * associated with the network we just rejected, and the
                         * picker's back arrow goes straight to amount entry. So
                         * without this the terminal looks ready while every
                         * payment fails at the RPC call. Roll back to the saved
                         * network — which boot already proved usable — before
                         * handing over the screen. Costs up to one association
                         * timeout, hence the progress screen. */
                        char b_ssid[33] = { 0 };
                        char b_pass[65] = { 0 };
                        if (settings_get_wifi(b_ssid, sizeof(b_ssid),
                                              b_pass, sizeof(b_pass)) &&
                            (b_ssid[0] != '\0')) {
                            ESP_LOGW(TAG, "rolling back to saved network '%s'",
                                     b_ssid);
                            ui_show_wifi_connecting(b_ssid);
                            (void)net_wifi_connect(b_ssid, b_pass);
                        }
                        CW_Utils::secure_wipe(
                            reinterpret_cast<uint8_t *>(b_pass), sizeof(b_pass));

                        /* Back to the picker with the reason, so another network
                         * can be chosen instead of a dead end. */
                        net_wifi_ap_t aps[16];
                        uint16_t n = net_wifi_scan(aps, 16);
                        ui_show_wifi_list(aps, n, why);
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
