/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file pay_sol.cpp
 * @ingroup app
 * @brief Solana: pre-flight balance, sign and broadcast a native SOL transfer.
 *
 * The sender is whichever card is tapped: its m/44'/501'/0'/0' Ed25519 key is
 * read over the secure channel, and the key IS the address. Ed25519 needs
 * applet 2.0; an older card refuses the export and the sale stops there.
 */

#include "pos_app.h"

/* m/44'/501'/0'/0' — the path Phantom and Solflare use, all hardened (Ed25519
 * derivation, SLIP-0010, has no unhardened children). */
extern const uint8_t SOL_DERIVE_PATH[16] = {   /* also read by card_read_payouts */
    0x80U, 0x00U, 0x00U, 0x2CU,
    0x80U, 0x00U, 0x01U, 0xF5U,
    0x80U, 0x00U, 0x00U, 0x00U,
    0x80U, 0x00U, 0x00U, 0x00U,
};

/** @brief Keypad units (6 decimals) to lamports (9). Cannot wrap: the keypad
 *         stops at 9999.99, ~10^13 lamports. */
static uint64_t units_to_lamports(uint64_t units)
{
    return units * 1000ULL;
}

/** @brief The amount, the recipient store and the message's recipient agree. */
static bool sol_reconciled(const pos_amount_t *amount, const uint8_t *msg)
{
    /* The recipient is the message's second account key (header 3 + count 1 +
     * the sender's 32): checked against the echo copy, which the message was
     * not built from. */
    return IS_TRUE32(amount_consistent(amount)) &&
           IS_TRUE32(address_consistent(&s_sol_dest)) &&
           ((msg == NULL) ||
            CW_Utils::secure_compare(&msg[4U + SOL_KEY_LEN], s_sol_dest.addr_echo,
                                     SOL_KEY_LEN));
}

/**
 * @brief Sign a native SOL transfer on the card and broadcast it.
 *
 * Same shape as @ref sign_and_broadcast_tron, but the message is built here
 * (sol_tx.h): the node contributes only the blockhash.
 *
 * @return BCAST_SENT, BCAST_UNKNOWN (no usable answer — poll until the
 *         blockhash expires), or BCAST_FAILED on refusal or user cancel.
 */
bcast_t sign_and_broadcast_sol(CryptnoxWallet &wallet, Pn532NfcTransport &transport,
                               const pos_amount_t *amount, const char *pin,
                               size_t pin_chars, inflight_t *fl,
                               char *err_out, size_t err_max)
{
    if (!sol_reconciled(amount, NULL)) {
        pos_handle_anomaly("pre-create reconcile (sol)");
        (void)snprintf(err_out, err_max, "Integrity check failed");
        return BCAST_FAILED;
    }
    const uint64_t lamports = units_to_lamports(amount->amount_minor);

    CW_SecureSession session;
    if (!card_connect(wallet, transport, session)) {
        if (!s_user_cancelled) {
            (void)snprintf(err_out, err_max, "%s",
                           (s_card_fault != NULL) ? s_card_fault : "Card not found");
        }
        return BCAST_FAILED;
    }
    if (!s_user_cancelled) {
        ui_show_tx_status(UI_TX_STATE_SIGNING, NULL);
    }

    if (!wallet.verifyPin(session, reinterpret_cast<const uint8_t *>(pin),
                          static_cast<uint8_t>(pin_chars))) {
        (void)snprintf(err_out, err_max, "%s", pin_fail_text(transport, "Wrong PIN"));
        wallet.disconnect(session);
        return BCAST_FAILED;
    }

    /* The sender. */
    uint8_t  from[SOL_KEY_LEN];
    uint16_t from_len = 0U;
    char     from_b58[SOL_ADDR_STR_MAX];
    if (!wallet.getPublicKey(session, CW_SIGN_DERIVE_ED25519, SOL_DERIVE_PATH,
                             static_cast<uint8_t>(sizeof(SOL_DERIVE_PATH)),
                             from, static_cast<uint16_t>(sizeof(from)), from_len) ||
        (from_len != SOL_KEY_LEN) ||
        !sol_b58_encode(from, sizeof(from), from_b58, sizeof(from_b58))) {
        /* A radio drop fails like a refusal; ask the card before closing. */
        (void)snprintf(err_out, err_max, "%s",
                       pin_fail_text(transport, "Card cannot sign Solana"));
        wallet.disconnect(session);
        return BCAST_FAILED;
    }
    ESP_LOGI(TAG, "Solana sender (this card): %s", from_b58);

    /* Before the signature: nothing is spent on a no. A read that fails is not
     * a refusal, as on the other networks — the preflight catches it anyway. */
    uint64_t have = 0U;
    if (!sol_rpc_get_balance(from_b58, &have)) {
        ESP_LOGW(TAG, "pre-flight: SOL balance read failed - letting it run");
    } else if (have < (lamports + SOL_BASE_FEE_LAMPORTS)) {
        wallet.disconnect(session);
        (void)snprintf(err_out, err_max, "Not enough SOL for amount + fee");
        return BCAST_FAILED;
    } else {
        /* funded */
    }

    uint8_t  blockhash[SOL_KEY_LEN];
    uint64_t valid_height = 0U;
    if (!sol_rpc_latest_blockhash(blockhash, &valid_height)) {
        wallet.disconnect(session);
        (void)snprintf(err_out, err_max, "Solana node not reachable");
        return BCAST_FAILED;
    }

    uint8_t msg[SOL_TRANSFER_MSG_LEN];
    if (sol_transfer_message(from, s_sol_dest.addr, blockhash, lamports,
                             msg, sizeof(msg)) != SOL_TRANSFER_MSG_LEN) {
        wallet.disconnect(session);
        (void)snprintf(err_out, err_max, "Card pays itself - use another card");
        return BCAST_FAILED;
    }

    /* Last reconcile before the irreversible signature — on the bytes signed. */
    if (!sol_reconciled(amount, msg)) {
        pos_handle_anomaly("pre-sign reconcile (sol)");
        (void)snprintf(err_out, err_max, "Integrity check failed");
        wallet.disconnect(session);
        return BCAST_FAILED;
    }

    uint8_t sig[SOL_SIG_LEN];
    WipeGuard g_sig(sig, sizeof(sig));
    if (!card_sign(wallet, session, CW_SIGN_DERIVE_ED25519, msg,
                   static_cast<uint8_t>(sizeof(msg)), SOL_DERIVE_PATH,
                   static_cast<uint8_t>(sizeof(SOL_DERIVE_PATH)),
                   pin, pin_chars, sig, err_out, err_max)) {
        return BCAST_FAILED;
    }

    if (!s_user_cancelled) {
        ui_show_tx_status(UI_TX_STATE_SENDING, NULL);
    }
    /* last cancel check right before the irreversible broadcast. */
    if (s_user_cancelled) {
        return BCAST_FAILED;
    }

    static_assert(sizeof(fl->hash) >= SOL_SIG_STR_MAX,
                  "inflight hash too small for a base58 Solana signature");
    uint8_t tx[SOL_TRANSFER_TX_LEN];
    if (sol_tx_wire(sig, msg, sizeof(msg), tx, sizeof(tx)) != sizeof(tx) ||
        !sol_b58_encode(sig, sizeof(sig), fl->hash, sizeof(fl->hash))) {
        (void)snprintf(err_out, err_max, "Transaction build failed");
        return BCAST_FAILED;
    }

    /* Before the broadcast: the signature is the transaction's id, fixed now,
     * and a reset between the send and its answer is what this record is for. */
    fl->sol              = true;
    fl->sol_valid_height = valid_height;
    inflight_persist(fl);

    switch (sol_rpc_send(tx, sizeof(tx), fl->hash, err_out, err_max)) {
        case SOL_SEND_OK:
            return BCAST_SENT;
        case SOL_SEND_REFUSED:
            return BCAST_FAILED;
        default:
            ESP_LOGW(TAG, "broadcast not confirmed - polling %s until it expires",
                     fl->hash);
            return BCAST_UNKNOWN;
    }
}

/** @brief Map a Solana status onto the Ethereum verdicts the UI flow uses. */
eth_rpc_receipt_result_t sol_status_as_eth(sol_status_t s)
{
    switch (s) {
        case SOL_STATUS_SUCCESS: return ETH_RPC_RECEIPT_SUCCESS;
        case SOL_STATUS_FAILED:  return ETH_RPC_RECEIPT_REVERTED;
        case SOL_STATUS_PENDING: return ETH_RPC_RECEIPT_PENDING;
        default:                 return ETH_RPC_RECEIPT_RPC_ERROR;
    }
}
