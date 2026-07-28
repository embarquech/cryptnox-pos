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
#include <stdbool.h>   /* eth_addr_parse returns bool — keep this header self-contained */

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Length of a raw (binary) Ethereum address, in bytes. */
#define ETH_ADDR_LEN     20U
/**
 * @brief Length of the same address in hex characters (no @c 0x prefix).
 *
 * Two hex chars per byte. The multiply is performed in @c size_t because every
 * use site is a @c size_t context (@c strlen result, array bound, loop bound):
 * multiplying in @c unsigned @c int and widening at the use site trips
 * bugprone-implicit-widening-of-multiplication-result on a 64-bit host.
 */
#define ETH_ADDR_HEX_LEN ((size_t)ETH_ADDR_LEN * 2U)

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
bool eth_addr_parse(const char *hex, uint8_t out[ETH_ADDR_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* ETH_ADDR_H */
