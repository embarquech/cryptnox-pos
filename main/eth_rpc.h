#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Set the RPC URL and the from-address used for nonce queries and
 * ecrecover comparison.  Must be called before any other eth_rpc_* function.
 * from_addr must be a "0x..."-prefixed 40-hex-char string.
 */
void eth_rpc_init(const char *rpc_url, const char *from_addr);

/*
 * Connect to a WiFi network and block until an IP is obtained (up to 30 s).
 * Returns true on success.
 */
bool eth_rpc_wifi_connect(const char *ssid, const char *password);

/*
 * Fetch the pending transaction count (nonce) for from_addr.
 * Returns true and writes the nonce into *nonce_out on success.
 */
bool eth_rpc_get_nonce(uint64_t *nonce_out);

/*
 * Determine the signature parity bit (v = 0 or 1) by calling the ecrecover
 * precompile (address 0x01) via eth_call and matching the recovered address
 * against from_addr.  Returns 0 or 1; defaults to 0 on RPC failure.
 */
uint8_t eth_rpc_ecrecover_parity(const uint8_t hash[32],
                                  const uint8_t r[32],
                                  const uint8_t s[32]);

/*
 * Broadcast a raw signed transaction (type-prefixed RLP bytes).
 * On success writes the "0x..."-prefixed tx hash into tx_hash_out and
 * returns true.  tx_hash_out must be at least 68 bytes (2 + 64 + NUL).
 */
bool eth_rpc_send_raw_tx(const uint8_t *tx, size_t tx_len,
                          char *tx_hash_out, size_t tx_hash_max);

#ifdef __cplusplus
}
#endif
