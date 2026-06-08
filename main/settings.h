/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file settings.h
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

/** @brief Backlight level in percent, or 80 if never set. */
uint8_t settings_get_brightness(void);

/** @brief Persist the backlight level (0..100). */
void settings_set_brightness(uint8_t pct);

/** @brief true if automatic (light-sensor) brightness is enabled (default false). */
bool settings_get_auto_brightness(void);

/** @brief Persist the automatic-brightness flag. */
void settings_set_auto_brightness(bool on);

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
