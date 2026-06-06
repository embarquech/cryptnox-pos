/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file eth_rpc.h
 * @brief WiFi bring-up, SNTP time sync and Ethereum JSON-RPC client
 *        (nonce / ecrecover parity / raw-tx broadcast) over HTTPS.
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

/** @brief Result of the ecrecover parity probe (F-10: failures are explicit). */
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

/** @brief A scanned access point (subset of fields the UI needs). */
typedef struct {
    char   ssid[33];   /**< NUL-terminated SSID (max 32 chars + NUL). */
    int8_t rssi;       /**< Signal strength, dBm (closer to 0 = stronger). */
    bool   open;       /**< true if the network is open (no password). */
} eth_wifi_ap_t;

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
 * storage, never stack buffers (F-19).
 *
 * @param[in] rpc_url   HTTPS JSON-RPC endpoint URL.
 * @param[in] from_addr "0x..."-prefixed 40-hex-char sender address.
 */
void eth_rpc_init(const char *rpc_url, const char *from_addr);

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
 * @brief Bring up the WiFi driver in station mode (idempotent).
 *
 * Initialises netif, the event loop and the WiFi driver and starts the STA.
 * Call once before scanning or connecting. Safe to call repeatedly.
 */
void eth_rpc_wifi_init(void);

/**
 * @brief Scan for nearby access points (blocking).
 *
 * @param[out] out  Array to fill with de-duplicated APs.
 * @param[in]  max  Capacity of @p out.
 * @return number of APs written (0 on error or none found).
 */
uint16_t eth_rpc_wifi_scan(eth_wifi_ap_t *out, uint16_t max);

/**
 * @brief Connect to a WiFi network and block until an IP is obtained
 *        (up to 30 s). May be called repeatedly to switch networks.
 *
 * @param[in] ssid     Network SSID.
 * @param[in] password Passphrase (empty string for an open network).
 * @return true on success, false on timeout or repeated association failure.
 */
bool eth_rpc_wifi_connect(const char *ssid, const char *password);

/**
 * @brief Read the RSSI of the currently associated access point.
 *
 * @param[out] rssi_out Signal strength in dBm (closer to 0 = stronger);
 *                      untouched when not associated.
 * @return true if associated and @p rssi_out was written, false otherwise.
 */
bool eth_rpc_wifi_rssi(int8_t *rssi_out);

/**
 * @brief Block until the system clock has been set via SNTP.
 *
 * Must be called after eth_rpc_wifi_connect() and before any HTTPS request:
 * without real time, TLS certificate validity-period checks are meaningless
 * (F-06).  SNTP keeps running in the background for periodic resyncs.
 *
 * @param[in] timeout_ms Maximum time to wait for the first sync.
 * @return true once the first sync completes, false on init error or timeout.
 */
bool eth_rpc_time_sync(uint32_t timeout_ms);

/**
 * @brief Fetch the pending transaction count (nonce) for from_addr.
 *
 * Responses with an HTTP status other than 200, malformed JSON, or a nonce
 * above 2^32-1 are rejected (F-09).
 *
 * @param[out] nonce_out Nonce on success; untouched on failure.
 * @return true on success, false on transport, parse or range error.
 */
bool eth_rpc_get_nonce(uint64_t *nonce_out);

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
 *                                  recovers from_addr (F-10).
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
 * @return true on success, false on transport error, JSON-RPC error
 *         response, or undersized @p tx_hash_out.
 */
bool eth_rpc_send_raw_tx(const uint8_t *tx, size_t tx_len,
                          char *tx_hash_out, size_t tx_hash_max);

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
