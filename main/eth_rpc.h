/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file eth_rpc.h
 * @ingroup eth
 * @brief Ethereum JSON-RPC client over HTTPS (nonce / ecrecover parity /
 *        raw-tx broadcast / receipt polling).  Network bring-up (Wi-Fi,
 *        SNTP) lives in net.h.
 */

#ifndef ETH_RPC_H
#define ETH_RPC_H

/******************************************************************
 * 1. Included files
 ******************************************************************/

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/******************************************************************
 * 2. Types
 ******************************************************************/

/** @brief Result of the ecrecover parity probe (failures are explicit). */
typedef enum {
    ETH_RPC_PARITY_OK = 0,      /**< *v_out is valid (0 or 1)                    */
    ETH_RPC_PARITY_MISMATCH,    /**< RPC answered but neither parity recovered
                                     from_addr — wrong ADDR_FROM or wrong card   */
    ETH_RPC_PARITY_RPC_ERROR,   /**< no usable RPC response for either parity    */
} eth_rpc_parity_result_t;

/** @brief Outcome of one eth_getTransactionReceipt poll. */
typedef enum {
    ETH_RPC_RECEIPT_PENDING,   /**< result is null — not mined yet            */
    ETH_RPC_RECEIPT_SUCCESS,   /**< mined with status 0x1 — payment final     */
    ETH_RPC_RECEIPT_REVERTED,  /**< mined with status 0x0 — execution failed  */
    ETH_RPC_RECEIPT_RPC_ERROR, /**< transport or parse error (transient)      */
} eth_rpc_receipt_result_t;

/******************************************************************
 * 3. Public API
 ******************************************************************/

/**
 * @brief Set the RPC URL and the from-address used for nonce queries and
 *        ecrecover comparison.
 *
 * Must be called before any other eth_rpc_* function.
 *
 * Lifetime: the module stores the pointers as-is (no copy).  Both strings
 * must outlive every eth_rpc_* call — pass string literals or static
 * storage, never stack buffers.
 *
 * @param[in] rpc_url   HTTPS JSON-RPC endpoint URL.
 * @param[in] from_addr "0x..."-prefixed 40-hex-char sender address.
 */
void eth_rpc_init(const char *rpc_url, const char *from_addr);

/**
 * @brief Replace the from-address — the account a sale spends from.
 *
 * The payer is the card on the reader, so it is not known until somebody taps.
 * @ref eth_rpc_init's @p from_addr is only the boot-time default (the config.h
 * literal, used for the startup reachability probe); this is the per-tap
 * override, and everything that reads the sender — the nonce, the balance, the
 * ecrecover comparison — follows it.
 *
 * Unlike every other setter in this header the string is **copied**, because
 * its caller derives it into a stack buffer inside one sale.
 *
 * @param[in] addr "0x"-prefixed, 40 hex characters. A malformed one is refused
 *                 rather than silently leaving the previous payer in force —
 *                 the next nonce would otherwise be somebody else's.
 * @return true if the address was accepted and is now in force.
 */
bool eth_rpc_set_from(const char *addr);

/**
 * @brief Optional: set Infura-style HTTP Basic Auth credentials.
 *
 * Same lifetime contract as eth_rpc_init: pointers are stored, not copied.
 *
 * @param[in] project_id Username (Infura project ID); NULL/empty disables auth.
 * @param[in] api_secret Password (Infura API secret); NULL/empty disables auth.
 */
void eth_rpc_set_auth(const char *project_id, const char *api_secret);

/**
 * @brief Optional: pin the RPC endpoint's TLS certificate.
 *
 * When set, the HTTPS connection is validated **only** against this PEM
 * (leaf or its issuing CA) instead of the full Mozilla CA bundle, so no
 * unrelated CA can MITM the RPC traffic. Pointer stored as-is (must outlive
 * every call — pass a static/embedded literal). NULL keeps the CA bundle.
 *
 * @param[in] ca_pem NUL-terminated PEM certificate, or NULL for the bundle.
 */
void eth_rpc_set_ca_cert(const char *ca_pem);

/**
 * @brief Fetch the pending transaction count (nonce) for from_addr.
 *
 * Responses with an HTTP status other than 200, malformed JSON, or a nonce
 * above 2^32-1 are rejected.
 *
 * @param[out] nonce_out Nonce on success; untouched on failure.
 * @return true on success, false on transport, parse or range error.
 */
bool eth_rpc_get_nonce(uint64_t *nonce_out);

/**
 * @brief Fetch the native balance of from_addr, in wei.
 *
 * For the pre-flight check that refuses a sale the payer cannot fund before the
 * customer is asked for anything — see @c evm_balance_ok in main.cpp. Without
 * it the first news of an empty account is the node's refusal *after* the PIN,
 * the tap and the signature.
 *
 * Saturating at @c UINT64_MAX (see @ref eth_json_hex_quantity): 20 ETH does not
 * fit a uint64 of wei, and over-reporting can only fail to refuse.
 *
 * @param[out] wei_out Balance on success; untouched on failure.
 * @return true on success, false on transport or parse error.
 */
bool eth_rpc_get_balance(uint64_t *wei_out);

/**
 * @brief Fetch from_addr's balance of an ERC-20, via @c balanceOf over eth_call.
 *
 * The token half of the same check, and the one that saves more: a transfer of
 * more tokens than the account holds is not refused by the node at all. It is
 * broadcast, mined, reverted, and charged for — so the customer waits through
 * the whole confirmation only to be declined, and pays the gas for the
 * privilege.
 *
 * Saturating, like @ref eth_rpc_get_balance.
 *
 * @param[in]  token_addr "0x..."-prefixed contract address to call.
 * @param[out] units_out  Balance in the token's base units on success;
 *                        untouched on failure.
 * @return true on success, false on transport or parse error, or if the
 *         configured from_addr is not a 20-byte hex address.
 */
bool eth_rpc_get_token_balance(const char *token_addr, uint64_t *units_out);

/**
 * @brief Determine the signature parity bit (v = 0 or 1).
 *
 * Calls the ecrecover precompile (address 0x01) via eth_call for both
 * parities and matches the recovered address against from_addr.
 *
 * @param[in]  hash  32-byte message hash that was signed.
 * @param[in]  r     32-byte big-endian ECDSA r component.
 * @param[in]  s     32-byte big-endian ECDSA s component.
 * @param[out] v_out Parity (0 or 1) on @ref ETH_RPC_PARITY_OK; untouched on
 *                   failure.
 * @retval ETH_RPC_PARITY_OK        *v_out is valid.
 * @retval ETH_RPC_PARITY_MISMATCH  The RPC answered but neither parity
 *                                  recovers from_addr.
 * @retval ETH_RPC_PARITY_RPC_ERROR No usable RPC response for either parity.
 */
eth_rpc_parity_result_t eth_rpc_ecrecover_parity(const uint8_t hash[32],
                                                 const uint8_t r[32],
                                                 const uint8_t s[32],
                                                 uint8_t *v_out);

/**
 * @brief Broadcast a raw signed transaction (type-prefixed RLP bytes).
 *
 * @param[in]  tx          Signed transaction bytes.
 * @param[in]  tx_len      Length of @p tx in bytes.
 * @param[out] tx_hash_out "0x..."-prefixed tx hash on success; must be at
 *                         least 68 bytes (2 + 64 + NUL).
 * @param[in]  tx_hash_max Capacity of @p tx_hash_out.
 * @param[out] err_out     On failure, the node's own @c error.message when it
 *                         sent one ("insufficient funds for gas * price +
 *                         value", "nonce too low", …), truncated to fit;
 *                         set to "" when the failure was a transport or parse
 *                         error with no message to report. May be NULL.
 * @param[in]  err_max     Capacity of @p err_out.
 * @return true on success, false on transport error, JSON-RPC error
 *         response, or undersized @p tx_hash_out.
 */
bool eth_rpc_send_raw_tx(const uint8_t *tx, size_t tx_len,
                          char *tx_hash_out, size_t tx_hash_max,
                          char *err_out, size_t err_max);

/**
 * @brief Poll the receipt of a broadcast transaction (one shot).
 *
 * Calls eth_getTransactionReceipt.  A broadcast acceptance only means the tx
 * entered the mempool — a POS must wait for the mined receipt (status 0x1)
 * before declaring the payment approved.
 *
 * @param[in] tx_hash "0x..."-prefixed transaction hash from
 *                    @ref eth_rpc_send_raw_tx.
 * @retval ETH_RPC_RECEIPT_PENDING   Not mined yet — poll again later.
 * @retval ETH_RPC_RECEIPT_SUCCESS   Mined, execution succeeded.
 * @retval ETH_RPC_RECEIPT_REVERTED  Mined but reverted — funds NOT moved.
 * @retval ETH_RPC_RECEIPT_RPC_ERROR Transport/parse error (may be transient).
 */
eth_rpc_receipt_result_t eth_rpc_get_tx_receipt(const char *tx_hash);

#ifdef __cplusplus
}
#endif

#endif // ETH_RPC_H
