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

/** @brief Which chain (and therefore which asset) the terminal charges in. */
typedef enum {
    POS_CHAIN_ETH_SEPOLIA = 0,  /**< USDC on Ethereum Sepolia (the default). */
    POS_CHAIN_TRON_NILE   = 1,  /**< Native TRX on the Tron Nile testnet.    */
} pos_chain_t;

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
 * @brief true once an admin code exists, i.e. first-run setup is done.
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

/** @brief Erase all stored settings (brightness, auto, Wi-Fi creds, fees). */
void settings_factory_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* SETTINGS_H */
