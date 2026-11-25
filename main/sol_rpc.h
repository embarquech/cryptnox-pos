/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file sol_rpc.h
 * @ingroup sol
 * @brief Solana JSON-RPC client: balance, blockhash, broadcast, status.
 *
 * The node supplies one input to the transaction, the recent blockhash, and a
 * wrong one only makes the transaction fail (sol_tx.h). Status is asked by
 * signature, which only our transaction carries, so a node cannot answer for
 * somebody else's transfer either.
 */

#ifndef SOL_RPC_H
#define SOL_RPC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Outcome of a broadcast. */
typedef enum {
    SOL_SEND_OK,        /**< The node took it and echoed our signature.        */
    SOL_SEND_REFUSED,   /**< The node said no (preflight): nothing went out.   */
    SOL_SEND_UNKNOWN,   /**< No usable answer: it may or may not be out there. */
} sol_send_t;

/** @brief Outcome of one status poll. */
typedef enum {
    SOL_STATUS_PENDING,    /**< Not seen, or only "processed" — poll again.    */
    SOL_STATUS_SUCCESS,    /**< Confirmed with no error — final.               */
    SOL_STATUS_FAILED,     /**< Confirmed with an error — nothing moved.       */
    SOL_STATUS_RPC_ERROR,  /**< Transport or parse error (transient).          */
} sol_status_t;

/** @brief Endpoint URL. Stored as-is: pass static storage. */
void sol_rpc_init(const char *url);

/** @brief Optional TLS pin (PEM); NULL keeps the CA bundle. Stored as-is. */
void sol_rpc_set_ca_cert(const char *ca_pem);

/** @brief Balance of a base58 address, in lamports. false on any failure. */
bool sol_rpc_get_balance(const char *addr_b58, uint64_t *lamports);

/**
 * @brief A recent blockhash at "confirmed", and the block height past which a
 *        transaction carrying it can never land.
 */
bool sol_rpc_latest_blockhash(uint8_t blockhash[32], uint64_t *last_valid_height);

/** @brief Current block height at "confirmed". */
bool sol_rpc_block_height(uint64_t *height);

/**
 * @brief sendTransaction, base64, with preflight.
 *
 * @param[in]  tx      Wire transaction.
 * @param[in]  sig_b58 Its signature: the answer must echo exactly this.
 * @param[out] err     Operator-facing reason on SOL_SEND_REFUSED.
 */
sol_send_t sol_rpc_send(const uint8_t *tx, size_t tx_len, const char *sig_b58,
                        char *err, size_t err_n);

/** @brief getSignatureStatuses for one signature. */
sol_status_t sol_rpc_get_status(const char *sig_b58);

#ifdef __cplusplus
}
#endif

#endif /* SOL_RPC_H */
