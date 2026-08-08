/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file recip_store.cpp
 * @brief NVS storage, re-decoding and RAM shadow for provisioned recipients.
 *
 * See recip_store.h for the rules this implements and why.
 */

/******************************************************************
 * 1. Included files
 ******************************************************************/

#include "recip_store.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

extern "C" {
#include "eth_addr.h"
}

#include "config.h"

static const char *const TAG = "recip";

#define NS_RECIP  "recip"

/******************************************************************
 * 2. Pair descriptors
 ******************************************************************/

/**
 * @brief Everything that differs between pairs, in one table.
 *
 * NVS keys are spelled out rather than built at runtime: a key assembled from
 * an index is a key an out-of-range index can silently point somewhere else.
 */
typedef struct {
    const char *label;
    recip_fmt_t fmt;
    const char *dflt;      /**< compile-time fallback, used until provisioned */
    const char *key;       /**< NVS: the address string                       */
    const char *key_inv;   /**< NVS: its bitwise complement                   */
} pair_desc_t;

static const pair_desc_t PAIRS[RECIP_PAIR__COUNT] = {
    { "USDC - Ethereum Sepolia", RECIP_FMT_EVM, "0x" ADDR_TO,
      "recip0", "recip0_i" },
};

static bool pair_ok(recip_pair_t p)
{
    return (p >= 0) && (p < RECIP_PAIR__COUNT);
}

const char *recip_pair_label(recip_pair_t p)
{
    return pair_ok(p) ? PAIRS[p].label : "?";
}

/******************************************************************
 * 3. RAM shadow
 ******************************************************************/

/**
 * @brief The in-RAM copy, kept with its complement.
 *
 * @c __NOINIT_ATTR puts this in .noinit — a different linker section from the
 * .bss the rest of the module lives in, so the two copies of the recipient are
 * not neighbours in the map file. Nothing zeroes .noinit at startup, so
 * @ref recip_store_init has to write every field.
 */
typedef struct {
    char    addr[RECIP_ADDR_MAX];
    uint8_t inv[RECIP_ADDR_MAX];
    uint8_t len;
    uint8_t len_inv;
} shadow_t;

static __NOINIT_ATTR shadow_t s_shadow[RECIP_PAIR__COUNT];

static void shadow_set(recip_pair_t p, const char *addr, size_t len)
{
    shadow_t *s = &s_shadow[p];
    (void)memset(s, 0, sizeof(*s));
    for (size_t i = 0U; i < len; i++) {
        s->addr[i] = addr[i];
        s->inv[i]  = (uint8_t)~(uint8_t)addr[i];
    }
    s->len     = (uint8_t)len;
    s->len_inv = (uint8_t)~(uint8_t)len;
}

/** @brief Re-check the shadow against its own complement. */
static recip_verdict_t shadow_ok(recip_pair_t p)
{
    const shadow_t *s = &s_shadow[p];
    const size_t len = s->len;
    /* Fold the length's own check in rather than branching on it twice. */
    uint32_t diff = (uint32_t)(uint8_t)(s->len ^ (uint8_t)~s->len_inv);
    if ((len == 0U) || (len >= RECIP_ADDR_MAX)) { return RECIP_FAIL; }
    diff |= RECIP_IS_OK(recip_bytes_ok(reinterpret_cast<const uint8_t *>(s->addr), len,
                                       s->inv, len, len)) ? 0U : 1U;
    return (diff == 0U) ? RECIP_OK : RECIP_FAIL;
}

/******************************************************************
 * 4. Re-decode / checksum
 ******************************************************************/

/**
 * @brief Decode @p addr for @p pair, checksum and all.
 *
 * @param[out] out20 20-byte key hash; only valid on #RECIP_OK.
 */
static recip_verdict_t decode(recip_pair_t p, const char *addr,
                              uint8_t out20[RECIP_ADDR_BYTES])
{
    if ((addr == NULL) || (out20 == NULL) || !pair_ok(p)) { return RECIP_FAIL; }
    const size_t len = strnlen(addr, RECIP_ADDR_MAX);
    if ((len == 0U) || (len >= RECIP_ADDR_MAX)) { return RECIP_FAIL; }

    switch (PAIRS[p].fmt) {
        case RECIP_FMT_EVM: {
            /* Require the 0x form: an operator pasting a bare 40-hex string
             * should be told, not silently accommodated. */
            if ((len != 42U) || (addr[0] != '0') ||
                ((addr[1] != 'x') && (addr[1] != 'X'))) {
                return RECIP_FAIL;
            }
            if (!eth_addr_parse(addr, out20))  { return RECIP_FAIL; }
            /* EIP-55 is this format's Base58Check: the only typo detection it
             * carries, and mandatory for a hand-typed address. */
            if (!eth_addr_eip55_ok(addr))      { return RECIP_FAIL; }
            return RECIP_OK;
        }
        case RECIP_FMT_TRON:
        default:
            /* Base58Check + the 0x41 prefix assert. Deliberately unimplemented
             * on this branch: nothing here can pay on Tron, so wiring a decoder
             * no payment path exercises would ship untested crypto. Add it with
             * the first Tron pair — failing closed is the right default until
             * then. */
            ESP_LOGE(TAG, "no decoder for this address format");
            return RECIP_FAIL;
    }
}

recip_verdict_t recip_validate(recip_pair_t p, const char *addr)
{
    uint8_t scratch[RECIP_ADDR_BYTES];
    recip_verdict_t v = decode(p, addr, scratch);
    (void)memset(scratch, 0, sizeof(scratch));
    return v;
}

/******************************************************************
 * 5. NVS
 ******************************************************************/

/** @brief Read the plain string copy. @return length, 0 on any problem. */
static size_t nvs_read_plain(recip_pair_t p, char *out, size_t n)
{
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NS_RECIP, NVS_READONLY, &h) != ESP_OK) { return 0U; }
    size_t len = n;
    esp_err_t err = nvs_get_str(h, PAIRS[p].key, out, &len);
    nvs_close(h);
    if (err != ESP_OK) { out[0] = '\0'; return 0U; }
    return strnlen(out, n);
}

/** @brief Read the complement copy, still complemented. @return length. */
static size_t nvs_read_inv(recip_pair_t p, uint8_t *out, size_t n)
{
    nvs_handle_t h;
    if (nvs_open(NS_RECIP, NVS_READONLY, &h) != ESP_OK) { return 0U; }
    size_t len = n;
    esp_err_t err = nvs_get_blob(h, PAIRS[p].key_inv, out, &len);
    nvs_close(h);
    return (err == ESP_OK) ? len : 0U;
}

bool recip_is_provisioned(recip_pair_t p)
{
    if (!pair_ok(p)) { return false; }
    char buf[RECIP_ADDR_MAX];
    return nvs_read_plain(p, buf, sizeof(buf)) > 0U;
}

bool recip_current(recip_pair_t p, char *out, size_t n)
{
    if ((out == NULL) || (n < RECIP_ADDR_MAX)) { return false; }
    out[0] = '\0';
    if (!pair_ok(p) || !RECIP_IS_OK(shadow_ok(p))) { return false; }
    const size_t len = s_shadow[p].len;
    (void)memcpy(out, s_shadow[p].addr, len);
    out[len] = '\0';
    return true;
}

recip_verdict_t recip_provision(recip_pair_t p, const char *addr)
{
    if (!pair_ok(p) || !RECIP_IS_OK(recip_validate(p, addr))) {
        return RECIP_FAIL;
    }
    const size_t len = strnlen(addr, RECIP_ADDR_MAX);

    uint8_t inv[RECIP_ADDR_MAX];
    for (size_t i = 0U; i < len; i++) { inv[i] = (uint8_t)~(uint8_t)addr[i]; }

    /* One open/commit/close: both keys land together or neither does, so the
     * pair cannot be left mid-update looking like corruption. */
    nvs_handle_t h;
    if (nvs_open(NS_RECIP, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "provision: nvs_open failed");
        return RECIP_FAIL;
    }
    esp_err_t e1 = nvs_set_str(h, PAIRS[p].key, addr);
    esp_err_t e2 = nvs_set_blob(h, PAIRS[p].key_inv, inv, len);
    esp_err_t e3 = nvs_commit(h);
    nvs_close(h);
    if ((e1 != ESP_OK) || (e2 != ESP_OK) || (e3 != ESP_OK)) {
        ESP_LOGE(TAG, "provision: write failed");
        return RECIP_FAIL;
    }

    /* Read both back before believing it. A write that reported success but
     * landed wrong would otherwise only surface mid-payment. */
    char     back[RECIP_ADDR_MAX];
    uint8_t  back_inv[RECIP_ADDR_MAX];
    const size_t blen = nvs_read_plain(p, back, sizeof(back));
    const size_t ilen = nvs_read_inv(p, back_inv, sizeof(back_inv));
    if (!RECIP_IS_OK(recip_bytes_ok(reinterpret_cast<const uint8_t *>(back), blen,
                                    back_inv, ilen, len))) {
        ESP_LOGE(TAG, "provision: read-back mismatch");
        return RECIP_FAIL;
    }

    shadow_set(p, back, blen);
    ESP_LOGI(TAG, "%s recipient provisioned", PAIRS[p].label);
    return RECIP_OK;
}

void recip_store_init(void)
{
    for (int i = 0; i < RECIP_PAIR__COUNT; i++) {
        const recip_pair_t p = (recip_pair_t)i;
        char buf[RECIP_ADDR_MAX];
        size_t len = nvs_read_plain(p, buf, sizeof(buf));

        if (len == 0U) {
            /* Never provisioned — fall back to the compile-time literal so an
             * untouched terminal behaves exactly as it did before. */
            (void)snprintf(buf, sizeof(buf), "%s", PAIRS[i].dflt);
            len = strnlen(buf, sizeof(buf));
            ESP_LOGI(TAG, "%s: using config default", PAIRS[i].label);
        }

        /* Whatever the source, it has to decode before it is allowed to become
         * the shadow. A corrupt cell (or a bad literal) must not reach display. */
        uint8_t scratch[RECIP_ADDR_BYTES];
        if (!RECIP_IS_OK(decode(p, buf, scratch))) {
            ESP_LOGE(TAG, "%s: recipient does not validate - pair unusable",
                     PAIRS[i].label);
            (void)memset(&s_shadow[i], 0, sizeof(s_shadow[i]));
            continue;
        }
        shadow_set(p, buf, len);
        ESP_LOGI(TAG, "%s -> %s", PAIRS[i].label, buf);
    }
}

/******************************************************************
 * 6. Read paths
 ******************************************************************/

recip_verdict_t recip_checkpoint(recip_pair_t p, recip_step_id_t to,
                                 recip_steps_t *st)
{
    if (!pair_ok(p) || (st == NULL))    { return RECIP_FAIL; }
    if (!RECIP_IS_OK(shadow_ok(p)))     { return RECIP_FAIL; }
    return recip_steps_advance(st, to);
}

recip_verdict_t recip_read_display(recip_pair_t p, char *out, size_t n,
                                   recip_steps_t *st)
{
    if ((out == NULL) || (n < RECIP_ADDR_MAX)) { return RECIP_FAIL; }
    out[0] = '\0';
    if (!pair_ok(p) || (st == NULL))    { return RECIP_FAIL; }
    if (!RECIP_IS_OK(recip_checkpoint(p, RECIP_STEP_READ, st))) {
        return RECIP_FAIL;
    }

    char buf[RECIP_ADDR_MAX];
    size_t len = nvs_read_plain(p, buf, sizeof(buf));
    if (len == 0U) {
        /* Not provisioned: the shadow already holds the validated default. */
        len = s_shadow[p].len;
        (void)memcpy(buf, s_shadow[p].addr, len);
        buf[len] = '\0';
    }

    /* Against the shadow's complement, not against the shadow's plain copy —
     * comparing a value to itself would pass on a doubly-corrupted pair. */
    if (!RECIP_IS_OK(recip_bytes_ok(reinterpret_cast<const uint8_t *>(buf), len,
                                    s_shadow[p].inv, s_shadow[p].len, len))) {
        ESP_LOGE(TAG, "display: NVS disagrees with RAM shadow");
        return RECIP_FAIL;
    }
    uint8_t scratch[RECIP_ADDR_BYTES];
    if (!RECIP_IS_OK(decode(p, buf, scratch))) {
        ESP_LOGE(TAG, "display: recipient no longer decodes");
        return RECIP_FAIL;
    }
    if (!RECIP_IS_OK(recip_steps_advance(st, RECIP_STEP_DISPLAY))) {
        return RECIP_FAIL;
    }

    (void)memcpy(out, buf, len);
    out[len] = '\0';
    return RECIP_OK;
}

recip_verdict_t recip_read_signing(recip_pair_t p, const char *displayed,
                                   uint8_t out20[RECIP_ADDR_BYTES],
                                   recip_steps_t *st)
{
    if (out20 == NULL) { return RECIP_FAIL; }
    (void)memset(out20, 0, RECIP_ADDR_BYTES);
    if (!pair_ok(p) || (displayed == NULL) || (st == NULL)) { return RECIP_FAIL; }

    /* Must have been displayed AND confirmed. Reaching signing from any other
     * step means a gate was skipped. */
    if (!RECIP_IS_OK(recip_steps_at(st, RECIP_STEP_CONFIRM))) {
        ESP_LOGE(TAG, "sign: step counter says a gate was skipped");
        return RECIP_FAIL;
    }
    if (!RECIP_IS_OK(shadow_ok(p))) {
        ESP_LOGE(TAG, "sign: RAM shadow inconsistent");
        return RECIP_FAIL;
    }

    const size_t dlen = strnlen(displayed, RECIP_ADDR_MAX);

    /* The second read: a different key, a different flash offset, a different
     * moment. Falls back to the shadow's complement when unprovisioned, which
     * is still a copy independent of the one display used. */
    uint8_t inv[RECIP_ADDR_MAX];
    size_t  ilen = nvs_read_inv(p, inv, sizeof(inv));
    if (ilen == 0U) {
        ilen = s_shadow[p].len;
        (void)memcpy(inv, s_shadow[p].inv, ilen);
    }

    if (!RECIP_IS_OK(recip_bytes_ok(reinterpret_cast<const uint8_t *>(displayed), dlen,
                                    inv, ilen, dlen))) {
        ESP_LOGE(TAG, "sign: displayed address is not the stored one - abort");
        return RECIP_FAIL;
    }

    /* Rebuild the string from the complement copy and decode THAT, so the bytes
     * that reach the calldata come from the second read, not the first. */
    char plain[RECIP_ADDR_MAX];
    for (size_t i = 0U; i < ilen; i++) { plain[i] = (char)~(uint8_t)inv[i]; }
    plain[ilen] = '\0';

    uint8_t decoded[RECIP_ADDR_BYTES];
    if (!RECIP_IS_OK(decode(p, plain, decoded))) {
        ESP_LOGE(TAG, "sign: stored recipient does not decode - abort");
        return RECIP_FAIL;
    }
    if (!RECIP_IS_OK(recip_steps_advance(st, RECIP_STEP_SERIALIZE))) {
        return RECIP_FAIL;
    }

    (void)memcpy(out20, decoded, RECIP_ADDR_BYTES);
    return RECIP_OK;
}
