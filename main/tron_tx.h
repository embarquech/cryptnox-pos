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
