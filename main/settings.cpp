/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file settings.cpp
 * @brief NVS-backed implementation of the persistent device settings.
 */

#include "settings.h"

#include <atomic>     /* the chain / network caches, read across tasks */
#include <stdio.h>    /* snprintf — payout address normalisation */
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"    /* settings_wipe_if_new_build — erases the partition */
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
/* Touch calibration, one key per axis, packed min<<16 | max. */
/* Minutes east of UTC, stored biased — see settings_get_tz_offset_min(). */
#define K_TZ_OFFSET   "tz_off"
#define TZ_OFFSET_BIAS 720
#define K_TOUCH_X     "touch_x"
#define K_TOUCH_Y     "touch_y"
/* BUILD_ID of the newest firmware that has run on this unit — see
 * settings_wipe_if_new_build. */
#define K_BUILD_ID    "build_id"
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

/******************************************************************
 * NVS scalar helpers
 *
 * Every scalar setting below is the same eight lines — open, read or write,
 * commit, close — differing only in the key, the width and the default. Written
 * once here so a getter is one line and the failure behaviour (fall back to the
 * default, never guess) cannot drift between them.
 *
 * A failed open is not logged on the read path on purpose: the callers each have
 * a default that is correct, and the reads happen often enough that a warning
 * would be noise. The write path does log — a setting that silently did not
 * persist is the kind of thing an operator reports as "it forgot".
 ******************************************************************/
static uint8_t nvs_u8_get(const char *key, uint8_t def)
{
    uint8_t val = def;
    nvs_handle_t h;
    if (nvs_open(NS_SETTINGS, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u8(h, key, &val);
        nvs_close(h);
    }
    return val;
}

static void nvs_u8_set(const char *key, uint8_t val)
{
    nvs_handle_t h;
    if (nvs_open(NS_SETTINGS, NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_set_u8(h, key, val);
        (void)nvs_commit(h);
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "%s: nvs_open failed", key);
    }
}

static uint32_t nvs_u32_get(const char *key, uint32_t def)
{
    uint32_t val = def;
    nvs_handle_t h;
    if (nvs_open(NS_SETTINGS, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u32(h, key, &val);
        nvs_close(h);
    }
    return val;
}

static void nvs_u32_set(const char *key, uint32_t val)
{
    nvs_handle_t h;
    if (nvs_open(NS_SETTINGS, NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_set_u32(h, key, val);
        (void)nvs_commit(h);
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "%s: nvs_open failed", key);
    }
}

/******************************************************************
 * The two settings that are read constantly
 *
 * The chain and the network flag are asked for on nearly every pass of the UI —
 * the asset badge, the ticker, the network subtitle, the keypad's ceiling — and
 * the keypad's ceiling means an NVS open per typed digit. Cached in RAM, with
 * -1 standing for "not read yet" so a factory-fresh unit still resolves through
 * the read path exactly once and lands on the documented default.
 *
 * Atomic because the writer is the UI task (the asset picker) and one reader is
 * the main task, mid-payment. Cheap: a 16-bit aligned load on this core.
 *
 * Safe to cache for opposite reasons. The chain is written only through
 * settings_set_chain(), so the cache is written through there and cannot go
 * stale. The network flag cannot change at all without a restart — the config
 * page reboots the terminal to apply it (provision.cpp, network_post) — so one
 * read per boot is the whole story.
 ******************************************************************/
static std::atomic<int16_t> s_chain_cache{-1};
static std::atomic<int8_t>  s_mainnet_cache{-1};

/** @brief Forget both, so the next read goes back to flash. For the erase paths. */
static void cache_invalidate(void)
{
    s_chain_cache.store(-1);
    s_mainnet_cache.store(-1);
}

pos_chain_t settings_get_chain(void)
{
    const int16_t cached = s_chain_cache.load();
    if (cached >= 0) { return (pos_chain_t)cached; }

    pos_chain_t chain = POS_CHAIN_ETH_SEPOLIA;
    /* Unknown value = a downgrade or a corrupt cell; fall back to the default
     * rather than charge on a chain no code path can handle. */
    const uint8_t stored = nvs_u8_get(K_CHAIN, (uint8_t)POS_CHAIN_ETH_SEPOLIA);
    if (stored < (uint8_t)POS_CHAIN__COUNT) {
        chain = (pos_chain_t)stored;
    }
    s_chain_cache.store((int16_t)chain);
    return chain;
}

void settings_set_chain(pos_chain_t chain)
{
    nvs_u8_set(K_CHAIN, (uint8_t)chain);
    /* After the write, not before: a reader that arrives in between gets the old
     * value, which is the one still in flash. */
    s_chain_cache.store((int16_t)chain);
}

bool settings_get_mainnet(void)
{
    const int8_t cached = s_mainnet_cache.load();
    if (cached >= 0) { return (cached != 0); }

    /* Defaults true, and the read is written so that every way of not knowing —
     * no key, an unopenable namespace, a factory-fresh unit — lands on the
     * production networks. A terminal that guesses "testnet" takes a shift's
     * worth of payments that settle nowhere and reports each one as done. */
    const bool mainnet = (nvs_u8_get(K_MAINNET, 1U) != 0U);
    s_mainnet_cache.store(mainnet ? 1 : 0);
    return mainnet;
}

void settings_set_mainnet(bool mainnet)
{
    nvs_u8_set(K_MAINNET, mainnet ? 1U : 0U);
    s_mainnet_cache.store(mainnet ? 1 : 0);
    ESP_LOGW(TAG, "network set to %s", mainnet ? "mainnet" : "testnet");
}

const char *settings_net_str(const char *testnet, const char *mainnet)
{
    return settings_get_mainnet() ? mainnet : testnet;
}

uint8_t settings_get_brightness(void)
{
    return nvs_u8_get(K_BRIGHTNESS, DEFAULT_BRIGHTNESS);
}

void settings_set_brightness(uint8_t pct)
{
    if (pct > 100U) { pct = 100U; }
    nvs_u8_set(K_BRIGHTNESS, pct);
}

int16_t settings_get_tz_offset_min(void)
{
    /* Stored biased by 720 so it fits the unsigned helpers the rest of this
     * file uses, and so a missing key reads as UTC rather than as UTC-12. */
    const uint32_t raw = nvs_u32_get(K_TZ_OFFSET, (uint32_t)TZ_OFFSET_BIAS);
    if (raw > (uint32_t)(TZ_OFFSET_BIAS + TZ_OFFSET_MAX)) {
        return 0;   /* nonsense in NVS is UTC, not a wild clock */
    }
    return (int16_t)((int32_t)raw - TZ_OFFSET_BIAS);
}

bool settings_set_tz_offset_min(int16_t minutes)
{
    if ((minutes < TZ_OFFSET_MIN) || (minutes > TZ_OFFSET_MAX)) {
        return false;
    }
    nvs_u32_set(K_TZ_OFFSET, (uint32_t)((int32_t)minutes + TZ_OFFSET_BIAS));
    return true;
}

/* Packed two per u32 (min<<16 | max) — two keys instead of four, and an axis
 * can never be half-written. */
#define TOUCH_CAL_DEF_MIN  200U
#define TOUCH_CAL_DEF_MAX  3800U

void settings_get_touch_cal(uint16_t *x_min, uint16_t *x_max,
                            uint16_t *y_min, uint16_t *y_max)
{
    const uint32_t def = (TOUCH_CAL_DEF_MIN << 16) | TOUCH_CAL_DEF_MAX;
    uint32_t x = nvs_u32_get(K_TOUCH_X, def);
    uint32_t y = nvs_u32_get(K_TOUCH_Y, def);
    *x_min = (uint16_t)(x >> 16); *x_max = (uint16_t)(x & 0xFFFFU);
    *y_min = (uint16_t)(y >> 16); *y_max = (uint16_t)(y & 0xFFFFU);
}

void settings_set_touch_cal(uint16_t x_min, uint16_t x_max,
                            uint16_t y_min, uint16_t y_max)
{
    /* A span under this is a double-tap on one spot, not a calibration, and
     * storing it maps the whole panel onto a few pixels — after which nothing,
     * including the calibration screen, can be tapped again. */
    const uint16_t MIN_SPAN = 500U;
    if ((x_max < x_min + MIN_SPAN) || (y_max < y_min + MIN_SPAN)) {
        ESP_LOGW(TAG, "touch cal rejected: span too small");
        return;
    }
    nvs_u32_set(K_TOUCH_X, ((uint32_t)x_min << 16) | x_max);
    nvs_u32_set(K_TOUCH_Y, ((uint32_t)y_min << 16) | y_max);
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

uint32_t settings_get_max_fee_gwei(void)
{
    return nvs_u32_get(K_MAX_FEE, DEFAULT_MAX_FEE_GWEI);
}

void settings_set_max_fee_gwei(uint32_t gwei)
{
    nvs_u32_set(K_MAX_FEE, gwei);
}

uint32_t settings_get_priority_fee_gwei(void)
{
    return nvs_u32_get(K_PRIO_FEE, DEFAULT_PRIORITY_FEE_GWEI);
}

void settings_set_priority_fee_gwei(uint32_t gwei)
{
    nvs_u32_set(K_PRIO_FEE, gwei);
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
    nvs_u8_set(K_ADMIN_FAILS, n);
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
    return nvs_u8_get(K_ADMIN_FAILS, 0U);
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

bool settings_wipe_if_new_build(void)
{
    nvs_handle_t h;
    uint32_t     stored = 0U;    /* no key yet reads as 0, i.e. older than any build */
    if (nvs_open(NS_SETTINGS, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u32(h, K_BUILD_ID, &stored);
        nvs_close(h);
    }
    /* Equal, and only equal, keeps the settings: that is a power-cycle of the
     * image that wrote them. Everything else — a newer build, an older one, no
     * stamp at all — means this NVS was last written by a different image, and
     * the firmware is open source, so "a different image" includes one somebody
     * built to leave a payout address behind for the official firmware to find
     * and pay out to. State from an image that is not this one is not state this
     * one may act on.
     *
     * Ordering was the earlier rule and it was wrong for exactly that reason: a
     * hostile image only had to stamp a large number to be treated as a rollback
     * and keep everything it had planted.
     *
     * Logged either way — "the update did not clear the settings" is
     * indistinguishable from a broken check unless both numbers are on the
     * console. */
    if (stored == BUILD_ID) {
        ESP_LOGI(TAG, "build %u - settings written by this build, kept",
                 (unsigned)BUILD_ID);
        return false;
    }

    ESP_LOGW(TAG, "stamped %u, running build %u - erasing NVS",
             (unsigned)stored, (unsigned)BUILD_ID);
    esp_err_t err = nvs_flash_erase();
    if (err == ESP_OK) { err = nvs_flash_init(); }
    if (err != ESP_OK) {
        /* Nothing was written, so the next boot sees the same older stamp and
         * tries again rather than recording a half-done wipe as finished. The
         * terminal meanwhile keeps working on its old settings: a unit that
         * still takes payments beats a brick. */
        ESP_LOGE(TAG, "NVS erase failed (%s) - settings kept", esp_err_to_name(err));
        return false;
    }

    /* Stamp straight away — the erase took the old value with it, and an
     * unwritten stamp means a wipe on every boot, so the operator would re-run
     * setup after each power cut. */
    if (nvs_open(NS_SETTINGS, NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_set_u32(h, K_BUILD_ID, (uint32_t)BUILD_ID);
        (void)nvs_commit(h);
        nvs_close(h);
    }
    /* Whatever the caches hold describes a partition that no longer exists. This
     * runs before anything reads them today, so it is belt and braces — but it is
     * the ordering that makes it safe, and an invalidation here does not depend on
     * that ordering staying true. */
    cache_invalidate();
    return true;
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
        /* Put the build stamp straight back: erase_all took it with the rest, and
         * without it the next boot reads 0, wipes again and greets the operator
         * with "Updated to ... settings are cleared" for an update that never
         * happened. */
        (void)nvs_set_u32(h, K_BUILD_ID, (uint32_t)BUILD_ID);
        (void)nvs_commit(h);
        nvs_close(h);
        ESP_LOGW(TAG, "settings: factory reset");
    }
    cache_invalidate();   /* the chain and network flag went with the erase */

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
