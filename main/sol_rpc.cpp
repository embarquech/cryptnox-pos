/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file sol_rpc.cpp
 * @brief Solana JSON-RPC client implementation.
 */

#include "sol_rpc.h"
#include "sol_tx.h"
#include "https_post.h"
#include "rpc_error.h"   /* rpc_err_contains */

#include <stdio.h>
#include <string.h>

/* CW_Utils.h pulls in Arduino.h; keep it ahead of any lwip-including header
 * (see the note in https_post.cpp). */
#include "CW_Utils.h"

#include "esp_log.h"
#include "mbedtls/base64.h"
#include "cJSON.h"

static const char *const TAG = "sol_rpc";

/* Most answers are a few hundred bytes; a refused sendTransaction carries the
 * simulation's logs, and one cut short would not parse — reading a refusal as
 * "no answer" and polling a transaction that never left. Same size as Tron's. */
#define RESP_BUF_SIZE  3072U

#define RESP_LOG_MAX   160

static const char *s_url     = NULL;
static const char *s_ca_cert = NULL;

void sol_rpc_init(const char *url)        { s_url = url; }
void sol_rpc_set_ca_cert(const char *pem) { s_ca_cert = pem; }

/**
 * @brief POST one JSON-RPC call and hand back the parsed reply.
 * @return the root object (caller deletes), or NULL on transport/parse error.
 */
static cJSON *sol_call(const char *method, const char *params)
{
    if (s_url == NULL) {
        ESP_LOGE(TAG, "sol_rpc_init not called");
        return NULL;
    }
    char body[512];
    const int k = snprintf(body, sizeof(body),
                           "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"%s\","
                           "\"params\":%s}", method, params);
    if ((k <= 0) || (static_cast<size_t>(k) >= sizeof(body))) { return NULL; }

    char resp[RESP_BUF_SIZE];
    if (!https_post_json(s_url, body, resp, sizeof(resp), NULL, NULL, s_ca_cert)) {
        return NULL;
    }
    cJSON *root = cJSON_Parse(resp);
    if (root == NULL) {
        ESP_LOGE(TAG, "%s: not JSON: %.*s", method, RESP_LOG_MAX, resp);
    }
    return root;
}

/** @brief A cJSON number as a uint64, clamped; anything else is false. */
static bool json_u64(const cJSON *n, uint64_t *out)
{
    if (!cJSON_IsNumber(n) || (n->valuedouble < 0.0)) { return false; }
    *out = (n->valuedouble >= 18446744073709551615.0) ? UINT64_MAX
                                                      : (uint64_t)n->valuedouble;
    return true;
}

bool sol_rpc_get_balance(const char *addr_b58, uint64_t *lamports)
{
    char params[96];
    (void)snprintf(params, sizeof(params),
                   "[\"%s\",{\"commitment\":\"confirmed\"}]", addr_b58);
    cJSON *root = sol_call("getBalance", params);
    if (root == NULL) { return false; }
    const cJSON *res = cJSON_GetObjectItemCaseSensitive(root, "result");
    const bool ok = json_u64(cJSON_GetObjectItemCaseSensitive(res, "value"), lamports);
    cJSON_Delete(root);
    return ok;
}

bool sol_rpc_latest_blockhash(uint8_t blockhash[32], uint64_t *last_valid_height)
{
    cJSON *root = sol_call("getLatestBlockhash", "[{\"commitment\":\"confirmed\"}]");
    if (root == NULL) { return false; }
    const cJSON *val = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(root, "result"), "value");
    const cJSON *bh = cJSON_GetObjectItemCaseSensitive(val, "blockhash");
    const bool ok = cJSON_IsString(bh) && (bh->valuestring != NULL) &&
                    sol_addr_decode(bh->valuestring, blockhash) &&
                    json_u64(cJSON_GetObjectItemCaseSensitive(val, "lastValidBlockHeight"),
                             last_valid_height);
    cJSON_Delete(root);
    return ok;
}

bool sol_rpc_block_height(uint64_t *height)
{
    cJSON *root = sol_call("getBlockHeight", "[{\"commitment\":\"confirmed\"}]");
    if (root == NULL) { return false; }
    const bool ok = json_u64(cJSON_GetObjectItemCaseSensitive(root, "result"), height);
    cJSON_Delete(root);
    return ok;
}

/** @brief A preflight refusal, said as what the operator can do about it. */
static void refusal_text(const char *msg, char *err, size_t err_n)
{
    if (rpc_err_contains(msg, "no record of a prior credit")) {
        (void)snprintf(err, err_n, "No SOL on this card");
    } else if (rpc_err_contains(msg, "insufficient")) {
        (void)snprintf(err, err_n, "Not enough SOL for amount + fee");
    } else if (rpc_err_contains(msg, "blockhash not found")) {
        (void)snprintf(err, err_n, "Network busy - try again");
    } else {
        (void)snprintf(err, err_n, "Node refused: %.40s", msg);
    }
}

sol_send_t sol_rpc_send(const uint8_t *tx, size_t tx_len, const char *sig_b58,
                        char *err, size_t err_n)
{
    unsigned char b64[((SOL_TRANSFER_TX_LEN + 2U) / 3U) * 4U + 1U];
    size_t        b64_len = 0U;
    if (mbedtls_base64_encode(b64, sizeof(b64), &b64_len, tx, tx_len) != 0) {
        (void)snprintf(err, err_n, "Transaction too large");
        return SOL_SEND_REFUSED;
    }
    char params[sizeof(b64) + 96U];
    (void)snprintf(params, sizeof(params),
                   "[\"%s\",{\"encoding\":\"base64\","
                   "\"preflightCommitment\":\"confirmed\"}]", b64);

    cJSON *root = sol_call("sendTransaction", params);
    if (root == NULL) { return SOL_SEND_UNKNOWN; }

    sol_send_t   rc  = SOL_SEND_UNKNOWN;
    const cJSON *res = cJSON_GetObjectItemCaseSensitive(root, "result");
    const cJSON *e   = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (cJSON_IsString(res) && (res->valuestring != NULL)) {
        if (strcmp(res->valuestring, sig_b58) == 0) {
            rc = SOL_SEND_OK;
        } else {
            /* Not our transaction's id. Whatever it means, ours may be out. */
            ESP_LOGE(TAG, "send: node answered %s for %s", res->valuestring, sig_b58);
        }
    } else if (cJSON_IsObject(e)) {
        /* Preflight is on, so an error is a simulation the node refused to
         * forward: nothing went out. */
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(e, "message");
        const char  *msg = (cJSON_IsString(m) && (m->valuestring != NULL))
                               ? m->valuestring : "";
        ESP_LOGW(TAG, "send refused: %s", msg);
        refusal_text(msg, err, err_n);
        rc = SOL_SEND_REFUSED;
    } else {
        /* neither: treated as no answer */
    }
    cJSON_Delete(root);
    return rc;
}

sol_status_t sol_rpc_get_status(const char *sig_b58)
{
    char params[160];
    (void)snprintf(params, sizeof(params), "[[\"%s\"]]", sig_b58);
    cJSON *root = sol_call("getSignatureStatuses", params);
    if (root == NULL) { return SOL_STATUS_RPC_ERROR; }

    sol_status_t rc  = SOL_STATUS_RPC_ERROR;
    const cJSON *val = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(root, "result"), "value");
    const cJSON *st  = cJSON_IsArray(val) ? cJSON_GetArrayItem(val, 0) : NULL;
    if (cJSON_IsNull(st)) {
        rc = SOL_STATUS_PENDING;   /* not seen (yet) */
    } else if (cJSON_IsObject(st)) {
        const cJSON *cs = cJSON_GetObjectItemCaseSensitive(st, "confirmationStatus");
        const cJSON *er = cJSON_GetObjectItemCaseSensitive(st, "err");
        const bool settled = cJSON_IsString(cs) && (cs->valuestring != NULL) &&
                             ((strcmp(cs->valuestring, "confirmed") == 0) ||
                              (strcmp(cs->valuestring, "finalized") == 0));
        if (!settled) {
            rc = SOL_STATUS_PENDING;   /* "processed": could still be on a fork */
        } else if (cJSON_IsNull(er)) {
            rc = SOL_STATUS_SUCCESS;
        } else if (er != NULL) {
            rc = SOL_STATUS_FAILED;
        } else {
            /* no "err" at all: not an answer either way */
        }

    } else {
        /* malformed: transient */
    }
    cJSON_Delete(root);
    return rc;
}
