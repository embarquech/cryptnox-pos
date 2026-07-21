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
 * garbage. A mixed-case input is treated as EIP-55 checksummed and its
 * checksum is verified (keccak256 recomputed, rejected on case mismatch); an
 * all-lower/all-upper input is accepted as un-checksummed.
 *
 * @param[in]  hex Address string (with or without @c 0x prefix).
 * @param[out] out 20-byte decoded address; zeroed then left partially written
 *                 on failure — must not be used unless true is returned.
 * @return true on success, false on wrong length, non-hex character, or a
 *         failed EIP-55 checksum.
 */
bool eth_addr_parse(const char *hex, uint8_t out[20]);

#ifdef __cplusplus
}
#endif

#endif /* ETH_ADDR_H */
