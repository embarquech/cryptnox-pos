/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file settings.cpp
 * @brief NVS-backed implementation of the persistent device settings.
 */

#include "settings.h"

#include <stdio.h>    /* snprintf — payout address normalisation */
#include <string.h>
#include "nvs.h"
#include "esp_log.h"
#include "esp_random.h"

#include "CW_Utils.h"   /* secure_wipe / secure_compare (CODING_RULES §1.4) */

extern "C" {
#include "keccak256.h"
}

#include "config.h"   /* MAX_FEE / MAX_PRIORITY_FEE — compile-time fee defaults */

/* Same guard main.cpp carries: Tron TRC-20 support post-dates the first config.h
 * files in the field, so an absent contract compiles to an empty string that
 * fails its decode and disables the asset, rather than breaking the build. */
#ifndef TRON_ADDR_USDT
#define TRON_ADDR_USDT  ""
#endif

static const char *const TAG = "settings";

#define NS_SETTINGS   "settings"
#define K_BRIGHTNESS  "bright"
#define K_WIFI_SSID   "wifi_ssid"
#define K_WIFI_PASS   "wifi_pass"
#define K_MAX_FEE     "max_fee_gw"
#define K_PRIO_FEE    "prio_fee_gw"
#define K_ADMIN_SALT  "adm_salt"
#define K_ADMIN_HASH  "adm_hash"
#define K_ADMIN_FAILS "adm_fails"
#define K_CHAIN       "chain"
/* Payout addresses, each stored twice — see settings_get_payout. */
#define K_PAY_ETH     "pay_eth"
#define K_PAY_ETH2    "pay_eth_e"
#define K_PAY_TRX     "pay_trx"
#define K_PAY_TRX2    "pay_trx_e"
/* Token contracts, same treatment — see settings_get_contract. */
#define K_CT_ETH      "ct_eth"
#define K_CT_ETH2     "ct_eth_e"
#define K_CT_TRX      "ct_trx"
#define K_CT_TRX2     "ct_trx_e"

#define ADMIN_SALT_LEN    16U
#define ADMIN_HASH_LEN    32U
/* Longest code that goes into the digest. Deliberately NOT ui.cpp's
 * ADMIN_CODE_MAX (9) — same name, different layer. Anything past this is
 * silently dropped from the hash, so keep it comfortably above the UI's cap. */
#define ADMIN_CODE_HASH_MAX  32U

#define DEFAULT_BRIGHTNESS  80U

/* config.h carries the fees in wei; the UI works in Gwei. */
#define WEI_PER_GWEI               1000000000ULL
#define DEFAULT_MAX_FEE_GWEI       (uint32_t)(MAX_FEE / WEI_PER_GWEI)
#define DEFAULT_PRIORITY_FEE_GWEI  (uint32_t)(MAX_PRIORITY_FEE / WEI_PER_GWEI)

pos_chain_t settings_get_chain(void)
{
    pos_chain_t chain = POS_CHAIN_ETH_SEPOLIA;
    nvs_handle_t h;
    if (nvs_open(NS_SETTINGS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t stored = 0U;
        /* Unknown value = a downgrade or a corrupt cell; fall back to the
         * default rather than charge on a chain no code path can handle. */
        if ((nvs_get_u8(h, K_CHAIN, &stored) == ESP_OK) &&
            (stored < (uint8_t)POS_CHAIN__COUNT)) {
            chain = (pos_chain_t)stored;
        }
        nvs_close(h);
    }
    return chain;
}

void settings_set_chain(pos_chain_t chain)
{
    nvs_handle_t h;
    if (nvs_open(NS_SETTINGS, NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_set_u8(h, K_CHAIN, (uint8_t)chain);
        (void)nvs_commit(h);
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "chain: nvs_open failed");
    }
}

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
    uint8_t buf[ADMIN_SALT_LEN + ADMIN_CODE_HASH_MAX];
    const size_t clen = strnlen(code, ADMIN_CODE_HASH_MAX);

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

bool settings_set_admin_code(const char *code)
{
    if (code == NULL) { return false; }

    uint8_t salt[ADMIN_SALT_LEN];
    esp_fill_random(salt, sizeof(salt));

    uint8_t hash[ADMIN_HASH_LEN];
    admin_derive(code, salt, hash);

    /* Reported rather than swallowed: the menu — factory reset included — is
     * unreachable without a stored code, so a silent write failure would leave
     * a terminal only a USB erase can rescue. */
    bool ok = false;
    nvs_handle_t h;
    if (nvs_open(NS_SETTINGS, NVS_READWRITE, &h) == ESP_OK) {
        ok = (nvs_set_blob(h, K_ADMIN_SALT, salt, sizeof(salt)) == ESP_OK) &&
             (nvs_set_blob(h, K_ADMIN_HASH, hash, sizeof(hash)) == ESP_OK) &&
             (nvs_set_u8(h, K_ADMIN_FAILS, 0U) == ESP_OK) &&
             (nvs_commit(h) == ESP_OK);
        nvs_close(h);
        ESP_LOGI(TAG, "admin code set: %s", ok ? "ok" : "FAILED");
    } else {
        ESP_LOGW(TAG, "admin code: nvs_open failed");
    }
    CW_Utils::secure_wipe(hash, sizeof(hash));
    return ok;
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

/* Money-carrying addresses — the payout recipient and the token contract. Two
 * keys each: the value and an echo copy, read back and compared, so a torn write
 * or a flipped bit in NVS cannot silently redirect a payment or point the terminal
 * at a different asset. See the settings.h contract for why the compile-time
 * address does not need this and a stored one does. */

/**
 * @brief Read a dual-stored address, falling back to @p def on any doubt.
 *
 * @param[in]  k_val  NVS key holding the value.
 * @param[in]  k_echo NVS key holding its echo copy.
 * @param[in]  def    Compile-time fallback, already in the returned form.
 * @param[in]  what   Label for the log line when the copies disagree.
 * @return true if the stored pair agreed and was returned.
 */
static bool dual_get(const char *k_val, const char *k_echo, const char *def,
                     const char *what, char *out, size_t n)
{
    if ((out == NULL) || (n == 0U)) { return false; }
    out[0] = '\0';

    char val[SETTINGS_PAYOUT_MAX]  = { 0 };
    char echo[SETTINGS_PAYOUT_MAX] = { 0 };
    bool stored = false;

    nvs_handle_t h;
    if (nvs_open(NS_SETTINGS, NVS_READONLY, &h) == ESP_OK) {
        size_t lv = sizeof(val);
        size_t le = sizeof(echo);
        stored = (nvs_get_str(h, k_val, val, &lv) == ESP_OK) &&
                 (nvs_get_str(h, k_echo, echo, &le) == ESP_OK);
        nvs_close(h);
    }

    /* secure_compare, not strcmp: this decides where money goes, so the
     * comparison must not leak on length or short-circuit on the first byte. */
    if (stored && !CW_Utils::secure_compare(reinterpret_cast<const uint8_t *>(val),
                                            reinterpret_cast<const uint8_t *>(echo),
                                            sizeof(val))) {
        ESP_LOGE(TAG, "%s: stored copies disagree - using config.h", what);
        stored = false;
    }

    (void)snprintf(out, n, "%s", stored ? val : def);
    CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(val), sizeof(val));
    CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(echo), sizeof(echo));
    return stored;
}

/** @brief Write a dual-stored address, normalising Ethereum to "0x"-prefixed. */
static bool dual_set(const char *k_val, const char *k_echo, bool tron,
                     const char *what, const char *addr)
{
    if ((addr == NULL) || (addr[0] == '\0')) { return false; }
    if (strlen(addr) >= SETTINGS_PAYOUT_MAX) { return false; }

    /* Normalise to the form the getter hands back, so the echo comparison
     * compares like with like on the next boot. */
    char norm[SETTINGS_PAYOUT_MAX];
    if (tron) {
        (void)snprintf(norm, sizeof(norm), "%s", addr);
    } else {
        const bool prefixed = (addr[0] == '0') && ((addr[1] == 'x') || (addr[1] == 'X'));
        (void)snprintf(norm, sizeof(norm), "0x%s", prefixed ? (addr + 2) : addr);
    }

    bool         ok = false;
    nvs_handle_t h;
    if (nvs_open(NS_SETTINGS, NVS_READWRITE, &h) == ESP_OK) {
        ok = (nvs_set_str(h, k_val,  norm) == ESP_OK) &&
             (nvs_set_str(h, k_echo, norm) == ESP_OK) &&
             (nvs_commit(h) == ESP_OK);
        nvs_close(h);
    }
    if (ok) {
        ESP_LOGW(TAG, "%s set to %s", what, norm);
    } else {
        ESP_LOGE(TAG, "%s: NVS write failed", what);
    }
    return ok;
}

bool settings_get_payout(bool tron, char *out, size_t n)
{
    /* Ethereum addresses are handed out "0x"-prefixed so every caller can parse
     * them directly; config.h stores them bare, the setup form accepts either. */
    return tron
        ? dual_get(K_PAY_TRX, K_PAY_TRX2, TRON_ADDR_TO, "payout(tron)", out, n)
        : dual_get(K_PAY_ETH, K_PAY_ETH2, "0x" ADDR_TO, "payout(eth)",  out, n);
}

bool settings_has_payout(bool tron)
{
    char scratch[SETTINGS_PAYOUT_MAX];
    const bool stored = settings_get_payout(tron, scratch, sizeof(scratch));
    CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(scratch), sizeof(scratch));
    return stored;
}

bool settings_set_payout(bool tron, const char *addr)
{
    return tron
        ? dual_set(K_PAY_TRX, K_PAY_TRX2, true,  "payout(tron)", addr)
        : dual_set(K_PAY_ETH, K_PAY_ETH2, false, "payout(eth)",  addr);
}

bool settings_get_contract(bool tron, char *out, size_t n)
{
    return tron
        ? dual_get(K_CT_TRX, K_CT_TRX2, TRON_ADDR_USDT, "contract(tron)", out, n)
        : dual_get(K_CT_ETH, K_CT_ETH2, "0x" ADDR_USDC, "contract(eth)",  out, n);
}

bool settings_set_contract(bool tron, const char *addr)
{
    return tron
        ? dual_set(K_CT_TRX, K_CT_TRX2, true,  "contract(tron)", addr)
        : dual_set(K_CT_ETH, K_CT_ETH2, false, "contract(eth)",  addr);
}

void settings_factory_reset(void)
{
    nvs_handle_t h;
    if (nvs_open(NS_SETTINGS, NVS_READWRITE, &h) == ESP_OK) {
        /* Drops brightness, Wi-Fi creds, the admin code and any operator-set
         * payout address, so a reset terminal comes back up into first-run setup
         * and pays out to the config.h recipient again — plus the now-unused
         * "auto_bl" key left on units provisioned before auto-brightness went. */
        (void)nvs_erase_all(h);
        (void)nvs_commit(h);
        nvs_close(h);
        ESP_LOGW(TAG, "settings: factory reset");
    }

    /* provision.cpp keeps the SoftAP passphrase in its own namespace, and a reset
     * puts the terminal straight back into the setup that raises that AP. Left
     * behind, the passphrase from the last setup — photographed off the panel by
     * whoever was there, along with its QR code — would still open the AP that
     * takes the new admin code. Erased here rather than in provision.cpp: this is
     * the function that means "forget the operator", and the passphrase is part
     * of what a new one must not inherit. A fresh one is generated on demand. */
    if (nvs_open("prov", NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_erase_all(h);
        (void)nvs_commit(h);
        nvs_close(h);
        ESP_LOGW(TAG, "settings: setup AP passphrase cleared");
    }
}
