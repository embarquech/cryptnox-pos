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
#include "CW_Tron.h"
#include "settings.h"
#include "provision.h"   /* phone-based first-run setup (SoftAP + portal) */
#include "ota.h"         /* browser-mediated firmware update + rollback confirm */

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
#include "hardening.h"
#include "eth_rlp.h"
#include "eth_rpc.h"
#include "tron_rpc.h"
#include "tron_tx.h"
#include "net.h"
#include "ui.h"
}

#include "config.h"

/* Tron TRC-20 support post-dates the first config.h files in the field. An
 * empty contract fails its base58 decode at boot, which disables that asset
 * rather than breaking the build of a config that predates it. */
#ifndef TRON_ADDR_USDT
#define TRON_ADDR_USDT  ""
#endif
#ifndef TRON_TRC20_FEE_LIMIT_SUN
#define TRON_TRC20_FEE_LIMIT_SUN  100000000ULL
#endif

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

/* The CYD's onboard RGB LED, common anode: the pin is the cathode side, so HIGH
 * is off and a floating pin is a glow. Nothing here uses it, and a till lighting
 * up at a customer for no stated reason is worse than no indicator at all. */
#define LED_R               GPIO_NUM_4
#define LED_G               GPIO_NUM_16
#define LED_B               GPIO_NUM_17

/* ── USDC ERC-20 transfer(address,uint256) selector + calldata ── */
static const uint8_t TRANSFER_SELECTOR[4] = { 0xa9U, 0x05U, 0x9cU, 0xbbU };
#define ABI_SELECTOR_LEN    4U     /* transfer(address,uint256) selector      */
#define ABI_WORD_LEN        32U    /* one ABI-encoded argument word           */
#define USDC_CALLDATA_LEN   (ABI_SELECTOR_LEN + (2U * ABI_WORD_LEN))  /* 68 */
#define ABI_TO_OFFSET       (ABI_SELECTOR_LEN + (ABI_WORD_LEN - ETH_ADDR_LEN))

/* ── Unsigned and signed tx buffers (EIP-1559 type 2) ─────────── */
#define TX_BUF_SIZE 300U

/* ── Tron ──────────────────────────────────────────────────────
 * The sender is not configured: the card's m/44'/195'/0'/0/0 public key is read
 * over the secure channel at sign time and turned into an address by CW_Tron, so
 * the terminal follows whichever card is presented. */

/* BIP32 Ethereum derivation path: m/44'/60'/0'/0/0 */
static const uint8_t ETH_DERIVE_PATH[20] = {
    0x80U, 0x00U, 0x00U, 0x2CU,
    0x80U, 0x00U, 0x00U, 0x3CU,
    0x80U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U,
};

/**
 * @brief Format a raw 21-byte Tron address as the "41..." hex the API wants.
 *
 * @param[in]  addr21 21-byte address (0x41 prefix included).
 * @param[out] out    #TRON_ADDR_HEX_LEN chars + NUL.
 * @param[in]  n      Capacity of @p out.
 */
static void tron_addr_to_hex(const uint8_t *addr21, char *out, size_t n)
{
    if (n < (TRON_ADDR_HEX_LEN + 1U)) { if (n > 0U) { out[0] = '\0'; } return; }
    for (size_t i = 0U; i < CW_TRON_ADDRESS_BYTES; i++) {
        (void)snprintf(&out[i * 2U], 3U, "%02x",
                       static_cast<unsigned>(addr21[i]));
    }
}

/** @brief true when the operator has switched the terminal to Tron. */
static bool chain_is_tron(void) {
    return settings_get_chain() != POS_CHAIN_ETH_SEPOLIA;
}

/**
 * @brief A TRC-20 token the terminal can charge in.
 *
 * The contract address decides *which* asset moves, so it gets the same
 * dual-store treatment as the recipient (§3.2/§7.1) rather than being read out
 * of config.h at signing time.
 */
typedef struct {
    const char *b58;    /**< as configured — also what the confirm screen shows */
    pos_addr_t  addr;   /**< dual-stored 20-byte key hash (no 0x41 prefix)      */
    bool        ok;     /**< false until decoded: refuse to charge in it        */
} trc20_asset_t;

/* Points at s_contract_trx, filled from settings at boot — the operator can set the
 * contract from the config page, and config.h is only the fallback. */
static trc20_asset_t s_trc20_usdt = { NULL, {}, false };

/**
 * @brief Decode a token's configured base58 contract into its dual store.
 *
 * Base58check-decoded twice by the SDK, which verifies the checksum — so a
 * mistyped contract is refused here rather than charged against the wrong
 * asset, exactly as the recipient is handled.
 *
 * @return false if the address does not decode; the token stays unusable.
 */
static bool trc20_load(trc20_asset_t *a, CW_CryptoProvider &crypto) {
    uint8_t c21[CW_TRON_ADDRESS_BYTES];
    uint8_t c21_echo[CW_TRON_ADDRESS_BYTES];
    if (!CW_Tron::decodeAddress(a->b58, crypto, c21) ||
        !CW_Tron::decodeAddress(a->b58, crypto, c21_echo)) {
        return false;
    }
    (void)CW_Utils::safe_memcpy(a->addr.addr, sizeof(a->addr.addr),
                                &c21[1], ETH_ADDR_LEN);
    (void)CW_Utils::safe_memcpy(a->addr.addr_echo, sizeof(a->addr.addr_echo),
                                &c21_echo[1], ETH_ADDR_LEN);
    a->ok = true;
    return true;
}

/** @brief The selected TRC-20 token, or NULL for native TRX / Ethereum. */
static trc20_asset_t *active_trc20(void) {
    return (settings_get_chain() == POS_CHAIN_TRON_USDT) ? &s_trc20_usdt : NULL;
}

/* The recipient and contract strings the dual stores are built from: whatever the
 * operator set through the config page, or the config.h value when they never set
 * one. Resolved once at boot — an address changed during setup takes effect through
 * a restart, so it goes through this same path rather than a second one that would
 * have to re-validate everything this one already validates. The Ethereum strings
 * are "0x"-prefixed, ready for eth_addr_parse. */
static char s_payout_eth[SETTINGS_PAYOUT_MAX]   = "";
static char s_payout_tron[SETTINGS_PAYOUT_MAX]  = "";
static char s_contract_eth[SETTINGS_PAYOUT_MAX] = "";
static char s_contract_trx[SETTINGS_PAYOUT_MAX] = "";

/**
 * @brief Point the UI's address rows at the selected chain.
 *
 * Called on every entry to the confirm screen, not once at boot: the chain is
 * switched from the settings menu while this task is parked on its queue.
 */
static void ui_refresh_addresses(void) {
    const trc20_asset_t *token = active_trc20();
    if (token != NULL) {
        ui_set_addresses(token->b58, s_payout_tron);
    } else if (chain_is_tron()) {
        ui_set_addresses("Native TRX (no contract)", s_payout_tron);
    } else {
        ui_set_addresses(s_contract_eth, s_payout_eth);
    }
}

/******************************************************************
 * 3. UI ↔ main task message queue
 ******************************************************************/

typedef struct {
    ui_event_t event;
    uint64_t   payload;
} ui_msg_t;

static QueueHandle_t s_ui_queue = NULL;

/* Dual-stored recipient (§3.2/§7.1): ADDR_TO parsed twice at boot into two
 * independent copies, reconciled before calldata encode and before signing so
 * a transient flip on the working copy can't redirect funds. */
static pos_addr_t s_dest;

/* Same treatment for the Tron recipient: it is a different address, so it needs
 * its own dual store rather than borrowing the Ethereum one. */
static pos_addr_t s_tron_dest;

/* And the ERC-20 contract. It used to be a config.h literal parsed at signing time,
 * which needed no protection because it lived in the signed app image. It is now an
 * operator-settable value in NVS, and the address the terminal *calls* decides which
 * asset moves just as surely as the recipient decides who is paid — so it gets the
 * same dual store and the same reconciles. The Tron token contract already had one
 * (trc20_asset_t). */
static pos_addr_t s_usdc;

/** @brief The reconciled recipient for the chain currently selected. */
static const pos_addr_t *active_dest(void) {
    return chain_is_tron() ? &s_tron_dest : &s_dest;
}

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
 * @param[out] out    Output buffer of #USDC_CALLDATA_LEN bytes.
 * @param[in]  to     Recipient address, #ETH_ADDR_LEN bytes (already
 *                    parsed/validated).
 * @param[in]  amount Transfer amount in USDC base units (6 decimals).
 */
static void build_usdc_calldata(uint8_t out[USDC_CALLDATA_LEN],
                                const uint8_t to[ETH_ADDR_LEN],
                                uint64_t amount)
{
    CW_Utils::secure_wipe(out, USDC_CALLDATA_LEN);
    (void)CW_Utils::safe_memcpy(out, USDC_CALLDATA_LEN,
                                TRANSFER_SELECTOR, ABI_SELECTOR_LEN);
    (void)CW_Utils::safe_memcpy(out + ABI_TO_OFFSET,
                                USDC_CALLDATA_LEN - ABI_TO_OFFSET,
                                to, ETH_ADDR_LEN);

    size_t j;
    for (j = 0U; j < sizeof(amount); j++) {
        out[(USDC_CALLDATA_LEN - 1U) - j] =
            static_cast<uint8_t>((amount >> (8U * j)) & 0xFFU);
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
 * @brief Wait for a card and open a secure channel, cancellable from the UI.
 *
 * Manual connect loop with cancel checks between PN532 polls — replaces
 * wallet.connect() so a Cancel from the user aborts within one PN532 timeout.
 * Drives the "Tap your card" / "Processing" screens itself.
 *
 * @param[in]  wallet    Initialised wallet instance.
 * @param[in]  transport PN532 transport, polled directly.
 * @param[out] session   Open secure session on success.
 * @param[in]  setup     true when the card is being read to derive a payout
 *                       address rather than to pay. The transaction screen says
 *                       "Transaction", prints a total and offers a Cancel whose
 *                       meaning is "abandon this sale" — none of which is true
 *                       during setup, so that case gets its own screen.
 * @return true with @p session open; false on user cancel or after 60 s —
 *         the two are told apart by @ref s_user_cancelled.
 */
static bool card_connect(CryptnoxWallet &wallet, Pn532NfcTransport &transport,
                         CW_SecureSession &session, bool setup = false)
{
    if (!s_user_cancelled) {
        if (setup) { ui_show_card_wait("Reading your payout addresses"); }
        else { ui_show_tx_status(UI_TX_STATE_PLACE_CARD, "Hold card to reader"); }
    }

    /* Start from a clean reader state: a previous attempt that ended with the
     * card ripped away mid-exchange can leave a stale target selected, which
     * would make every InListPassiveTarget below time out. */
    transport.resetReader();

    /* Give the user up to 60 s to present the card. */
    const int64_t card_wait_us = 60LL * 1000000LL;
    const int64_t start_us     = esp_timer_get_time();
    while (true) {
        if (s_user_cancelled) {
            return false;
        }
        if ((esp_timer_get_time() - start_us) > card_wait_us) {
            return false;   /* timed out waiting for the card */
        }
        if (transport.inListPassiveTarget()) {
            /* Card tapped — show immediate feedback while the secure channel
             * comes up. */
            if (setup) { ui_show_card_wait("Reading..."); }
            else { ui_show_tx_status(UI_TX_STATE_PROCESSING, NULL); }
            vTaskDelay(pdMS_TO_TICKS(200));
            if (wallet.establishSecureChannel(session)) {
                return true;
            }
            /* Card pulled away (or channel failed) — the PN532 still holds the
             * now-dead target selected, which makes every following
             * InListPassiveTarget time out. Release it so the reader can see
             * the card again, then back to the tap prompt. */
            transport.resetReader();
            if (!s_user_cancelled) {
                if (setup) { ui_show_card_wait("Hold it still"); }
                else { ui_show_tx_status(UI_TX_STATE_PLACE_CARD, "Hold card to reader"); }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

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
                                const pos_amount_t *amount,
                                const pos_addr_t *to,
                                const char *pin, size_t pin_chars,
                                char *tx_hash_out, size_t tx_hash_max,
                                char *err_out, size_t err_max)
{
    /* Reconcile amount + recipient + contract right before they enter the calldata
     * (§3.2/§7.1); a mismatch means the working copy was corrupted. */
    if (!IS_TRUE32(amount_consistent(amount)) ||
        !IS_TRUE32(address_consistent(to)) ||
        !IS_TRUE32(address_consistent(&s_usdc))) {
        pos_handle_anomaly("pre-calldata reconcile");
        (void)snprintf(err_out, err_max, "Integrity check failed");
        return false;
    }
    const uint64_t amount_units = amount->amount_minor;

    uint8_t calldata[USDC_CALLDATA_LEN];
    build_usdc_calldata(calldata, to->addr, amount_units);

    uint64_t nonce = 0U;
    if (!eth_rpc_get_nonce(&nonce)) {
        (void)snprintf(err_out, err_max, "RPC: get nonce failed");
        return false;
    }

    eth_tx_t tx;
    CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(&tx), sizeof(tx));
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
    /* From the reconciled store, not from a literal or from NVS at this moment:
     * this is the address the transaction calls, so it comes from the copy that was
     * validated at boot and has just been checked against its echo. */
    (void)CW_Utils::safe_memcpy(tx.to, sizeof(tx.to),
                                s_usdc.addr, ETH_ADDR_LEN);

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

    CW_SecureSession session;
    if (!card_connect(wallet, transport, session)) {
        if (!s_user_cancelled) {
            (void)snprintf(err_out, err_max, "Card not found");
        }
        return false;
    }

    if (!s_user_cancelled) {
        ui_show_tx_status(UI_TX_STATE_SIGNING, NULL);
    }

    CW_SignRequest req(session,
                       CW_SIGN_DERIVE_K1,
                       CW_SIGN_SIG_ECDSA_LOW_S,
                       CW_SIGN_WITH_PIN);
    req.hash             = hash;
    req.hashLength       = static_cast<uint8_t>(CW_HASH_SIZE);
    req.derivePath       = ETH_DERIVE_PATH;
    req.derivePathLength = static_cast<uint8_t>(sizeof(ETH_DERIVE_PATH));

    /* Re-reconcile amount + recipient + contract right before signing — this is the
     * last point before the card produces an irreversible signature over the
     * calldata (§3.2/§7.1). */
    if (!IS_TRUE32(amount_consistent(amount)) ||
        !IS_TRUE32(address_consistent(to)) ||
        !IS_TRUE32(address_consistent(&s_usdc))) {
        pos_handle_anomaly("pre-sign reconcile");
        (void)snprintf(err_out, err_max, "Integrity check failed");
        return false;
    }

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

/**
 * @brief Sign a Tron transfer on the card and broadcast it — TRX or TRC-20.
 *
 * Same shape as @ref sign_and_broadcast, but Tron has no RLP and no local
 * nonce: the full node serialises the transaction and we sign its txID. What
 * the node returns is therefore verified before the card ever sees the hash
 * (see tron_rpc.h), and the recipient handed to the node is derived from the
 * dual-stored @p to right after the reconcile, never from a config literal.
 *
 * TRX and TRC-20 differ only in which transaction the node is asked to build;
 * card handling, the reconciles and the broadcast are identical, so they share
 * one function rather than a near-copy that could drift on the security checks.
 *
 * @param[in]  wallet       Initialised wallet instance.
 * @param[in]  transport    PN532 transport (cancellable connect loop).
 * @param[in]  amount       Dual-stored amount, 6 decimals — sun for TRX, token
 *                          base units for TRC-20.
 * @param[in]  to           Dual-stored recipient (20-byte key hash).
 * @param[in]  token        Token to charge in, or NULL for native TRX.
 * @param[in]  pin          Operator-entered card PIN (scrubbed after signing).
 * @param[in]  pin_chars    Number of PIN characters in @p pin.
 * @param[out] txid_out     Transaction id hex on success (>= 65 bytes).
 * @param[in]  txid_max     Capacity of @p txid_out.
 * @param[out] err_out      Short UI-facing error message on failure.
 * @param[in]  err_max      Capacity of @p err_out.
 * @return true on successful broadcast; false on failure or user cancel.
 */
static bool sign_and_broadcast_tron(CryptnoxWallet &wallet,
                                     Pn532NfcTransport &transport,
                                     CW_CryptoProvider &crypto,
                                     const pos_amount_t *amount,
                                     const pos_addr_t *to,
                                     const trc20_asset_t *token,
                                     const char *pin, size_t pin_chars,
                                     char *txid_out, size_t txid_max,
                                     char *err_out, size_t err_max)
{
    if (!IS_TRUE32(amount_consistent(amount)) ||
        !IS_TRUE32(address_consistent(to)) ||
        ((token != NULL) && !IS_TRUE32(address_consistent(&token->addr)))) {
        pos_handle_anomaly("pre-create reconcile (tron)");
        (void)snprintf(err_out, err_max, "Integrity check failed");
        return false;
    }
    if ((token != NULL) && !token->ok) {
        (void)snprintf(err_out, err_max, "Token contract not configured");
        return false;
    }
    const uint64_t amount_sun = amount->amount_minor;   /* both 6 decimals */

    /* Recipient in Tron form, built from the reconciled copy. */
    uint8_t to21[CW_TRON_ADDRESS_BYTES];
    to21[0] = CW_TRON_ADDRESS_PREFIX;
    (void)CW_Utils::safe_memcpy(&to21[1], sizeof(to21) - 1U,
                                to->addr, ETH_ADDR_LEN);
    char to_hex[TRON_ADDR_HEX_LEN + 1U];
    tron_addr_to_hex(to21, to_hex, sizeof(to_hex));

    /* Same for the token contract — the node is told which contract to call,
     * so that address has to come from the reconciled store too. */
    char token_hex[TRON_ADDR_HEX_LEN + 1U] = { 0 };
    if (token != NULL) {
        uint8_t c21[CW_TRON_ADDRESS_BYTES];
        c21[0] = CW_TRON_ADDRESS_PREFIX;
        (void)CW_Utils::safe_memcpy(&c21[1], sizeof(c21) - 1U,
                                    token->addr.addr, ETH_ADDR_LEN);
        tron_addr_to_hex(c21, token_hex, sizeof(token_hex));
    }

    /* The card comes before the RPC on this path: Tron will not serialise a
     * transfer without the sender, and the sender is whoever tapped. */
    CW_SecureSession session;
    if (!card_connect(wallet, transport, session)) {
        if (!s_user_cancelled) {
            (void)snprintf(err_out, err_max, "Card not found");
        }
        return false;
    }

    if (!s_user_cancelled) {
        ui_show_tx_status(UI_TX_STATE_SIGNING, NULL);
    }

    /* Read this card's Tron account. The export needs the PIN verified for the
     * session first; the same PIN then rides along with the SIGN APDU. */
    char    owner_hex[TRON_ADDR_HEX_LEN + 1] = { 0 };
    char    owner_b58[CW_TRON_ADDRESS_STR_SIZE] = { 0 };
    uint8_t pubkey[64];
    uint8_t owner21[CW_TRON_ADDRESS_BYTES];
    WipeGuard g_pub(pubkey, sizeof(pubkey));
    if (!wallet.verifyPin(session,
                          reinterpret_cast<const uint8_t *>(pin),
                          static_cast<uint8_t>(pin_chars))) {
        wallet.disconnect(session);
        (void)snprintf(err_out, err_max, "Wrong PIN");
        return false;
    }
    if (!wallet.getPublicKey(session, CW_TRON_DERIVE_PATH,
                             CW_TRON_PATH_LENGTH, pubkey) ||
        !CW_Tron::addressBytesFromPublicKey(pubkey, owner21) ||
        !CW_Tron::encodeAddress(owner21, crypto,
                                owner_b58, sizeof(owner_b58))) {
        wallet.disconnect(session);
        (void)snprintf(err_out, err_max, "Cannot read card address");
        return false;
    }
    tron_addr_to_hex(owner21, owner_hex, sizeof(owner_hex));
    ESP_LOGI(TAG, "Tron sender (this card): %s", owner_b58);

    tron_tx_ctx_t tx;
    const bool built = (token == NULL)
        ? tron_rpc_create_transfer(owner_hex, to_hex, amount_sun, &tx)
        : tron_rpc_create_trc20_transfer(owner_hex, token_hex, to_hex,
                                         amount_sun, TRON_TRC20_FEE_LIMIT_SUN,
                                         &tx);
    if (!built) {
        wallet.disconnect(session);
        /* Most often an account the chain has never seen funded: Tron will not
         * build a transfer from it, and a TRC-20 call additionally needs TRX
         * for the energy the transfer burns. */
        (void)snprintf(err_out, err_max, "No %s on %.12s...",
                       (token == NULL) ? "TRX" : "funds/TRX", owner_b58);
        return false;
    }

    CW_SignRequest req(session,
                       CW_SIGN_DERIVE_K1,
                       CW_SIGN_SIG_ECDSA_LOW_S,
                       CW_SIGN_WITH_PIN);
    req.hash             = tx.txid;
    req.hashLength       = static_cast<uint8_t>(sizeof(tx.txid));
    req.derivePath       = CW_TRON_DERIVE_PATH;
    req.derivePathLength = CW_TRON_PATH_LENGTH;

    /* Last reconcile before the card produces an irreversible signature. */
    if (!IS_TRUE32(amount_consistent(amount)) ||
        !IS_TRUE32(address_consistent(to)) ||
        ((token != NULL) && !IS_TRUE32(address_consistent(&token->addr)))) {
        pos_handle_anomaly("pre-sign reconcile (tron)");
        (void)snprintf(err_out, err_max, "Integrity check failed");
        return false;
    }

    const size_t copy_len = (pin_chars < CW_MAX_PIN_LENGTH) ? pin_chars
                                                            : CW_MAX_PIN_LENGTH;
    (void)CW_Utils::safe_memcpy(req.pin, sizeof(req.pin),
                                reinterpret_cast<const uint8_t *>(pin),
                                copy_len);

    CW_SignResult result = wallet.sign(req);
    WipeGuard g_sig(result.signature, sizeof(result.signature));
    wallet.disconnect(session);
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

    /* Tron wants r || s || recovery id, and nothing on the card or in the API
     * tells us the id — so try 0 and fall back to 1, the same way the SDK's
     * TronSigning example does. A wrong id recovers to some other account, the
     * node answers SIGERROR and nothing is committed, so the retry is safe. The
     * two attempts carry identical raw_data, hence the identical txID: only one
     * transaction can ever exist. */
    uint8_t sig[65];
    WipeGuard g_sig65(sig, sizeof(sig));
    (void)CW_Utils::safe_memcpy(sig, sizeof(sig), sig_r, 32U);
    (void)CW_Utils::safe_memcpy(sig + 32U, sizeof(sig) - 32U, sig_s, 32U);

    /* last cancel check right before the irreversible broadcast. */
    if (s_user_cancelled) {
        return false;
    }

    bool sent = false;
    for (uint8_t v = 0U; (v < 2U) && !sent; v++) {
        sig[64] = v;
        sent = tron_rpc_broadcast(&tx, sig);
        if (!sent) {
            ESP_LOGW(TAG, "broadcast rejected with v=%u", static_cast<unsigned>(v));
        }
    }

    /* "Refused" and "we never heard back" are not the same thing, and only the
     * chain can tell them apart. A v=0 broadcast that landed but whose response
     * was lost leaves this loop with sent == false, and the v=1 retry is then
     * refused as a duplicate — so reporting a bare failure here would tell a
     * merchant a payment did not happen while it settles behind their back.
     *
     * Tron block time is ~3 s. Ask the chain about our txID before writing the
     * sale off: anything other than "no such transaction" means it is out there,
     * and the caller's own 120 s receipt poll is the right place to settle what
     * it did. The txID is fixed by raw_data, so both attempts carried the same
     * one and there is nothing ambiguous to look up. */
    if (!sent) {
        vTaskDelay(pdMS_TO_TICKS(4000));
        const tron_receipt_t r = tron_rpc_get_receipt(tx.txid_hex);
        if ((r == TRON_RECEIPT_SUCCESS) || (r == TRON_RECEIPT_FAILED)) {
            ESP_LOGW(TAG, "broadcast reported failure but %s is on-chain - "
                          "letting the receipt poll decide", tx.txid_hex);
            sent = true;
        }
    }

    if (!sent) {
        /* Genuinely not on the chain as far as the node can tell. Say that it is
         * unconfirmed rather than that it failed — the operator's next move is to
         * check the address, not to assume nothing happened. */
        (void)snprintf(err_out, err_max, "Not broadcast - unconfirmed");
        return false;
    }

    (void)snprintf(txid_out, txid_max, "%s", tx.txid_hex);
    return true;
}

/** @brief Map a Tron receipt onto the Ethereum verdicts the UI flow uses. */
static eth_rpc_receipt_result_t tron_receipt_as_eth(tron_receipt_t r)
{
    switch (r) {
        case TRON_RECEIPT_SUCCESS: return ETH_RPC_RECEIPT_SUCCESS;
        case TRON_RECEIPT_FAILED:  return ETH_RPC_RECEIPT_REVERTED;
        case TRON_RECEIPT_PENDING: return ETH_RPC_RECEIPT_PENDING;
        default:                   return ETH_RPC_RECEIPT_RPC_ERROR;
    }
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
 * a repeated tap would otherwise be read by the next stage, and wifi_picker()
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
 * @brief Try the saved credentials, staying on the splash while it happens.
 *
 * Unattended, so it reports through @ref ui_set_boot_status rather than flashing
 * up a setup screen the operator never asked for. config.h Wi-Fi credentials are
 * intentionally not used (NVS only).
 *
 * @return true if a saved network was joined.
 */
static bool wifi_try_saved(void)
{
    char ssid[33] = { 0 };
    char pass[65] = { 0 };
    if (!settings_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass))) {
        return false;
    }

    ui_set_boot_status("Connecting to Wi-Fi");
    bool ok = false;
    for (uint32_t a = 1U; (a <= WIFI_SAVED_ATTEMPTS) && !ok; a++) {
        ESP_LOGI(TAG, "Wi-Fi '%s': attempt %" PRIu32 "/%u",
                 ssid, a, WIFI_SAVED_ATTEMPTS);
        ok = net_wifi_connect(ssid, pass);
    }
    CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(pass), sizeof(pass));
    if (!ok) {
        ESP_LOGW(TAG, "saved network '%s' failed %u times",
                 ssid, WIFI_SAVED_ATTEMPTS);
    }
    return ok;
}

/**
 * @brief Run the panel network picker (scan → list → keyboard → connect) until
 *        connected.
 *
 * Blocks until a connection succeeds; the operator cannot leave setup without one.
 * This is now the fallback path rather than the main one — setup does Wi-Fi in the
 * browser (see @ref run_wizard), because typing a venue passphrase on a resistive
 * 240x320 panel is what that exists to avoid. It is still reached two ways: from
 * the settings menu, and when the SoftAP itself could not come up.
 *
 * A network joined here is not persisted — the credentials are staged for
 * @ref wifi_keep_or_drop, which the caller invokes once the clock proves the uplink
 * usable.
 *
 * @param[in] note One-line reason shown above the picker, or NULL.
 * @return true always, as a reminder to the caller that the picker took the screen
 *         and the splash has to be restored.
 */
static bool wifi_picker(const char *note)
{
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

/******************************************************************
 * 5b. The setup wizard
 ******************************************************************/

/**
 * @brief Read the card's payout addresses and put them through the panel.
 *
 * The card will not export a public key without a verified PIN, so this needs the
 * card PIN exactly as signing does — the caller collects it on the keypad first.
 *
 * One tap yields both addresses: the Ethereum one from m/44'/60'/0'/0/0 and the
 * Tron one from m/44'/195'/0'/0/0. That is deliberate, and it is the fix for the
 * commonest way a terminal ends up half configured — somebody sets Ethereum, gets
 * interrupted, and the terminal then offers Tron payments to whatever address the
 * firmware was compiled with.
 *
 * Neither address is stored here. Both are *proposed*, through the same
 * accept-on-the-panel handshake a browser submission goes through, because "the
 * card said so" is not the same claim as "the operator checked it" — a card
 * presented by a customer would otherwise redirect the takings.
 *
 * @param[out] eth_out  EIP-55 "0x..." address, or "" if it could not be read.
 * @param[in]  eth_n    Capacity of @p eth_out.
 * @param[out] tron_out base58 "T..." address, or "".
 * @param[in]  tron_n   Capacity of @p tron_out.
 * @param[out] err      Short reason for the panel when nothing could be read.
 * @param[in]  err_n    Capacity of @p err.
 * @return true if at least one address was read.
 */
static bool card_read_payouts(CryptnoxWallet &wallet,
                              Pn532NfcTransport &transport,
                              CW_CryptoProvider &crypto,
                              const char *pin, size_t pin_chars,
                              char *eth_out, size_t eth_n,
                              char *tron_out, size_t tron_n,
                              char *err, size_t err_n)
{
    eth_out[0]  = '\0';
    tron_out[0] = '\0';

    CW_SecureSession session;
    /* setup = true: same 60-second cancellable poll, but the card-wait screen
     * rather than the transaction one. Nothing is being paid here. */
    if (!card_connect(wallet, transport, session, true)) {
        (void)snprintf(err, err_n, s_user_cancelled ? "Cancelled"
                                                    : "Card not found");
        return false;
    }

    if (!wallet.verifyPin(session, reinterpret_cast<const uint8_t *>(pin),
                          static_cast<uint8_t>(pin_chars))) {
        wallet.disconnect(session);
        (void)snprintf(err, err_n, "Wrong card PIN");
        return false;
    }

    uint8_t pubkey[64];
    WipeGuard g_pub(pubkey, sizeof(pubkey));

    /* Ethereum: keccak256 of the uncompressed public key, low 20 bytes. Rendered
     * checksummed so it reads exactly as the operator's own wallet shows it, which
     * is the entire point of putting it on the screen. */
    if (wallet.getPublicKey(session, ETH_DERIVE_PATH,
                            static_cast<uint8_t>(sizeof(ETH_DERIVE_PATH)),
                            pubkey)) {
        uint8_t h[32];
        keccak256(pubkey, sizeof(pubkey), h);
        (void)eth_addr_format(&h[12], eth_out, eth_n);
        CW_Utils::secure_wipe(h, sizeof(h));
    } else {
        ESP_LOGW(TAG, "card would not export its Ethereum key");
    }

    uint8_t addr21[CW_TRON_ADDRESS_BYTES];
    if (wallet.getPublicKey(session, CW_TRON_DERIVE_PATH,
                            CW_TRON_PATH_LENGTH, pubkey) &&
        CW_Tron::addressBytesFromPublicKey(pubkey, addr21)) {
        (void)CW_Tron::encodeAddress(addr21, crypto, tron_out, tron_n);
    } else {
        ESP_LOGW(TAG, "card would not export its Tron key");
    }

    wallet.disconnect(session);

    if ((eth_out[0] == '\0') && (tron_out[0] == '\0')) {
        (void)snprintf(err, err_n, "Card gave no usable address");
        return false;
    }
    ESP_LOGI(TAG, "card addresses: eth=%s tron=%s", eth_out, tron_out);
    return true;
}

/**
 * @brief Run the browser wizard until the operator presses Finish.
 *
 * The flow, and the reason it is shaped this way:
 *
 *   1. admin code   on the panel  — the caller does this before calling us. It is
 *                                   the one secret whose value is that it never
 *                                   crosses a network.
 *   2. QR code      on the panel  — a camera joins the SoftAP from it and the
 *                                   captive portal opens the page by itself.
 *   3. authorise    both          — the browser asks; the panel takes the code.
 *   4. addresses    in the browser
 *   5. Wi-Fi        in the browser
 *   6. Finish       on the panel  — restarts, which is what applies everything.
 *
 * @param[in] wifi_only Steps 5 and 6 only. For a terminal that is already configured
 *                      but whose saved network has gone: nothing is missing except a
 *                      password, so the addresses are not asked for again and neither
 *                      is the admin code — three screens standing between an operator
 *                      and a working till, guarding a form that can only propose
 *                      things the panel still has to accept. The AP passphrase, which
 *                      is on that panel, is the perimeter (see prov_set_wifi_only).
 * @return false if the portal could not be raised at all, in which case the caller
 *         has to fall back to the panel.
 */
static bool run_wizard(CryptnoxWallet &wallet, Pn532NfcTransport &transport,
                       CW_CryptoProvider &crypto, bool wifi_only)
{
    if (!prov_start(PROV_MODE_WIZARD, ui_event_dispatch)) {
        ESP_LOGE(TAG, "setup portal unavailable - falling back to the panel");
        return false;
    }

    /* Entering a step is two or three things, never one, and the Wi-Fi step's third
     * thing is the reason this is a function: the scan has to run on THIS task,
     * because it hops the radio across channels and briefly drops whoever is joined
     * to the SoftAP. Doing it once as the step opens is deliberate; doing it inside
     * an HTTP handler would drop the very browser asking. */
    auto enter_step = [](prov_step_t step) {
        prov_set_step(step);
        ui_show_prov(step);
        if (step == PROV_STEP_WIFI) {
            net_wifi_ap_t aps[16];
            prov_set_scan(aps, net_wifi_scan(aps, 16));
        }
    };

    /* A configured terminal that has only lost its network opens on the Wi-Fi step
     * with nothing to authorise: no admin code, no numbered steps, one screen. The
     * page still cannot store anything without the panel, and the AP passphrase it
     * was reached through is on that panel. */
    if (wifi_only) { prov_set_wifi_only(); }
    enter_step(wifi_only ? PROV_STEP_WIFI : PROV_STEP_AUTH);

    /* Held between the two halves of a card read: one tap yields both addresses,
     * but only one value at a time can be waiting on the panel, so the second is
     * parked here until the first is resolved either way. */
    char card_eth[SETTINGS_PAYOUT_MAX]  = "";
    char card_tron[SETTINGS_PAYOUT_MAX] = "";

    ui_msg_t msg;
    while (true) {
        if (xQueueReceive(s_ui_queue, &msg, portMAX_DELAY) != pdTRUE) { continue; }

        switch (msg.event) {
            case UI_EVENT_PROV_AUTH:
                /* A browser is asking to be let in. Demand the admin code on the
                 * panel; the UI task resolves it and prov_auth_resolve() reports
                 * the grant back here as PROV_NEXT, which is what moves the flow
                 * on. A refusal reports nothing — the browser can ask again. */
                ui_show_prov_auth();
                break;

            /* One event moves the flow on, whether it came from the browser's
             * Continue button or from prov_auth_resolve() letting it in. */
            case UI_EVENT_PROV_NEXT:
                if (!prov_authed()) { break; }
                if (prov_step() == PROV_STEP_AUTH) {
                    prov_set_note("");
                    enter_step(wifi_only ? PROV_STEP_WIFI : PROV_STEP_ADDR);
                } else if (prov_step() == PROV_STEP_ADDR) {
                    /* Refuse to leave the address step with nothing set. A terminal
                     * with no payout address of its own offers no assets at all
                     * (see the network picker), so letting this pass would end the
                     * wizard on a till that cannot take a payment. */
                    if (!settings_has_payout(false) && !settings_has_payout(true)) {
                        prov_set_note("Set at least one payout address first.");
                        break;
                    }
                    prov_set_note("");
                    enter_step(PROV_STEP_WIFI);
                } else {
                    /* Nowhere left to go, but the panel may still be showing the
                     * QR code: this is also how the Wi-Fi-only flow reports that a
                     * browser walked in, and that screen changes when it does. */
                    ui_show_prov(prov_step());
                }
                break;

            case UI_EVENT_PROV_SCAN: {
                net_wifi_ap_t aps[16];
                prov_set_scan(aps, net_wifi_scan(aps, 16));
                break;
            }

            case UI_EVENT_PROV_CARD:
                /* The page asked us to read the card. Collect the PIN first — the
                 * card will not export a key without it. */
                card_eth[0]  = '\0';
                card_tron[0] = '\0';
                s_user_cancelled = false;
                prov_set_note("Follow the terminal screen.");
                ui_show_card_pin();
                break;

            case UI_EVENT_CARD_PIN: {
                char   pin[16]   = { 0 };
                size_t pin_chars = ui_take_pin(pin, sizeof(pin));
                char   err[48]   = { 0 };
                const bool got = card_read_payouts(wallet, transport, crypto,
                                                   pin, pin_chars,
                                                   card_eth, sizeof(card_eth),
                                                   card_tron, sizeof(card_tron),
                                                   err, sizeof(err));
                CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(pin), sizeof(pin));

                ui_show_prov(prov_step());   /* back to the QR/step screen */
                if (!got) {
                    prov_set_note(err);
                    break;
                }
                prov_set_note("Accept each address on the terminal screen.");
                /* Ethereum first if there is one; the Tron one waits in card_tron
                 * and is offered when this proposal is resolved. */
                if (card_eth[0] != '\0') {
                    (void)prov_propose(PROV_ASK_PAYOUT_ETH, card_eth);
                    card_eth[0] = '\0';
                } else if (card_tron[0] != '\0') {
                    (void)prov_propose(PROV_ASK_PAYOUT_TRON, card_tron);
                    card_tron[0] = '\0';
                }
                break;
            }

            case UI_EVENT_PROV_VALUE:
                ui_show_prov_confirm();
                break;

            case UI_EVENT_PROV_VALUE_SET:
            case UI_EVENT_PROV_VALUE_NO:
                /* Accepted or refused, the panel slot is free again — so offer the
                 * second card-derived address if one is still parked. */
                if (card_tron[0] != '\0') {
                    (void)prov_propose(PROV_ASK_PAYOUT_TRON, card_tron);
                    card_tron[0] = '\0';
                }
                break;

            case UI_EVENT_WIFI_TRY: {
                if (!prov_authed()) { break; }
                char w_ssid[33] = { 0 };
                char w_pass[65] = { 0 };
                bool joined = false;
                if (ui_take_wifi_creds(w_ssid, sizeof(w_ssid),
                                       w_pass, sizeof(w_pass)) > 0U) {
                    ui_show_wifi_connecting(w_ssid);
                    /* Associating proves nothing — a captive-portal or offline AP
                     * joins fine. The clock is the proof, and it is also what TLS
                     * needs before the first RPC call. */
                    if (!net_wifi_connect(w_ssid, w_pass)) {
                        prov_set_note(NOTE_JOIN_FAILED);
                        /* It may still be half-associated: a hung DHCP times out
                         * in net_wifi_connect(), not in the driver, so a lease
                         * arriving after this point would put the setup forms on
                         * that LAN. Back to AP-only. */
                        net_wifi_disconnect();
                    } else if (!net_time_sync(15000U)) {
                        prov_set_note(NOTE_NO_TIME);
                        /* Joined, but we are not keeping it — and the portal stays
                         * up for another attempt. Its HTTP server binds every
                         * interface, so an association we are not using would leave
                         * the setup forms answering the whole venue LAN, which is
                         * the one place the AP passphrase guards nothing. Drop it. */
                        net_wifi_disconnect();
                    } else {
                        settings_set_wifi(w_ssid, w_pass);
                        joined = true;
                    }
                }
                CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(w_pass),
                                      sizeof(w_pass));
                if (joined) {
                    /* Done. The AP goes down with the portal, which is also what
                     * dropped the operator's phone the moment we joined their
                     * network — so the last word is on the panel, not in the
                     * browser. */
                    prov_stop();
                    ui_show_prov(PROV_STEP_DONE);
                } else {
                    /* Back on the setup AP alone — either the join failed and the
                     * radio never moved, or it worked without a clock and we just
                     * dropped it. Rescan on the way back: the list is a minute old
                     * and the network that would not join may simply have gone.
                     * A phone that was dragged off by the association rejoins the
                     * SoftAP by itself; the outcome is on the panel regardless. */
                    enter_step(PROV_STEP_WIFI);
                }
                break;
            }

            case UI_EVENT_PROV_FINISH:
                /* Restart to apply. The recipient and contract dual stores are
                 * built at boot from validated strings; rebuilding them in place
                 * would mean a second path into the money code that has to
                 * re-validate everything the boot path already validates. A reboot
                 * costs three seconds during setup and reuses the checks verbatim. */
                prov_stop();
                ESP_LOGW(TAG, "setup finished - restarting to apply it");
                ui_set_boot_status("Applying settings");
                vTaskDelay(pdMS_TO_TICKS(600));   /* let the screen land */
                esp_restart();
                break;

            default:
                break;
        }
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

    /* Before anything slow: the LED is lit from reset until this runs. */
    for (gpio_num_t p : { LED_R, LED_G, LED_B }) {
        (void)gpio_set_direction(p, GPIO_MODE_OUTPUT);
        (void)gpio_set_level(p, 1);
    }
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
    ui_refresh_addresses();
    /* No ui_show_splash() here — ui_init() already selects it, and asking twice
     * races the UI task into rebuilding the screen and replaying the logo. */

    /* Resolve the Ethereum recipient: operator-set if there is one, config.h
     * otherwise. A stored address gets a probe parse first, and a failure falls
     * back to the compile-time value rather than refusing the boot — the setup
     * page can only check an address structurally, and a terminal stuck on an
     * error screen is worse than one paying out to its configured default and
     * saying so. A bad config.h address is still fatal, as it always was: that
     * one is the integrator's own doing and there is nothing to fall back to. */
    if (settings_get_payout(false, s_payout_eth, sizeof(s_payout_eth)) &&
        !eth_addr_parse(s_payout_eth, s_dest.addr)) {
        ESP_LOGE(TAG, "stored Ethereum payout address rejected - using config.h");
        (void)snprintf(s_payout_eth, sizeof(s_payout_eth), "0x%s", ADDR_TO);
    }

    /* Parse the recipient twice into the dual store (§7.1). Also runs the EIP-55
     * checksum once at boot — a mistyped recipient is caught here, before any
     * payment. */
    if (!eth_addr_parse(s_payout_eth, s_dest.addr) ||
        !eth_addr_parse(s_payout_eth, s_dest.addr_echo)) {
        ESP_LOGE(TAG, "Bad ADDR_TO in config");
        ui_show_tx_status(UI_TX_STATE_FAILED, "Bad ADDR_TO in config");
        return;
    }

    /* Same shape for the ERC-20 contract, and for the same reason: it is settable
     * from the config page now, so a stored value gets a probe parse and falls back
     * to config.h on failure rather than stranding the terminal. */
    if (settings_get_contract(false, s_contract_eth, sizeof(s_contract_eth)) &&
        !eth_addr_parse(s_contract_eth, s_usdc.addr)) {
        ESP_LOGE(TAG, "stored ERC-20 contract rejected - using config.h");
        (void)snprintf(s_contract_eth, sizeof(s_contract_eth), "0x%s", ADDR_USDC);
    }
    if (!eth_addr_parse(s_contract_eth, s_usdc.addr) ||
        !eth_addr_parse(s_contract_eth, s_usdc.addr_echo)) {
        ESP_LOGE(TAG, "Bad ADDR_USDC in config");
        ui_show_tx_status(UI_TX_STATE_FAILED, "Bad ADDR_USDC in config");
        return;
    }
    /* Warn if the recipient carries no EIP-55 checksum (no upper-case hex
     * letter) — the boot-time typo check above is a no-op on an all-lowercase
     * address. Skip the "0x" so the 'x' is never read as hex. */
    bool addr_checksummed = false;
    for (const char *pc = s_payout_eth + 2; *pc != '\0'; ++pc) {
        if ((*pc >= 'A') && (*pc <= 'F')) { addr_checksummed = true; break; }
    }
    if (!addr_checksummed) {
        ESP_LOGW(TAG, "recipient is all-lowercase: no EIP-55 checksum verified");
    }

    /* ── PN532 NFC reader ──────────────────────────────────────── */
    ui_set_boot_status("Starting NFC reader");
    pn532_t nfc;
    CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(&nfc), sizeof(nfc));

    pn532_config_t nfc_cfg;
    CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(&nfc_cfg), sizeof(nfc_cfg));
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

    /* Tron recipient: base58check-decoded by the SDK, which validates the
     * checksum — so a mistyped address in config.h fails the boot instead of
     * sending TRX to a stranger. Decoded twice into the dual store (§7.1), each
     * pass independent, exactly as ADDR_TO is parsed twice above. */
    uint8_t tron_to21[CW_TRON_ADDRESS_BYTES];
    uint8_t tron_to21_echo[CW_TRON_ADDRESS_BYTES];
    /* Same fallback as the Ethereum recipient above, and this is where the
     * authoritative base58check on a stored Tron address happens: the setup page
     * only checks its length, prefix and alphabet, because the decoder needs a
     * crypto provider and that lives here. */
    if (settings_get_payout(true, s_payout_tron, sizeof(s_payout_tron)) &&
        !CW_Tron::decodeAddress(s_payout_tron, cryptoProvider, tron_to21)) {
        ESP_LOGE(TAG, "stored Tron payout address rejected - using config.h");
        (void)snprintf(s_payout_tron, sizeof(s_payout_tron), "%s", TRON_ADDR_TO);
    }
    if (!CW_Tron::decodeAddress(s_payout_tron, cryptoProvider, tron_to21) ||
        !CW_Tron::decodeAddress(s_payout_tron, cryptoProvider, tron_to21_echo)) {
        ESP_LOGE(TAG, "Bad TRON_ADDR_TO in config");
        ui_show_tx_status(UI_TX_STATE_FAILED, "Bad TRON_ADDR_TO in config");
        return;
    }
    (void)CW_Utils::safe_memcpy(s_tron_dest.addr, sizeof(s_tron_dest.addr),
                                &tron_to21[1], ETH_ADDR_LEN);
    (void)CW_Utils::safe_memcpy(s_tron_dest.addr_echo,
                                sizeof(s_tron_dest.addr_echo),
                                &tron_to21_echo[1], ETH_ADDR_LEN);
    /* The resolved address, not the literal — an operator reading the boot log to
     * find out where takings go must see the one that is actually in use. */
    ESP_LOGI(TAG, "Tron recipient: %s", s_payout_tron);

    /* TRC-20 contract, decoded the same way. Operator-settable now, so the stored
     * value is tried first and a failure falls back to config.h. Non-fatal on
     * purpose either way: an operator who never charges in tokens leaves the
     * placeholder in config.h, and the terminal must still boot — the asset is
     * simply refused if selected. */
    (void)settings_get_contract(true, s_contract_trx, sizeof(s_contract_trx));
    s_trc20_usdt.b58 = s_contract_trx;
    if (!trc20_load(&s_trc20_usdt, cryptoProvider)) {
        ESP_LOGW(TAG, "stored TRC-20 contract not usable - trying config.h");
        (void)snprintf(s_contract_trx, sizeof(s_contract_trx), "%s", TRON_ADDR_USDT);
    }
    if (!s_trc20_usdt.ok && !trc20_load(&s_trc20_usdt, cryptoProvider)) {
        ESP_LOGW(TAG, "TRON_ADDR_USDT not usable - USDT on Tron disabled");
    }

    /* Refuse to come up selling an asset whose payout address nobody set.
     *
     * The picker will not let an operator *switch* to such a network, but that is
     * not enough on its own: a terminal can already be sitting on one, which is
     * exactly what happens when somebody configures Ethereum, is interrupted before
     * Tron, and the stored chain is still Tron. The result looks entirely normal and
     * pays the compile-time recipient — somebody else's address. So the selection is
     * corrected here, at the one point that has both the stored chain and the
     * resolved addresses in hand. */
    if (!settings_has_payout(chain_is_tron())) {
        if (settings_has_payout(!chain_is_tron())) {
            const pos_chain_t to = chain_is_tron() ? POS_CHAIN_ETH_SEPOLIA
                                                   : POS_CHAIN_TRON_NILE;
            ESP_LOGW(TAG, "selected chain has no payout address - switching to %d",
                     static_cast<int>(to));
            settings_set_chain(to);
            ui_refresh_addresses();
        } else {
            /* Neither network is configured. Nothing to switch to, so the terminal
             * runs on its config.h recipient exactly as it always has — but the log
             * says so, and the Tx tab says so on screen. */
            ESP_LOGW(TAG, "no payout address configured - using the config.h "
                          "recipient; set one from the config page");
        }
    }
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
    tron_rpc_init(TRON_URL);
#if defined(RPC_PROJECT_ID) && defined(RPC_API_SECRET)
    eth_rpc_set_auth(RPC_PROJECT_ID, RPC_API_SECRET);
#endif
#ifdef RPC_CA_CERT_PEM
    /* pin the RPC endpoint's certificate instead of the CA bundle. */
    eth_rpc_set_ca_cert(RPC_CA_CERT_PEM);
#endif
#ifdef TRON_CA_CERT_PEM
    /* Same for the Tron endpoint. Optional because the node is not trusted on
     * this path either way — every transaction it serialises is re-derived and
     * compared before the card sees the hash (tron_tx.h) — but pinning means an
     * attacker has to beat both that check and TLS rather than just the one. */
    tron_rpc_set_ca_cert(TRON_CA_CERT_PEM);
#endif
    /* ── First run: greet, take the admin code, then hand over to the browser ──
     *
     * A missing admin code means a virgin or factory-reset terminal, since the
     * reset erases it too. Greet before the setup steps start asking for a
     * network and a code — it is the one moment we have the operator's attention
     * and nothing to demand of them yet.
     *
     * The admin code is the one step that stays on this panel. Not for tidiness:
     * its entire value is that it is never on a network, and it also has to exist
     * before anything else, because the Wi-Fi picker carries a back arrow that the
     * UI task honours on its own — which drops the operator on the amount screen,
     * burger included, while main is still blocked in setup. With no code stored
     * that burger opened the settings freely. Creating the code first closes that
     * window; the creation screen itself has no way out.
     *
     * run_wizard() does not return: it ends in a restart, which is what applies
     * the addresses. It only comes back if the portal could not be raised at all,
     * and then the panel picker below is the fallback. */
    const bool first_run = !settings_has_admin_code();
    if (first_run) {
        ui_show_welcome();
        wait_for_ui_event(UI_EVENT_WELCOME_DONE);

        ESP_LOGI(TAG, "no admin code - first-run setup");
        ui_show_admin_set();
        wait_for_ui_event(UI_EVENT_ADMIN_SET);

        (void)run_wizard(wallet, nfcTransport, cryptoProvider, false);
    }

    ui_set_boot_status("Starting network");
    net_wifi_init();

    /* Wi-Fi and a valid clock are one bring-up step, since TLS needs both: a
     * failed sync sends the operator back to setup with the reason rather than
     * stranding the terminal on an error screen.
     *
     * A terminal that is genuinely configured — a payout address, not merely an admin
     * code — and has only lost its saved network gets the wizard cut short to the
     * Wi-Fi step, with no admin code either: the addresses are set, so asking again
     * would be busywork standing between the operator and a working till. Typing a
     * venue passphrase on this panel is exactly what the browser flow exists to
     * avoid, so the panel picker is only reached when the SoftAP would not come up. */
    bool        try_saved  = true;
    /* first_run already ran the full wizard, so it has had its turn. */
    bool        offer_setup = !first_run;
    const char *net_note   = NULL;
    while (true) {
        if (!(try_saved && wifi_try_saved())) {
            /* No usable saved network. The browser flow first, once; then the panel
             * picker, which is also where a failed clock sync sends us. */
            if (offer_setup) {
                offer_setup = false;
                /* "Cut it short" is only honest for a terminal that really is
                 * configured. An admin code proves somebody started setup, not that
                 * they finished it — a terminal left with no payout address takes
                 * payments to the config.h recipient, and that one needs the whole
                 * wizard, addresses and admin code included, rather than a one-screen
                 * flow that never mentions either. */
                const bool configured = settings_has_payout(false) ||
                                        settings_has_payout(true);
                (void)run_wizard(wallet, nfcTransport, cryptoProvider, configured);
                /* Only reached if the SoftAP would not come up. */
                ESP_LOGW(TAG, "no setup portal - panel Wi-Fi picker");
            }
            /* Only blame the saved network if there was one — on a terminal that
             * has never been on a network this is the first screen, not a failure. */
            if ((net_note == NULL) && settings_has_wifi()) {
                net_note = "Could not join the saved network";
            }
            (void)wifi_picker(net_note);
            ui_show_splash();   /* the picker took the screen */
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

    /* Everything a terminal needs is now proven to work on this image: the
     * panel, the card reader, the wallet layer, the uplink and one authenticated
     * RPC round-trip. That is the bar for keeping a firmware update — before
     * this line, any reset would have sent the bootloader back to the previous
     * slot, which is exactly what should happen to a build that cannot get
     * here. See ota.h. */
    ota_mark_valid();

    /* ── Main interaction loop ────────────────────────────────── */
    ui_show_amount_entry();

    pos_amount_t pending_amount;
    pos_amount_set(&pending_amount, 0U);
    ui_msg_t msg;

    while (true) {
        if (xQueueReceive(s_ui_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (msg.event) {
            case UI_EVENT_AMOUNT_CONFIRMED: {
                pos_amount_set(&pending_amount, msg.payload);
                const trc20_asset_t *tok = active_trc20();
                /* Say so here rather than after the customer has tapped a card:
                 * a placeholder contract means this asset was never set up. */
                if ((tok != NULL) && !tok->ok) {
                    ui_show_tx_status(UI_TX_STATE_FAILED,
                                      "Token contract not configured");
                    pos_amount_set(&pending_amount, 0U);
                    break;
                }
                /* Reconcile amount + recipient (+ token contract) before they
                 * are shown to the customer — displayed value must equal what
                 * gets signed. */
                if (!IS_TRUE32(amount_consistent(&pending_amount)) ||
                    !IS_TRUE32(address_consistent(active_dest())) ||
                    ((tok != NULL) &&
                     !IS_TRUE32(address_consistent(&tok->addr)))) {
                    pos_handle_anomaly("pre-display reconcile");
                    ui_show_tx_status(UI_TX_STATE_FAILED, "Integrity check failed");
                    pos_amount_set(&pending_amount, 0U);
                    break;
                }
                ui_refresh_addresses();
                /* The resolved recipient, NOT the config.h literal: on a
                 * terminal provisioned through the setup page those differ, and
                 * this row is the one the operator is told to check an address
                 * against. Showing a payout address the transaction does not use
                 * would make the review screen worse than no review screen —
                 * s_payout_* is the string s_dest / s_tron_dest were parsed
                 * from, so what is displayed is what gets signed. */
                ui_show_confirm(pending_amount.amount_minor,
                                chain_is_tron() ? s_payout_tron : s_payout_eth);
                break;
            }

            case UI_EVENT_CONFIRM_CANCEL:
                ui_show_amount_entry();
                break;

            case UI_EVENT_PIN_ENTERED: {
                if (pending_amount.amount_minor == 0U) {
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
                /* Read the chain once per payment: the operator can switch it
                 * between sales, but never mid-sale. */
                const bool tron = chain_is_tron();
                bool ok = tron
                    ? sign_and_broadcast_tron(wallet, nfcTransport,
                                              cryptoProvider,
                                              &pending_amount, &s_tron_dest,
                                              active_trc20(),
                                              pin, pin_chars,
                                              tx_hash, sizeof(tx_hash),
                                              err_msg, sizeof(err_msg))
                    : sign_and_broadcast(wallet, nfcTransport,
                                              &pending_amount, &s_dest,
                                              pin, pin_chars,
                                              tx_hash, sizeof(tx_hash),
                                              err_msg, sizeof(err_msg));
                /* scrub our copy of the PIN as soon as signing is done. */
                CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(pin), sizeof(pin));

                /* Snapshot the decided amount for the final gate, then clear
                 * pending so the next sign needs a fresh New Payment flow. */
                pos_amount_t decided = pending_amount;
                pos_amount_set(&pending_amount, 0U);
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
                        rc = tron
                             ? tron_receipt_as_eth(tron_rpc_get_receipt(tx_hash))
                             : eth_rpc_get_tx_receipt(tx_hash);
                        if ((rc == ETH_RPC_RECEIPT_SUCCESS) ||
                            (rc == ETH_RPC_RECEIPT_REVERTED)) {
                            break;
                        }
                        /* PENDING or transient RPC error — try again. */
                        vTaskDelay(pdMS_TO_TICKS(4000));
                    }
                    /* §4: render PAID only if the monotonic gate holds — the
                     * on-chain APPROVED verdict AND amount/recipient still
                     * self-consistent through the decide→render window. */
                    pos_verdict_t verdict = (rc == ETH_RPC_RECEIPT_SUCCESS)
                                                ? POS_VERDICT_APPROVED
                                                : POS_VERDICT_DECLINED;
                    bool32 decision =
                        run_payment_decision(&decided, active_dest(), verdict);
                    if (rc == ETH_RPC_RECEIPT_SUCCESS && IS_TRUE32(decision)) {
                        ESP_LOGI(TAG, "Tx confirmed on-chain");
                        ui_show_tx_status(UI_TX_STATE_DONE, tx_hash);
                    } else if (rc == ETH_RPC_RECEIPT_SUCCESS) {
                        /* Mined OK but the integrity gate failed — never show
                         * PAID on a corrupted decision. */
                        pos_handle_anomaly("final decision gate");
                        ESP_LOGE(TAG, "Integrity gate failed post-receipt: %s", tx_hash);
                        ui_show_tx_status(UI_TX_STATE_FAILED, "Integrity check failed");
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

            case UI_EVENT_OTA_STAGED:
                /* A browser has uploaded firmware and the terminal has verified
                 * it; nothing boots until it is accepted on the panel. Routed
                 * through this queue rather than raised from the HTTP task, which
                 * also settles the mid-payment case for free: a payment occupies
                 * this loop, so the offer waits its turn behind it instead of
                 * appearing over a customer's transaction. */
                ui_show_ota_confirm();
                break;

            /* ── The admin page, open beside a running terminal ──
             * Same handlers as the wizard's, and for the same reason: a browser may
             * propose, only the panel may accept. Routed through this queue so an
             * offer waits behind a payment in progress rather than appearing over
             * a customer's transaction. */
            case UI_EVENT_PROV_AUTH:
                ui_show_prov_auth();
                break;

            case UI_EVENT_PROV_VALUE:
                ui_show_prov_confirm();
                break;

            case UI_EVENT_PROV_VALUE_SET:
                /* Stored. The recipient and contract dual stores are built at boot
                 * from validated strings, so the change takes effect through a
                 * restart — the same rule the wizard follows, and for the same
                 * reason: the alternative is a second path into the money code that
                 * has to re-validate everything the boot path already validates. */
                ESP_LOGW(TAG, "config changed from the admin page - restarting");
                prov_stop();
                ui_set_boot_status("Applying settings");
                vTaskDelay(pdMS_TO_TICKS(600));
                esp_restart();
                break;

            case UI_EVENT_PROV_CARD: {
                /* Deriving addresses from a card mid-shift is the same flow as in
                 * the wizard, so it reuses it: PIN on the keypad, tap, then the
                 * accept-on-the-panel handshake. */
                s_user_cancelled = false;
                prov_set_note("Follow the terminal screen.");
                ui_show_card_pin();
                break;
            }

            case UI_EVENT_CARD_PIN: {
                char   pin[16]   = { 0 };
                size_t pin_chars = ui_take_pin(pin, sizeof(pin));
                char   c_eth[SETTINGS_PAYOUT_MAX]  = "";
                char   c_tron[SETTINGS_PAYOUT_MAX] = "";
                char   err[48] = { 0 };
                const bool got = card_read_payouts(wallet, nfcTransport,
                                                   cryptoProvider,
                                                   pin, pin_chars,
                                                   c_eth, sizeof(c_eth),
                                                   c_tron, sizeof(c_tron),
                                                   err, sizeof(err));
                CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(pin), sizeof(pin));
                ui_show_amount_entry();
                if (!got) {
                    prov_set_note(err);
                } else {
                    /* One at a time here too. Unlike the wizard this loop does not
                     * park waiting for the second, because accepting the first
                     * restarts the terminal — so the operator taps the card again
                     * for the other network, which is a fair trade for not having
                     * two ways to store an address. */
                    prov_set_note("Accept the address on the terminal screen. "
                                  "Tap the card again for the other network.");
                    if (c_eth[0] != '\0') {
                        (void)prov_propose(PROV_ASK_PAYOUT_ETH, c_eth);
                    } else if (c_tron[0] != '\0') {
                        (void)prov_propose(PROV_ASK_PAYOUT_TRON, c_tron);
                    }
                }
                break;
            }

            case UI_EVENT_PROV_SCAN: {
                net_wifi_ap_t aps[16];
                prov_set_scan(aps, net_wifi_scan(aps, 16));
                break;
            }

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
                        /* Submitted from the config page, not the settings
                         * picker: the terminal is now on a network *and* serving
                         * that page, which is the one combination the portal must
                         * never be in — httpd binds every interface. The network
                         * was the thing being changed, so close the page rather
                         * than hand the forms to the new LAN. */
                        if (prov_mode() != PROV_MODE_OFF) { prov_stop(); }
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

                        /* The rollback restored the credentials the driver holds,
                         * which is what prov_stop() re-joins with — but the
                         * association itself cannot stay up while the config page
                         * is being served. Drop it and go back to AP-only. */
                        if (prov_mode() != PROV_MODE_OFF) { net_wifi_disconnect(); }

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
