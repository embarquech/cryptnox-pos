/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file tron_rpc.cpp
 * @brief Tron HTTP API client implementation.
 */

/******************************************************************
 * 1. Included files
 ******************************************************************/

#include "tron_rpc.h"
#include "tron_tx.h"
#include "https_post.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <inttypes.h>

/* CW_Utils.h pulls in Arduino.h; keep it ahead of any lwip-including header
 * (see the note in https_post.cpp). */
#include "CW_Utils.h"

#include "esp_log.h"
#include "mbedtls/sha256.h"
#include "cJSON.h"

static const char *const TAG = "tron_rpc";

/* Responses echo the whole transaction; a transfer needs ~1 KB. */
#define RESP_BUF_SIZE   2048U
#define RESP_LOG_MAX    120

/* raw_data is at most TRON_RAW_HEX_MAX/2 bytes. */
#define RAW_BYTES_MAX   (TRON_RAW_HEX_MAX / 2U)

static const char *s_base_url = NULL;

/******************************************************************
 * 2. Local helpers
 ******************************************************************/

static char nibble_hex(unsigned n)
{
    return (n < 10U) ? static_cast<char>('0' + n)
                     : static_cast<char>('a' + n - 10U);
}

static int nibble_val(char c)
{
    if ((c >= '0') && (c <= '9')) { return c - '0'; }
    if ((c >= 'a') && (c <= 'f')) { return (c - 'a') + 10; }
    if ((c >= 'A') && (c <= 'F')) { return (c - 'A') + 10; }
    return -1;
}

/**
 * @brief Decode a hex string into bytes.
 *
 * @param[in]  hex      NUL-terminated hex, even length.
 * @param[out] out      Destination bytes.
 * @param[in]  out_size Capacity of @p out.
 * @return number of bytes written, 0 on odd length, non-hex input or overflow.
 */
static size_t hex_to_bytes(const char *hex, uint8_t *out, size_t out_size)
{
    const size_t len = strlen(hex);
    if ((len == 0U) || ((len % 2U) != 0U) || ((len / 2U) > out_size)) {
        return 0U;
    }
    for (size_t i = 0U; i < (len / 2U); i++) {
        int hi = nibble_val(hex[i * 2U]);
        int lo = nibble_val(hex[i * 2U + 1U]);
        if ((hi < 0) || (lo < 0)) { return 0U; }
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return len / 2U;
}

/** @brief Hex-encode @p len bytes into @p out (NUL-terminated). */
static void bytes_to_hex(const uint8_t *data, size_t len, char *out)
{
    for (size_t i = 0U; i < len; i++) {
        out[i * 2U]      = nibble_hex((data[i] >> 4U) & 0x0FU);
        out[i * 2U + 1U] = nibble_hex(data[i] & 0x0FU);
    }
    out[len * 2U] = '\0';
}

/** @brief POST @p body to @p path under the configured base URL. */
static bool tron_post(const char *path, const char *body,
                      char *resp, size_t resp_size)
{
    if (s_base_url == NULL) {
        ESP_LOGE(TAG, "tron_rpc_init not called");
        return false;
    }
    char url[128];
    int k = snprintf(url, sizeof(url), "%s%s", s_base_url, path);
    if ((k <= 0) || (static_cast<size_t>(k) >= sizeof(url))) { return false; }

    /* No auth, no pinned cert: TronGrid's public testnet endpoint. */
    return https_post_json(url, body, resp, resp_size, NULL, NULL, NULL);
}

/******************************************************************
 * 3. Public API
 ******************************************************************/

void tron_rpc_init(const char *base_url)
{
    s_base_url = base_url;
}

bool tron_rpc_create_transfer(const char *owner_hex, const char *to_hex,
                              uint64_t amount_sun, tron_tx_ctx_t *out)
{
    if ((owner_hex == NULL) || (to_hex == NULL) || (out == NULL) ||
        (amount_sun == 0U)) {
        return false;
    }

    char body[192];
    int k = snprintf(body, sizeof(body),
                     "{\"owner_address\":\"%s\",\"to_address\":\"%s\","
                     "\"amount\":%" PRIu64 "}",
                     owner_hex, to_hex, amount_sun);
    if ((k <= 0) || (static_cast<size_t>(k) >= sizeof(body))) { return false; }

    char resp[RESP_BUF_SIZE];
    if (!tron_post("/wallet/createtransaction", body, resp, sizeof(resp))) {
        return false;
    }

    bool   ok   = false;
    cJSON *root = cJSON_Parse(resp);
    if (root == NULL) {
        ESP_LOGE(TAG, "create: not JSON: %.*s", RESP_LOG_MAX, resp);
        return false;
    }

    const cJSON *txid = cJSON_GetObjectItemCaseSensitive(root, "txID");
    const cJSON *raw  = cJSON_GetObjectItemCaseSensitive(root, "raw_data_hex");
    if (!cJSON_IsString(txid) || (txid->valuestring == NULL) ||
        !cJSON_IsString(raw)  || (raw->valuestring == NULL)) {
        /* Tron reports bad parameters as {"Error":"..."} with HTTP 200. */
        ESP_LOGE(TAG, "create: no txID/raw_data_hex in: %.*s",
                 RESP_LOG_MAX, resp);
        goto done;
    }
    if (strlen(txid->valuestring) != 64U) {
        ESP_LOGE(TAG, "create: txID length %u", (unsigned)strlen(txid->valuestring));
        goto done;
    }
    if (strlen(raw->valuestring) >= TRON_RAW_HEX_MAX) {
        ESP_LOGE(TAG, "create: raw_data too long (%u)",
                 (unsigned)strlen(raw->valuestring));
        goto done;
    }

    /* The node is a trust boundary — check what it serialised before the card
     * signs anything (see tron_tx.h). Contract first: cheapest and the one that
     * catches a redirected payment. */
    if (!tron_tx_contract_ok(raw->valuestring, owner_hex, to_hex, amount_sun)) {
        ESP_LOGE(TAG, "create: raw_data does not match the requested transfer");
        goto done;
    }

    {
        uint8_t raw_bytes[RAW_BYTES_MAX];
        size_t  raw_len = hex_to_bytes(raw->valuestring, raw_bytes,
                                       sizeof(raw_bytes));
        if (raw_len == 0U) {
            ESP_LOGE(TAG, "create: raw_data not hex");
            goto done;
        }

        /* txID must be sha256(raw_data). Without this the node could hand us
         * the hash of an unrelated transaction and get it blind-signed by the
         * card — the raw_data check above would not notice, since the signature
         * covers the hash, not the bytes. */
        uint8_t digest[32];
        if (mbedtls_sha256(raw_bytes, raw_len, digest, 0) != 0) {
            ESP_LOGE(TAG, "create: sha256 failed");
            goto done;
        }
        char digest_hex[65];
        bytes_to_hex(digest, sizeof(digest), digest_hex);
        if (strcasecmp(digest_hex, txid->valuestring) != 0) {
            ESP_LOGE(TAG, "create: txID is not sha256(raw_data) - refusing");
            goto done;
        }

        (void)memcpy(out->txid, digest, sizeof(out->txid));
        (void)snprintf(out->txid_hex, sizeof(out->txid_hex), "%s", digest_hex);
        (void)snprintf(out->raw_hex, sizeof(out->raw_hex), "%s",
                       raw->valuestring);
        ok = true;
        ESP_LOGI(TAG, "Tron tx %s (%" PRIu64 " sun)", out->txid_hex,
                 amount_sun);
    }

done:
    cJSON_Delete(root);
    return ok;
}

bool tron_rpc_broadcast(const tron_tx_ctx_t *tx, const uint8_t sig[65])
{
    if ((tx == NULL) || (sig == NULL)) { return false; }

    char sig_hex[TRON_SIG_HEX_LEN + 1U];
    bytes_to_hex(sig, 65U, sig_hex);

    /* "0a" + varint len + raw_data + "1241" + signature */
    char envelope[TRON_RAW_HEX_MAX + TRON_SIG_HEX_LEN + 16U];
    if (tron_tx_envelope_hex(tx->raw_hex, sig_hex,
                             envelope, sizeof(envelope)) == 0U) {
        ESP_LOGE(TAG, "broadcast: envelope build failed");
        return false;
    }

    char body[sizeof(envelope) + 32U];
    int k = snprintf(body, sizeof(body), "{\"transaction\":\"%s\"}", envelope);
    if ((k <= 0) || (static_cast<size_t>(k) >= sizeof(body))) { return false; }

    char resp[RESP_BUF_SIZE];
    if (!tron_post("/wallet/broadcasthex", body, resp, sizeof(resp))) {
        return false;
    }

    bool   ok   = false;
    cJSON *root = cJSON_Parse(resp);
    if (root == NULL) {
        ESP_LOGE(TAG, "broadcast: not JSON: %.*s", RESP_LOG_MAX, resp);
        return false;
    }
    const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    ok = cJSON_IsTrue(result);
    if (!ok) {
        /* code/message carry the reason (SIGERROR, BANDWIDTH_ERROR, ...). */
        ESP_LOGE(TAG, "broadcast rejected: %.*s", RESP_LOG_MAX, resp);
    }
    cJSON_Delete(root);
    return ok;
}

tron_receipt_t tron_rpc_get_receipt(const char *txid_hex)
{
    if (txid_hex == NULL) { return TRON_RECEIPT_RPC_ERROR; }

    char body[96];
    int k = snprintf(body, sizeof(body), "{\"value\":\"%s\"}", txid_hex);
    if ((k <= 0) || (static_cast<size_t>(k) >= sizeof(body))) {
        return TRON_RECEIPT_RPC_ERROR;
    }

    char resp[RESP_BUF_SIZE];
    if (!tron_post("/wallet/gettransactioninfobyid", body,
                   resp, sizeof(resp))) {
        return TRON_RECEIPT_RPC_ERROR;
    }

    cJSON *root = cJSON_Parse(resp);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return TRON_RECEIPT_RPC_ERROR;
    }

    /* Not in a block yet: the node answers with an empty object. */
    tron_receipt_t verdict = TRON_RECEIPT_PENDING;
    const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    const cJSON *block  = cJSON_GetObjectItemCaseSensitive(root, "blockNumber");
    if (cJSON_IsString(result) && (result->valuestring != NULL) &&
        (strcmp(result->valuestring, "FAILED") == 0)) {
        verdict = TRON_RECEIPT_FAILED;
    } else if (cJSON_IsNumber(block)) {
        verdict = TRON_RECEIPT_SUCCESS;
    }
    cJSON_Delete(root);
    return verdict;
}
