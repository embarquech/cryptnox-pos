/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file eth_addr.h
 * @ingroup eth
 * @brief Hex Ethereum-address parsing — a pure, dependency-free unit so it can
 *        be unit-tested and fuzzed on the host (see fuzz/fuzz_parse_address.cpp).
 */

#ifndef ETH_ADDR_H
#define ETH_ADDR_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Parse a 20-byte Ethereum address from a hex string.
 *
 * Accepts an optional @c 0x / @c 0X prefix, then requires exactly 40 hex
 * characters and rejects any non-hex input instead of silently decoding
 * garbage.
 *
 * @param[in]  hex Address string (with or without @c 0x prefix).
 * @param[out] out 20-byte decoded address; zeroed then left partially written
 *                 on failure — must not be used unless true is returned.
 * @return true on success, false on wrong length or non-hex character.
 */
bool eth_addr_parse(const char *hex, uint8_t out[20]);

/**
 * @brief Verify an address carries a valid EIP-55 mixed-case checksum.
 *
 * The capitalisation of an Ethereum address is a checksum over the address
 * itself: keccak256 of the lowercase hex, one nibble per character, upper-case
 * where that nibble is >= 8. It is the only typo detection the format offers,
 * and it catches roughly every realistic single-character slip.
 *
 * An all-lowercase address is @b rejected, not waved through: it carries no
 * checksum at all, and a caller asking this question wants the check, not a
 * best-effort. Callers that must accept unchecksummed input should say so by
 * using @ref eth_addr_parse alone.
 *
 * @param[in] hex Address string, with or without the @c 0x prefix.
 * @return true only for exactly 40 hex characters whose case matches the digest.
 */
bool eth_addr_eip55_ok(const char *hex);

#ifdef __cplusplus
}
#endif

#endif /* ETH_ADDR_H */
