/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file sol_tx.h
 * @ingroup sol
 * @brief Solana: base58 addresses and the native SOL transfer, built here.
 *
 * Pure unit: no IDF, no networking, no globals — host-tested by
 * tests/units/test_sol_tx.cpp against bytes produced by solders.
 *
 * Unlike Tron, nothing about the transaction comes from the node except the
 * recent blockhash: the message is serialised here from the reconciled amount
 * and recipient, and the card signs exactly those bytes (Ed25519 is pure — the
 * card hashes the message itself). A hostile node can make the transaction
 * fail with a bad blockhash; it cannot change who is paid.
 */

#ifndef SOL_TX_H
#define SOL_TX_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SOL_KEY_LEN       32U   /**< public key, blockhash               */
#define SOL_SIG_LEN       64U   /**< Ed25519 R || S                      */
#define SOL_ADDR_STR_MAX  45U   /**< base58 of 32 bytes (<= 44) + NUL    */
#define SOL_SIG_STR_MAX   89U   /**< base58 of 64 bytes (<= 88) + NUL    */

/** @brief Bytes in a one-instruction System Program transfer message. */
#define SOL_TRANSFER_MSG_LEN  150U
/** @brief The signed transaction: one signature, then the message. */
#define SOL_TRANSFER_TX_LEN   (1U + SOL_SIG_LEN + SOL_TRANSFER_MSG_LEN)

/** @brief Lamports per signature — the whole fee of a native transfer. */
#define SOL_BASE_FEE_LAMPORTS  5000ULL

/**
 * @brief Base58-encode @p n bytes (Bitcoin alphabet, leading zeros as '1').
 * @return false if @p out is too small; @p out is then "".
 */
bool sol_b58_encode(const uint8_t *in, size_t n, char *out, size_t out_n);

/**
 * @brief Decode a base58 Solana address into its 32 bytes.
 *
 * Strict: the string must be the canonical encoding of exactly 32 bytes (it is
 * re-encoded and compared), so two spellings can never name one account.
 * Solana addresses carry no checksum; this is the whole check there is.
 */
bool sol_addr_decode(const char *s, uint8_t out[SOL_KEY_LEN]);

/**
 * @brief Serialise a legacy message moving @p lamports from @p from to @p to,
 *        @p from paying the fee.
 *
 * Accounts [from (signer, writable), to (writable), System Program (readonly)],
 * one instruction: SystemInstruction::Transfer (index 2, u64 LE lamports).
 *
 * @return #SOL_TRANSFER_MSG_LEN, or 0 if @p out is too small, @p lamports is 0
 *         or @p from equals @p to (the runtime refuses a duplicated account).
 */
size_t sol_transfer_message(const uint8_t from[SOL_KEY_LEN],
                            const uint8_t to[SOL_KEY_LEN],
                            const uint8_t blockhash[SOL_KEY_LEN],
                            uint64_t lamports, uint8_t *out, size_t out_n);

/**
 * @brief The wire transaction: compact-u16 count (1), @p sig, @p msg.
 * @return bytes written, or 0 if @p out is too small.
 */
size_t sol_tx_wire(const uint8_t sig[SOL_SIG_LEN], const uint8_t *msg,
                   size_t msg_len, uint8_t *out, size_t out_n);

#ifdef __cplusplus
}
#endif

#endif /* SOL_TX_H */
