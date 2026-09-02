/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file settings.h
 * @ingroup device
 * @brief Persistent device settings stored in NVS (backlight, Wi-Fi creds).
 *
 * All getters are safe to call before any value has ever been written — they
 * return sensible defaults. Requires nvs_flash_init() to have run first.
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Which chain (and therefore which asset) the terminal charges in.
 *
 * The numbers are persisted in NVS, so existing ones never move — a terminal
 * that stored 2 before this list grew must still come up charging in USDT on
 * Tron. New assets are appended, and settings.cpp rejects anything at or past
 * @ref POS_CHAIN__COUNT, so a downgrade falls back to the default rather than
 * charging in whatever an unknown number would have meant.
 */
typedef enum {
    POS_CHAIN_ETH_SEPOLIA = 0,  /**< USDC on Ethereum Sepolia (the default). */
    POS_CHAIN_TRON_NILE   = 1,  /**< Native TRX on the Tron Nile testnet.    */
    POS_CHAIN_TRON_USDT   = 2,  /**< USDT (TRC-20) on Tron Nile.             */
    POS_CHAIN_ETH_USDT    = 3,  /**< USDT (ERC-20) on Ethereum Sepolia.      */
    POS_CHAIN_POLY_USDC   = 4,  /**< USDC (ERC-20) on Polygon Amoy.          */
    POS_CHAIN_POLY_USDT   = 5,  /**< USDT (ERC-20) on Polygon Amoy.          */
    POS_CHAIN_TRON_USDC   = 6,  /**< USDC (TRC-20) on Tron Nile.             */
    /* USDC on Tron is a testnet-only selection: Circle stopped minting it on
     * Tron in Feb 2024 and closed redemptions a year later, so there is no
     * mainnet asset for it to graduate to. The TRC-20 path is generic, so it
     * costs a contract in config.h and nothing else. */
    POS_CHAIN_ETH_NATIVE  = 7,  /**< Native ETH on Ethereum Sepolia.         */
    POS_CHAIN_POLY_NATIVE = 8,  /**< Native POL on Polygon Amoy.             */
    POS_CHAIN__COUNT            /**< Sentinel — keep last, not a selection.  */
} pos_chain_t;

/**
 * @brief true for the Tron chains; every other selection is on an EVM network.
 *
 * Here rather than once per file. While Sepolia was the only Ethereum chain,
 * "not Sepolia" meant Tron, and main.cpp and ui.cpp each said so in their own
 * one-liner. Polygon made that wrong in two places at once, which is exactly the
 * shape of bug that sends an Ethereum payment down the Tron path — so the
 * question is asked in one place and tested in tests/units/test_chain.cpp.
 */
static inline bool pos_chain_is_tron(pos_chain_t c) {
    return (c == POS_CHAIN_TRON_NILE) ||
           (c == POS_CHAIN_TRON_USDT) ||
           (c == POS_CHAIN_TRON_USDC);
}

/**
 * @brief true for the Polygon chains.
 *
 * Polygon is EVM, so it shares the whole Ethereum signing path — the RLP, the
 * derivation path, the payout address, the card. What differs is the endpoint,
 * the chain id in the signed transaction and the token contract, which is why
 * this is a question of its own rather than a second meaning for "not Tron".
 */
static inline bool pos_chain_is_polygon(pos_chain_t c) {
    return (c == POS_CHAIN_POLY_USDC) ||
           (c == POS_CHAIN_POLY_USDT) ||
           (c == POS_CHAIN_POLY_NATIVE);
}

/**
 * @brief true for the EVM networks' own coins — ETH and POL — not their tokens.
 *
 * A different transaction, not a different network: no contract is called, the
 * recipient goes in @c to instead of the token's address, the amount goes in
 * @c value instead of the calldata, and 21000 gas is enough. It is also the only
 * pair of selections carrying 18 decimals rather than 6, which is why
 * @ref POS_AMOUNT_UNITS_MAX_NATIVE exists.
 *
 * Native TRX is deliberately not in here: Tron is a wholly separate signing
 * path, and asking "is this a native coin" on the Ethereum side of the fork is
 * the only place the question means anything.
 */
static inline bool pos_chain_is_native_evm(pos_chain_t c) {
    return (c == POS_CHAIN_ETH_NATIVE) || (c == POS_CHAIN_POLY_NATIVE);
}

/**
 * @brief Ceiling on a native-coin sale, in the keypad's 6-decimal base units.
 *
 * ETH and POL are 18-decimal, so wei = units * 10^12, and eth_tx_t::eth_value is
 * a uint64 — 2^64-1 wei is 18.446744073709551615 of the coin. Past that the
 * multiply wraps and the card would sign a value nobody entered, so the keypad
 * stops at 18.44 and the payment path re-checks it.
 *
 * ponytail: uint64 wei, 18.44 ETH/POL a sale. Widening means carrying
 * eth_value as a 32-byte big-endian buffer through eth_rlp and its two
 * encoders — worth it only if somebody actually needs to charge more.
 */
#define POS_AMOUNT_UNITS_MAX_NATIVE  18446744ULL

/** @brief Selected chain, or @ref POS_CHAIN_ETH_SEPOLIA if never set. */
pos_chain_t settings_get_chain(void);

/** @brief Persist the selected chain. */
void settings_set_chain(pos_chain_t chain);

/** @brief Backlight level in percent, or 80 if never set. */
uint8_t settings_get_brightness(void);

/** @brief Persist the backlight level (0..100). */
void settings_set_brightness(uint8_t pct);

/** @brief true if a Wi-Fi SSID has been stored. */
bool settings_has_wifi(void);

/**
 * @brief Read the stored Wi-Fi credentials.
 *
 * @param[out] ssid     Buffer for the SSID (>= 33 bytes recommended).
 * @param[in]  ssid_n   Capacity of @p ssid.
 * @param[out] pass     Buffer for the password (>= 65 bytes recommended).
 * @param[in]  pass_n   Capacity of @p pass.
 * @return true if both SSID and password were present, false otherwise
 *         (buffers are left NUL-terminated/empty on failure).
 */
bool settings_get_wifi(char *ssid, size_t ssid_n, char *pass, size_t pass_n);

/** @brief Persist Wi-Fi credentials (plaintext — see README threat model). */
void settings_set_wifi(const char *ssid, const char *pass);

/**
 * @brief true once an admin code exists.
 *
 * NOT the same as "configured": a terminal whose setup was interrupted has a code
 * and no payout address, and that one is not a till (see the boot sequence in
 * main.cpp, which runs setup on either being missing). Use
 * @ref settings_has_payout for "can this unit take money".
 *
 * Cleared by @ref settings_factory_reset, so a reset device asks for a new code.
 */
bool settings_has_admin_code(void);

/**
 * @brief Store a new admin code, with a fresh random salt.
 *
 * The code is never persisted — only a salted keccak256 digest of it,
 * deliberately not stretched (see settings.cpp). Resets the failure counter.
 *
 * @param[in] code NUL-terminated code; the caller wipes its own copy.
 * @return false if NVS refused the write — the caller must not treat setup as
 *         done, since a terminal with no stored code can never open its menu.
 */
bool settings_set_admin_code(const char *code);

/**
 * @brief Check a candidate code, maintaining the failure counter.
 *
 * @param[in] code NUL-terminated candidate.
 * @return true on match (counter cleared); false on mismatch or when no code is
 *         stored (counter incremented on a genuine mismatch).
 */
bool settings_check_admin_code(const char *code);

/**
 * @brief Consecutive failed unlock attempts, persisted.
 *
 * Kept in NVS so power-cycling does not clear the penalty the UI derives from it.
 */
uint8_t settings_admin_fail_count(void);

/**
 * @brief EIP-1559 max fee per gas, in Gwei.
 * @return the stored override, or the config.h compile-time default (MAX_FEE)
 *         if the user has never changed it.
 */
uint32_t settings_get_max_fee_gwei(void);

/** @brief Persist the max fee per gas (Gwei). */
void settings_set_max_fee_gwei(uint32_t gwei);

/**
 * @brief EIP-1559 max priority fee (tip) per gas, in Gwei.
 * @return the stored override, or the config.h default (MAX_PRIORITY_FEE).
 */
uint32_t settings_get_priority_fee_gwei(void);

/** @brief Persist the max priority fee per gas (Gwei). */
void settings_set_priority_fee_gwei(uint32_t gwei);

/** @brief Longest payout address plus NUL — "0x" + 40 hex, or 34 base58 Tron. */
#define SETTINGS_PAYOUT_MAX  64U

/**
 * @brief Read the payout address for a network.
 *
 * Falls back to the config.h compile-time recipient when nothing is stored, so
 * every caller gets a parseable address and none of them has to carry a
 * not-configured branch. It is not a licence to spend to it: the payment path
 * checks @ref settings_has_payout first and refuses the sale (main.cpp,
 * UI_EVENT_AMOUNT_CONFIRMED), because an address nobody chose is somebody
 * else's. The fallback is what the Tx tab displays and what the boot-time chain
 * correction reasons about, not what a transaction pays.
 *
 * Stored twice and compared here, which is the same dual-store rule main.cpp
 * applies when it parses the recipient (§7.1). A compile-time address lives in
 * the signed app image; an NVS one does not, so it carries its own echo copy and
 * a mismatch falls back to the config.h value rather than paying out to a
 * half-written string.
 *
 * @param[in]  tron  true for the Tron payout address, false for Ethereum.
 * @param[out] out   Buffer, >= @ref SETTINGS_PAYOUT_MAX. Ethereum addresses are
 *                   returned "0x"-prefixed, ready to parse.
 * @param[in]  n     Capacity of @p out.
 * @return true if a stored (operator-set) address was returned, false if the
 *         config.h default was used. Either way @p out is valid.
 */
bool settings_get_payout(bool tron, char *out, size_t n);

/**
 * @brief Whether an operator has actually set the payout address for a network.
 *
 * The difference between this and @ref settings_get_payout's return value is only
 * that this one does not need a buffer. It exists because it decides whether an
 * asset is offered at all: a terminal that was set up for Ethereum and never got
 * as far as Tron must not quietly offer Tron payments to the compile-time
 * recipient, which is somebody else's address.
 */
bool settings_has_payout(bool tron);

/**
 * @brief Persist a payout address, writing both the value and its echo copy.
 *
 * Does not validate: the caller checks the EIP-55 / base58 checksum first and
 * shows the address on the device screen for the operator to accept.
 *
 * @param[in] tron true for the Tron address, false for Ethereum.
 * @param[in] addr NUL-terminated address; Ethereum with or without "0x".
 * @return false if NVS refused either write (nothing is left half-applied
 *         that a later read would trust — the echo comparison catches it).
 */
bool settings_set_payout(bool tron, const char *addr);

/**
 * @brief Read the token-contract address for a network.
 *
 * Which token the terminal charges in, as opposed to who gets paid. Same
 * dual-store and same config.h fallback as @ref settings_get_payout, for the same
 * reason: the contract decides which asset moves, so a half-written string in NVS
 * must not be able to point the terminal at a different token.
 *
 * There is one settable contract per network — USDC on Ethereum, USDT on Tron.
 * Native TRX has none, and asking for one on a chain that has no token is the
 * caller's mistake; @p tron selects the network, not the chain.
 *
 * The other assets (USDT on Ethereum, both on Polygon, USDC on Tron) are named
 * by config.h alone and have no NVS slot — see erc20_token_t in main.cpp for
 * why that is deliberate rather than an omission.
 *
 * @param[in]  tron true for the Tron TRC-20 contract, false for the ERC-20 one.
 * @param[out] out  Buffer, >= @ref SETTINGS_PAYOUT_MAX. Ethereum contracts are
 *                  returned "0x"-prefixed.
 * @param[in]  n    Capacity of @p out.
 * @return true if a stored (operator-set) contract was returned, false if the
 *         config.h default was used. Either way @p out is valid.
 */
bool settings_get_contract(bool tron, char *out, size_t n);

/**
 * @brief Persist a token-contract address, value and echo copy.
 *
 * Does not validate — same contract as @ref settings_set_payout: the caller
 * checks the address and has it accepted on the device screen first.
 */
bool settings_set_contract(bool tron, const char *addr);

/** @brief Erase all stored settings (brightness, auto, Wi-Fi creds, fees). */
void settings_factory_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* SETTINGS_H */
