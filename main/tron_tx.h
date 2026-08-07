/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file tron_tx.h
 * @ingroup tron
 * @brief Tron transaction hex helpers — protobuf varints, the TransferContract
 *        integrity check and the signed-Transaction envelope.
 *
 * Pure unit: string/hex work only, no IDF, no networking, no globals — so it
 * builds and self-checks on the host (see tests/units/test_tron_tx.cpp), the
 * same pattern as eth_json.cpp and civil_time.cpp.
 *
 * Why a check at all: a Tron terminal does not serialise its own transaction,
 * it asks a full node to (@c /wallet/createtransaction) and signs the txID the
 * node hands back. That makes the node a trust boundary — it could return a
 * transaction paying somebody else. @ref tron_tx_contract_ok re-derives the
 * exact bytes a TransferContract for (owner, to, amount) must contain and
 * refuses anything else, so a hostile or buggy node cannot redirect funds.
 */

#ifndef TRON_TX_H
#define TRON_TX_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Hex length of a Tron address (21 bytes: 0x41 || 20-byte key hash). */
#define TRON_ADDR_HEX_LEN  42U

/** @brief Hex length of a Tron signature (r || s || v). */
#define TRON_SIG_HEX_LEN   130U

/** @brief TRC-20 @c transfer(address,uint256) selector, hex (same as ERC-20). */
#define TRON_TRC20_SELECTOR       "a9059cbb"

/** @brief Hex length of the two ABI argument words that follow the selector. */
#define TRON_TRC20_PARAM_HEX_LEN  128U

/**
 * @brief Hex-encode a value as a protobuf base-128 varint.
 *
 * @param[out] out      Destination, NUL-terminated on success (>= 21 bytes is
 *                      always enough).
 * @param[in]  out_size Capacity of @p out.
 * @param[in]  v        Value to encode.
 * @return number of hex characters written (excluding the NUL), 0 if @p out is
 *         NULL or too small.
 */
size_t tron_varint_hex(uint64_t v, char *out, size_t out_size);

/**
 * @brief Verify that a node-serialised raw_data really transfers what we asked.
 *
 * Searches @p raw_data_hex (case-insensitively) for the serialised
 * @c TransferContract fields — owner_address (field 1), to_address (field 2)
 * and amount (field 3) — as one contiguous byte run. Fails closed: any
 * re-ordering, any different address, any different amount, and the caller must
 * decline the payment rather than sign it.
 *
 * @param[in] raw_data_hex Hex of the node's raw_data protobuf.
 * @param[in] owner_hex    Sender, #TRON_ADDR_HEX_LEN hex chars, "41"-prefixed.
 * @param[in] to_hex       Recipient, same form.
 * @param[in] amount_sun   Amount in sun (1 TRX = 1e6 sun).
 * @return true only if the expected contract bytes are present.
 */
bool tron_tx_contract_ok(const char *raw_data_hex, const char *owner_hex,
                         const char *to_hex, uint64_t amount_sun);

/**
 * @brief ABI-encode the arguments of @c transfer(address,uint256), hex.
 *
 * Two 32-byte words, no selector: the recipient left-padded (its @c 0x41 Tron
 * prefix dropped — the ABI address word holds the bare 20-byte key hash) and
 * the amount big-endian. This is exactly what the HTTP API's @c parameter field
 * wants, and exactly what must reappear inside the node's raw_data.
 *
 * @param[in]  to_hex   Recipient, #TRON_ADDR_HEX_LEN hex chars, "41"-prefixed.
 * @param[in]  amount   Token amount in base units.
 * @param[out] out      Destination, NUL-terminated on success.
 * @param[in]  out_size Capacity of @p out (>= #TRON_TRC20_PARAM_HEX_LEN + 1).
 * @return #TRON_TRC20_PARAM_HEX_LEN on success, 0 on bad input or overflow.
 */
size_t tron_trc20_param_hex(const char *to_hex, uint64_t amount,
                            char *out, size_t out_size);

/**
 * @brief Verify that a node-serialised raw_data really is the TRC-20 transfer
 *        we asked for.
 *
 * The TRX sibling of this check is @ref tron_tx_contract_ok; a token transfer
 * is a @c TriggerSmartContract instead, so three things must match rather than
 * two — and one of them, the contract address, decides *which asset* moves:
 *
 * @code 0a15 owner | 1215 contract | 2244 a9059cbb | to-word | amount-word @endcode
 *
 * Also checks the @c fee_limit (raw_data field 18, tag @c 9001) separately: it
 * sits outside the contract run, and it caps the TRX a failed call may burn, so
 * a node echoing a cap larger than the operator agreed to must be refused.
 *
 * @param[in] raw_data_hex  Hex of the node's raw_data protobuf.
 * @param[in] owner_hex     Sender, #TRON_ADDR_HEX_LEN hex chars, "41"-prefixed.
 * @param[in] contract_hex  Token contract, same form.
 * @param[in] to_hex        Recipient, same form.
 * @param[in] amount        Token amount in base units; must be non-zero.
 * @param[in] fee_limit_sun Fee cap in sun, as requested; must be non-zero.
 * @return true only if every expected byte run is present.
 */
bool tron_tx_trc20_ok(const char *raw_data_hex, const char *owner_hex,
                      const char *contract_hex, const char *to_hex,
                      uint64_t amount, uint64_t fee_limit_sun);

/**
 * @brief Build the hex of a signed @c Transaction protobuf for broadcasthex.
 *
 * @code Transaction { 1: raw_data (bytes) , 2: signature (repeated bytes) } @endcode
 *
 * @param[in]  raw_data_hex Hex of the raw_data protobuf (even length).
 * @param[in]  sig_hex      Signature hex, exactly #TRON_SIG_HEX_LEN chars.
 * @param[out] out          Destination, NUL-terminated on success.
 * @param[in]  out_size     Capacity of @p out.
 * @return number of hex characters written, 0 on bad input or overflow.
 */
size_t tron_tx_envelope_hex(const char *raw_data_hex, const char *sig_hex,
                            char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* TRON_TX_H */
