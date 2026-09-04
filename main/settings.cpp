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
#include "nvs_flash.h"    /* settings_wipe_if_new_firmware — erases the partition */
#include "esp_app_desc.h" /* the running image's ELF SHA-256, its identity here */
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
/* And the mainnet halves of both pairs, for a config.h written before the
 * production networks were selectable. Empty means the asset is refused on
 * mainnet and works on the testnet exactly as it did. */
#ifndef TRON_ADDR_USDT_MAIN
#define TRON_ADDR_USDT_MAIN  ""
#endif
#ifndef ADDR_USDC_MAIN
#define ADDR_USDC_MAIN  ""
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
#define K_MAINNET     "mainnet"
/* Which firmware last ran on this unit, and whether the next boot owes it a
 * wipe — see settings_arm_wipe_if_new_firmware. */
#define K_FW_SHA      "fw_sha"
#define K_WIPE        "fw_wipe"
/* Payout addresses, each stored twice — see settings_get_payout. */
#define K_PAY_ETH     "pay_eth"
#define K_PAY_ETH2    "pay_eth_e"
#define K_PAY_TRX     "pay_trx"
#define K_PAY_TRX2    "pay_trx_e"
/* Token contracts, same treatment — see settings_get_contract. One pair of keys
 * per network per deployment: the mainnet USDC and the Sepolia one are different
 * addresses, and sharing a slot would carry one across a network switch. */
#define K_CT_ETH      "ct_eth"
#define K_CT_ETH2     "ct_eth_e"
#define K_CT_TRX      "ct_trx"
#define K_CT_TRX2     "ct_trx_e"
#define K_CT_ETH_M    "ct_eth_m"
#define K_CT_ETH_M2   "ct_eth_me"
#define K_CT_TRX_M    "ct_trx_m"
#define K_CT_TRX_M2   "ct_trx_me"

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

bool settings_get_mainnet(void)
{
    /* Defaults true, and the read is written so that every way of not knowing —
     * no key, an unopenable namespace, a factory-fresh unit — lands on the
     * production networks. A terminal that guesses "testnet" takes a shift's
     * worth of payments that settle nowhere and reports each one as done. */
    uint8_t stored = 1U;
    nvs_handle_t h;
    if (nvs_open(NS_SETTINGS, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u8(h, K_MAINNET, &stored);
        nvs_close(h);
    }
    return (stored != 0U);
}

void settings_set_mainnet(bool mainnet)
{
    nvs_handle_t h;
    if (nvs_open(NS_SETTINGS, NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_set_u8(h, K_MAINNET, mainnet ? 1U : 0U);
        (void)nvs_commit(h);
        nvs_close(h);
        ESP_LOGW(TAG, "network set to %s", mainnet ? "mainnet" : "testnet");
    } else {
        ESP_LOGW(TAG, "mainnet: nvs_open failed");
    }
}

const char *settings_net_str(const char *testnet, const char *mainnet)
{
    return settings_get_mainnet() ? mainnet : testnet;
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
    const bool m = settings_get_mainnet();
    return tron
        ? dual_get(m ? K_CT_TRX_M : K_CT_TRX, m ? K_CT_TRX_M2 : K_CT_TRX2,
                   m ? TRON_ADDR_USDT_MAIN : TRON_ADDR_USDT,
                   "contract(tron)", out, n)
        : dual_get(m ? K_CT_ETH_M : K_CT_ETH, m ? K_CT_ETH_M2 : K_CT_ETH2,
                   m ? "0x" ADDR_USDC_MAIN : "0x" ADDR_USDC,
                   "contract(eth)",  out, n);
}

bool settings_set_contract(bool tron, const char *addr)
{
    const bool m = settings_get_mainnet();
    return tron
        ? dual_set(m ? K_CT_TRX_M : K_CT_TRX, m ? K_CT_TRX_M2 : K_CT_TRX2,
                   true,  "contract(tron)", addr)
        : dual_set(m ? K_CT_ETH_M : K_CT_ETH, m ? K_CT_ETH_M2 : K_CT_ETH2,
                   false, "contract(eth)",  addr);
}

/** @brief Write the running image's identity into the settings namespace. */
static void fw_stamp_write(const esp_app_desc_t *d)
{
    nvs_handle_t h;
    if (nvs_open(NS_SETTINGS, NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_set_blob(h, K_FW_SHA, d->app_elf_sha256,
                           sizeof(d->app_elf_sha256));
        (void)nvs_commit(h);
        nvs_close(h);
    }
}

void settings_apply_pending_wipe(void)
{
    nvs_handle_t h;
    uint8_t      armed = 0U;
    if (nvs_open(NS_SETTINGS, NVS_READONLY, &h) != ESP_OK) { return; }
    const bool pending = (nvs_get_u8(h, K_WIPE, &armed) == ESP_OK) && (armed != 0U);
    nvs_close(h);
    if (!pending) { return; }

    ESP_LOGW(TAG, "wipe armed by the last boot - erasing NVS");
    esp_err_t err = nvs_flash_erase();
    if (err == ESP_OK) { err = nvs_flash_init(); }
    if (err != ESP_OK) {
        /* The flag is only cleared by the erase itself, so a failure here leaves
         * it armed and the next boot tries again rather than recording a
         * half-done wipe as finished. */
        ESP_LOGE(TAG, "NVS erase failed (%s) - settings kept", esp_err_to_name(err));
        return;
    }

    /* The erase took the flag and the stamp with it. Re-stamp, or the check below
     * would arm another wipe on the very next boot and loop. */
    const esp_app_desc_t *d = esp_app_get_description();
    if (d != NULL) { fw_stamp_write(d); }
}

bool settings_arm_wipe_if_new_firmware(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    if (d == NULL) { return false; }   /* nothing to compare against */

    uint8_t     stored[sizeof(d->app_elf_sha256)] = { 0 };
    size_t      n = sizeof(stored);
    nvs_handle_t h;
    bool         have  = false;
    bool         same  = false;
    if (nvs_open(NS_SETTINGS, NVS_READONLY, &h) == ESP_OK) {
        have = (nvs_get_blob(h, K_FW_SHA, stored, &n) == ESP_OK) &&
               (n == sizeof(stored));
        same = have && (memcmp(stored, d->app_elf_sha256, n) == 0);
        nvs_close(h);
    }
    if (same) { return false; }   /* same image as last boot — a power-cycle */

    /* A flag still set at this point means settings_apply_pending_wipe() ran
     * this boot and its erase failed. Re-arming would restart into the same
     * failure and never reach the main loop, so the terminal would stop taking
     * payments entirely — and the panel would be insisting the settings were
     * cleared when they were not. Give up loudly instead and carry on: a unit
     * that keeps working with its old settings beats a brick. */
    uint8_t stale = 0U;
    if ((nvs_open(NS_SETTINGS, NVS_READONLY, &h) == ESP_OK)) {
        const bool armed = (nvs_get_u8(h, K_WIPE, &stale) == ESP_OK) && (stale != 0U);
        nvs_close(h);
        if (armed) {
            ESP_LOGE(TAG, "a wipe is armed but the erase failed - not retrying");
            return false;
        }
    }

    if (!have) {
        /* No stamp: a factory unit, or a unit updated from a build that predates
         * this check. There is no "previous firmware" to clear after, and wiping
         * here would erase the setup the operator has just finished on this very
         * boot — which, since setup ends in a restart, would loop. Adopt the
         * running image instead; every update after this one clears. */
        ESP_LOGI(TAG, "firmware not stamped yet - adopting it, no wipe");
        fw_stamp_write(d);
        return false;
    }

    /* Arm rather than erase. nvs_flash_erase() cannot run with handles open, and
     * by this point the Wi-Fi driver holds its own; the erase therefore happens
     * on the next boot, in settings_apply_pending_wipe(), before anything has
     * opened NVS. This is also why the decision is made here and not at boot: it
     * must come after the image has cancelled its rollback, or a wipe would be
     * followed by a revert to firmware that then wipes again. */
    ESP_LOGW(TAG, "firmware changed - arming an NVS wipe for the next boot");
    if (nvs_open(NS_SETTINGS, NVS_READWRITE, &h) != ESP_OK) { return false; }
    const bool armed_ok = (nvs_set_u8(h, K_WIPE, 1U) == ESP_OK) &&
                          (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    return armed_ok;
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

    /* provision.cpp's own namespace. Nothing in it is load-bearing any more — the
     * AP passphrase is drawn per session and never leaves RAM, and the admin
     * page's TLS identity is gone with the TLS — so what this clears is whatever
     * an older build of this firmware left behind on the unit. Erased here rather
     * than in provision.cpp because this is the function that means "forget the
     * operator", and a new one must not inherit any of the last one's keys. */
    if (nvs_open("prov", NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_erase_all(h);
        (void)nvs_commit(h);
        nvs_close(h);
        ESP_LOGW(TAG, "settings: portal namespace cleared");
    }
}
