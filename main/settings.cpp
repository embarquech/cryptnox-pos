/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file settings.cpp
 * @brief NVS-backed implementation of the persistent device settings.
 */

#include "settings.h"

#include <string.h>
#include "nvs.h"
#include "esp_log.h"
#include "esp_random.h"

#include "CW_Utils.h"   /* secure_wipe / secure_compare (CODING_RULES §1.4) */

extern "C" {
#include "keccak256.h"
}

#include "config.h"   /* MAX_FEE / MAX_PRIORITY_FEE — compile-time fee defaults */

static const char *const TAG = "settings";

#define NS_SETTINGS   "settings"
#define K_BRIGHTNESS  "bright"
#define K_AUTO_BL     "auto_bl"
#define K_WIFI_SSID   "wifi_ssid"
#define K_WIFI_PASS   "wifi_pass"
#define K_MAX_FEE     "max_fee_gw"
#define K_PRIO_FEE    "prio_fee_gw"
#define K_ADMIN_SALT  "adm_salt"
#define K_ADMIN_HASH  "adm_hash"
#define K_ADMIN_FAILS "adm_fails"

#define ADMIN_SALT_LEN    16U
#define ADMIN_HASH_LEN    32U
#define ADMIN_CODE_MAX    32U

#define DEFAULT_BRIGHTNESS  80U

/* config.h carries the fees in wei; the UI works in Gwei. */
#define WEI_PER_GWEI               1000000000ULL
#define DEFAULT_MAX_FEE_GWEI       (uint32_t)(MAX_FEE / WEI_PER_GWEI)
#define DEFAULT_PRIORITY_FEE_GWEI  (uint32_t)(MAX_PRIORITY_FEE / WEI_PER_GWEI)

uint8_t settings_get_brightness(void)
{
    uint8_t val = DEFAULT_BRIGHTNESS;
    nvs_handle_t h;
    if (nvs_open(NS_SETTINGS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t stored;
        if (nvs_get_u8(h, K_BRIGHTNESS, &stored) == ESP_OK) {
            val = stored;
        }
        nvs_close(h);
    }
    return val;
}

void settings_set_brightness(uint8_t pct)
{
    if (pct > 100U) { pct = 100U; }
    nvs_handle_t h;
    if (nvs_open(NS_SETTINGS, NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_set_u8(h, K_BRIGHTNESS, pct);
        (void)nvs_commit(h);
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "brightness: nvs_open failed");
    }
}

bool settings_get_auto_brightness(void)
{
    uint8_t val = 0U;
    nvs_handle_t h;
    if (nvs_open(NS_SETTINGS, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u8(h, K_AUTO_BL, &val);
        nvs_close(h);
    }
    return (val != 0U);
}

void settings_set_auto_brightness(bool on)
{
    nvs_handle_t h;
    if (nvs_open(NS_SETTINGS, NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_set_u8(h, K_AUTO_BL, on ? 1U : 0U);
        (void)nvs_commit(h);
        nvs_close(h);
    }
}

bool settings_has_wifi(void)
{
    bool present = false;
    nvs_handle_t h;
    if (nvs_open(NS_SETTINGS, NVS_READONLY, &h) == ESP_OK) {
        char ssid[33] = {0};
        size_t len = sizeof(ssid);
        present = (nvs_get_str(h, K_WIFI_SSID, ssid, &len) == ESP_OK) &&
                  (ssid[0] != '\0');
        nvs_close(h);
    }
    return present;
}

bool settings_get_wifi(char *ssid, size_t ssid_n, char *pass, size_t pass_n)
{
    if ((ssid == NULL) || (pass == NULL) || (ssid_n == 0U) || (pass_n == 0U)) {
        return false;
    }
    ssid[0] = '\0';
    pass[0] = '\0';

    bool ok = false;
    nvs_handle_t h;
    if (nvs_open(NS_SETTINGS, NVS_READONLY, &h) == ESP_OK) {
        size_t ls = ssid_n;
        size_t lp = pass_n;
        if ((nvs_get_str(h, K_WIFI_SSID, ssid, &ls) == ESP_OK) &&
            (nvs_get_str(h, K_WIFI_PASS, pass, &lp) == ESP_OK) &&
            (ssid[0] != '\0')) {
            ok = true;
        } else {
            ssid[0] = '\0';
            pass[0] = '\0';
        }
        nvs_close(h);
    }
    return ok;
}

void settings_set_wifi(const char *ssid, const char *pass)
{
    if ((ssid == NULL) || (pass == NULL)) { return; }
    nvs_handle_t h;
    if (nvs_open(NS_SETTINGS, NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_set_str(h, K_WIFI_SSID, ssid);
        (void)nvs_set_str(h, K_WIFI_PASS, pass);
        (void)nvs_commit(h);
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "wifi: nvs_open failed");
    }
}

static uint32_t fee_get(const char *key, uint32_t def)
{
    uint32_t val = def;
    nvs_handle_t h;
    if (nvs_open(NS_SETTINGS, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u32(h, key, &val);
        nvs_close(h);
    }
    return val;
}

static void fee_set(const char *key, uint32_t gwei)
{
    nvs_handle_t h;
    if (nvs_open(NS_SETTINGS, NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_set_u32(h, key, gwei);
        (void)nvs_commit(h);
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "fee %s: nvs_open failed", key);
    }
}

uint32_t settings_get_max_fee_gwei(void)
{
    return fee_get(K_MAX_FEE, DEFAULT_MAX_FEE_GWEI);
}

void settings_set_max_fee_gwei(uint32_t gwei)
{
    fee_set(K_MAX_FEE, gwei);
}

uint32_t settings_get_priority_fee_gwei(void)
{
    return fee_get(K_PRIO_FEE, DEFAULT_PRIORITY_FEE_GWEI);
}

void settings_set_priority_fee_gwei(uint32_t gwei)
{
    fee_set(K_PRIO_FEE, gwei);
}

/**
 * @brief Derive the stored digest: a single keccak256 over salt || code.
 *
 * Not stretched, on purpose. The digest lives in the flash-encrypted NVS, so
 * reading it already means the encryption is defeated — and past that point no
 * KDF cost saves a 4-digit code anyway. The salt is still there so the same code
 * yields a different digest on every unit. Guessing at the panel is what the
 * escalating lockout in ui.cpp is for.
 */
static void admin_derive(const char *code, const uint8_t *salt,
                         uint8_t out[ADMIN_HASH_LEN])
{
    uint8_t buf[ADMIN_SALT_LEN + ADMIN_CODE_MAX];
    const size_t clen = strnlen(code, ADMIN_CODE_MAX);

    (void)memcpy(buf, salt, ADMIN_SALT_LEN);
    (void)memcpy(buf + ADMIN_SALT_LEN, code, clen);
    keccak256(buf, ADMIN_SALT_LEN + clen, out);
    CW_Utils::secure_wipe(buf, sizeof(buf));
}

static void admin_set_fails(uint8_t n)
{
    nvs_handle_t h;
    if (nvs_open(NS_SETTINGS, NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_set_u8(h, K_ADMIN_FAILS, n);
        (void)nvs_commit(h);
        nvs_close(h);
    }
}

bool settings_has_admin_code(void)
{
    bool present = false;
    nvs_handle_t h;
    if (nvs_open(NS_SETTINGS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = 0U;
        present = (nvs_get_blob(h, K_ADMIN_HASH, NULL, &len) == ESP_OK) &&
                  (len == ADMIN_HASH_LEN);
        nvs_close(h);
    }
    return present;
}

void settings_set_admin_code(const char *code)
{
    if (code == NULL) { return; }

    uint8_t salt[ADMIN_SALT_LEN];
    esp_fill_random(salt, sizeof(salt));

    uint8_t hash[ADMIN_HASH_LEN];
    admin_derive(code, salt, hash);

    nvs_handle_t h;
    if (nvs_open(NS_SETTINGS, NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_set_blob(h, K_ADMIN_SALT, salt, sizeof(salt));
        (void)nvs_set_blob(h, K_ADMIN_HASH, hash, sizeof(hash));
        (void)nvs_set_u8(h, K_ADMIN_FAILS, 0U);
        (void)nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "admin code set");
    } else {
        ESP_LOGW(TAG, "admin code: nvs_open failed");
    }
    CW_Utils::secure_wipe(hash, sizeof(hash));
}

bool settings_check_admin_code(const char *code)
{
    if (code == NULL) { return false; }

    uint8_t salt[ADMIN_SALT_LEN];
    uint8_t stored[ADMIN_HASH_LEN];
    bool    have = false;

    nvs_handle_t h;
    if (nvs_open(NS_SETTINGS, NVS_READONLY, &h) == ESP_OK) {
        size_t ls = sizeof(salt);
        size_t lh = sizeof(stored);
        have = (nvs_get_blob(h, K_ADMIN_SALT, salt, &ls) == ESP_OK) &&
               (nvs_get_blob(h, K_ADMIN_HASH, stored, &lh) == ESP_OK) &&
               (ls == ADMIN_SALT_LEN) && (lh == ADMIN_HASH_LEN);
        nvs_close(h);
    }
    if (!have) { return false; }   /* no code stored — nothing to match */

    uint8_t calc[ADMIN_HASH_LEN];
    admin_derive(code, salt, calc);
    const bool ok = CW_Utils::secure_compare(calc, stored, ADMIN_HASH_LEN);
    CW_Utils::secure_wipe(calc, sizeof(calc));

    if (ok) {
        if (settings_admin_fail_count() != 0U) { admin_set_fails(0U); }
    } else {
        const uint8_t n = settings_admin_fail_count();
        admin_set_fails((n < 255U) ? (uint8_t)(n + 1U) : 255U);
        ESP_LOGW(TAG, "admin unlock failed (%u consecutive)", (unsigned)(n + 1U));
    }
    return ok;
}

uint8_t settings_admin_fail_count(void)
{
    uint8_t n = 0U;
    nvs_handle_t h;
    if (nvs_open(NS_SETTINGS, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u8(h, K_ADMIN_FAILS, &n);
        nvs_close(h);
    }
    return n;
}

void settings_factory_reset(void)
{
    nvs_handle_t h;
    if (nvs_open(NS_SETTINGS, NVS_READWRITE, &h) == ESP_OK) {
        /* Drops brightness, auto flag, Wi-Fi creds and the admin code — so a
         * reset terminal comes back up into first-run setup for both. */
        (void)nvs_erase_all(h);
        (void)nvs_commit(h);
        nvs_close(h);
        ESP_LOGW(TAG, "settings: factory reset");
    }
}
