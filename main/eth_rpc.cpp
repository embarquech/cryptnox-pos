/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file eth_rpc.cpp
 * @brief Ethereum JSON-RPC client implementation (HTTPS).  Network bring-up
 *        (Wi-Fi, SNTP) lives in net.cpp.
 */

/******************************************************************
 * 1. Included files
 ******************************************************************/

#include "eth_rpc.h"
#include "eth_json.h"
#include "https_post.h"

#include <string.h>
#include <strings.h>    /* strncasecmp */
#include <stdlib.h>     /* strtoull, malloc, free */
#include <stdio.h>      /* snprintf */
#include <inttypes.h>   /* PRIu64 */

/* CW_Utils.h pulls in Arduino.h (via platform_compat.h); it must come before
 * any lwip-including IDF header so that IPAddress.h declares INADDR_NONE
 * before lwip defines it as a macro. */
#include "CW_Utils.h"   /* hardened memory primitives (CODING_RULES §1.4) */

#include "esp_log.h"

static const char *const TAG = "eth_rpc";

/* JSON-RPC response buffer — large enough for any expected response. */
#define RESP_BUF_SIZE  1024U

/* Hex chars per byte */
#define HEX_PER_BYTE  2U

/* never dump full RPC responses (they can echo credentials embedded
 * in the URL) — log at most this many bytes on parse failures. */
#define RESP_LOG_MAX  80

/* sanity bound for the account nonce — a real terminal never gets
 * anywhere near 2^32 transactions, so anything above is a bogus response. */
#define NONCE_MAX  0xFFFFFFFFULL

/* Largest expected "result" string: 0x + 64 hex chars + NUL, rounded up. */
#define RESULT_STR_MAX  80U

/******************************************************************
 * 2. Constants and module state
 ******************************************************************/

static const char *s_rpc_url     = NULL;
static const char *s_from_addr   = NULL;
static const char *s_project_id  = NULL;
static const char *s_api_secret  = NULL;
static const char *s_ca_cert     = NULL;   /* pinned cert; NULL = CA bundle */

/******************************************************************
 * 4. HTTP helper
 ******************************************************************/

/**
 * @brief POST a JSON-RPC body to the configured endpoint over HTTPS.
 *
 * Thin wrapper over @ref https_post_json that supplies this module's endpoint,
 * optional Infura credentials and optional pinned certificate.
 *
 * @param[in]  body          JSON request body (NUL-terminated).
 * @param[out] resp_buf      Response buffer, NUL-terminated on return.
 * @param[in]  resp_buf_size Capacity of @p resp_buf.
 * @return true on an HTTP 200 with a non-empty body, false otherwise.
 */
static bool do_post(const char *body, char *resp_buf, size_t resp_buf_size)
{
    return https_post_json(s_rpc_url, body, resp_buf, resp_buf_size,
                           s_project_id, s_api_secret, s_ca_cert);
}

/******************************************************************
 * 5. Hex utilities
 ******************************************************************/

/**
 * @brief Convert a nibble value to its lowercase ASCII hex digit.
 *
 * @param[in] n Nibble value; only the range 0–15 is meaningful.
 * @return '0'–'9' or 'a'–'f'.
 */
static char hex_nibble(uint8_t n)
{
    return (n < 10U) ? static_cast<char>('0' + n)
                     : static_cast<char>('a' + n - 10U);
}

/**
 * @brief Hex-encode a byte buffer (lowercase, no prefix, no NUL).
 *
 * @param[in]  data Input bytes.
 * @param[in]  len  Number of input bytes.
 * @param[out] out  Output buffer of at least 2*len chars; not NUL-terminated.
 */
static void bytes_to_hex(const uint8_t *data, size_t len, char *out)
{
    size_t i;
    for (i = 0U; i < len; i++) {
        out[i * HEX_PER_BYTE]       = hex_nibble((data[i] >> 4U) & 0x0FU);
        out[i * HEX_PER_BYTE + 1U]  = hex_nibble(data[i] & 0x0FU);
    }
}

/* The JSON-RPC "result" string extractor lives in eth_json.cpp (a pure,
 * host-fuzzable unit — see fuzz/fuzz_eth_rpc_json.cpp). */

/******************************************************************
 * 7. Public API
 ******************************************************************/

void eth_rpc_init(const char *rpc_url, const char *from_addr)
{
    s_rpc_url   = rpc_url;
    s_from_addr = from_addr;
}

void eth_rpc_set_auth(const char *project_id, const char *api_secret)
{
    s_project_id = project_id;
    s_api_secret = api_secret;
}

void eth_rpc_set_ca_cert(const char *ca_pem)
{
    s_ca_cert = ca_pem;
}

bool eth_rpc_get_nonce(uint64_t *nonce_out)
{
    char body[256];
    (void)snprintf(body, sizeof(body),
                   "{\"jsonrpc\":\"2.0\",\"method\":\"eth_getTransactionCount\","
                   "\"params\":[\"%s\",\"pending\"],\"id\":1}",
                   s_from_addr);

    char resp[RESP_BUF_SIZE];
    if (!do_post(body, resp, sizeof(resp))) {
        return false;
    }

    char result[RESULT_STR_MAX];
    if (!eth_json_result_string(resp, result, sizeof(result))) {
        ESP_LOGE(TAG, "nonce: no result in: %.*s", RESP_LOG_MAX, resp);
        return false;
    }
    if (strncmp(result, "0x", 2U) != 0) {
        ESP_LOGE(TAG, "nonce: result not hex: %.*s", RESP_LOG_MAX, result);
        return false;
    }

    char *end = NULL;
    uint64_t nonce = strtoull(result + 2, &end, 16);
    if ((end == (result + 2)) || (*end != '\0')) {
        ESP_LOGE(TAG, "nonce: malformed hex: %.*s", RESP_LOG_MAX, result);
        return false;
    }
    /* strtoull saturates silently — reject absurd values outright. */
    if (nonce > NONCE_MAX) {
        ESP_LOGE(TAG, "nonce: out of range: %" PRIu64, nonce);
        return false;
    }

    *nonce_out = nonce;
    ESP_LOGI(TAG, "Nonce: %" PRIu64, nonce);
    return true;
}

eth_rpc_parity_result_t eth_rpc_ecrecover_parity(const uint8_t hash[32],
                                                 const uint8_t r[32],
                                                 const uint8_t s[32],
                                                 uint8_t *v_out)
{
    /* Track whether at least one eth_call produced a usable recovered
     * address, so the caller can distinguish "RPC down" from "address
     * mismatch". */
    bool got_recovered = false;

    /* from_addr without "0x" prefix for comparison */
    const char *from_hex = s_from_addr;
    if ((from_hex[0] == '0') && ((from_hex[1] == 'x') || (from_hex[1] == 'X'))) {
        from_hex += 2;
    }

    /* ecrecover precompile input: hash(32) || v_uint256(32) || r(32) || s(32) */
    uint8_t input[128];
    CW_Utils::secure_wipe(input, sizeof(input));
    (void)CW_Utils::safe_memcpy(input, sizeof(input), hash, 32U);
    /* v occupies the last byte of the second 32-byte slot (index 63) */
    (void)CW_Utils::safe_memcpy(input + 64U, sizeof(input) - 64U, r, 32U);
    (void)CW_Utils::safe_memcpy(input + 96U, sizeof(input) - 96U, s, 32U);

    /* Hex-encode the 128-byte input */
    char input_hex[257];
    bytes_to_hex(input, sizeof(input), input_hex);
    input_hex[256] = '\0';

    uint8_t v_raw;
    for (v_raw = 0U; v_raw < 2U; v_raw++) {
        /* Set v byte (27 or 28) in slot [32..63] last byte */
        input_hex[63U * HEX_PER_BYTE]      = hex_nibble(((27U + v_raw) >> 4U) & 0x0FU);
        input_hex[63U * HEX_PER_BYTE + 1U] = hex_nibble((27U + v_raw) & 0x0FU);

        char body[600];
        (void)snprintf(body, sizeof(body),
                       "{\"jsonrpc\":\"2.0\",\"method\":\"eth_call\","
                       "\"params\":[{\"to\":"
                       "\"0x0000000000000000000000000000000000000001\","
                       "\"data\":\"0x%s\"},\"latest\"],\"id\":3}",
                       input_hex);

        char resp[RESP_BUF_SIZE];
        if (!do_post(body, resp, sizeof(resp))) {
            continue;
        }

        /* Expected result: "0x" + 64 hex chars (32-byte ABI-encoded address).
         * The address occupies the last 40 hex chars (bytes 12-31). */
        char result[RESULT_STR_MAX];
        if (!eth_json_result_string(resp, result, sizeof(result))) {
            ESP_LOGW(TAG, "ecrecover v=%u: no result in: %.*s",
                     v_raw, RESP_LOG_MAX, resp);
            continue;
        }
        if ((strncmp(result, "0x", 2U) != 0) || (strlen(result) != 66U)) {
            /* Empty/short result — address not recovered (try other v) */
            continue;
        }
        got_recovered = true;

        /* ABI address: 24 hex chars of zeros + 40 hex chars of address */
        const char *recovered_hex = result + 2U + 24U;

        if (strncasecmp(recovered_hex, from_hex, 40U) == 0) {
            ESP_LOGI(TAG, "v=%u matched ecrecover", v_raw);
            *v_out = v_raw;
            return ETH_RPC_PARITY_OK;
        }
    }

    /* no silent v=0 fallback — broadcasting with a wrong parity just
     * produces an invalid signature and an opaque failure downstream. */
    if (got_recovered) {
        ESP_LOGE(TAG, "ecrecover: neither parity matches %s "
                      "(wrong address or wrong derivation path?)", from_hex);
        return ETH_RPC_PARITY_MISMATCH;
    }
    ESP_LOGE(TAG, "ecrecover: no usable RPC response for either parity");
    return ETH_RPC_PARITY_RPC_ERROR;
}

bool eth_rpc_send_raw_tx(const uint8_t *tx, size_t tx_len,
                          char *tx_hash_out, size_t tx_hash_max,
                          char *err_out, size_t err_max)
{
    /* Cleared up front so every failure path below leaves a defined value: the
     * caller distinguishes "the node said why" from "we never got that far" by
     * whether this is empty, and a stale message from a previous sale would be
     * worse than none at all. */
    if ((err_out != NULL) && (err_max > 0U)) { err_out[0] = '\0'; }

    /* "0x" + 2 hex chars per byte + NUL */
    size_t hex_str_size = 2U + tx_len * HEX_PER_BYTE + 1U;
    char *tx_hex = static_cast<char *>(malloc(hex_str_size));
    if (tx_hex == NULL) { return false; }

    tx_hex[0] = '0';
    tx_hex[1] = 'x';
    bytes_to_hex(tx, tx_len, tx_hex + 2U);
    tx_hex[hex_str_size - 1U] = '\0';

    /* JSON body */
    size_t body_size = hex_str_size + 128U;
    char *body = static_cast<char *>(malloc(body_size));
    if (body == NULL) { free(tx_hex); return false; }

    (void)snprintf(body, body_size,
                   "{\"jsonrpc\":\"2.0\",\"method\":\"eth_sendRawTransaction\","
                   "\"params\":[\"%s\"],\"id\":2}",
                   tx_hex);
    free(tx_hex);

    char resp[RESP_BUF_SIZE];
    bool ok = do_post(body, resp, sizeof(resp));
    free(body);

    if (!ok) { return false; }

    /* Extract the "result" string (the tx hash) with a real JSON parse, so
     * a JSON-RPC error object is reported as a failure. */
    char result[RESULT_STR_MAX];
    if (!eth_json_result_string(resp, result, sizeof(result))) {
        ESP_LOGE(TAG, "send_raw_tx: no result in: %.*s", RESP_LOG_MAX, resp);
        /* A refusal, as opposed to a malformed body, carries the node's reason —
         * the one thing that tells an operator whether to top up gas, lower the
         * amount or just try again. Hand it back rather than log it and forget. */
        if ((err_out != NULL) && (err_max > 0U)) {
            (void)eth_json_error_message(resp, err_out, err_max);
        }
        return false;
    }
    if (strncmp(result, "0x", 2U) != 0) {
        ESP_LOGE(TAG, "send_raw_tx: result not a hash: %.*s",
                 RESP_LOG_MAX, result);
        return false;
    }

    size_t hash_len = strlen(result);
    if ((hash_len + 1U) > tx_hash_max) { return false; }

    (void)CW_Utils::safe_memcpy(reinterpret_cast<uint8_t *>(tx_hash_out),
                                tx_hash_max,
                                reinterpret_cast<const uint8_t *>(result),
                                hash_len);
    tx_hash_out[hash_len] = '\0';
    ESP_LOGI(TAG, "Tx hash: %s", tx_hash_out);
    return true;
}

eth_rpc_receipt_result_t eth_rpc_get_tx_receipt(const char *tx_hash)
{
    char body[160];
    (void)snprintf(body, sizeof(body),
                   "{\"jsonrpc\":\"2.0\",\"method\":\"eth_getTransactionReceipt\","
                   "\"params\":[\"%s\"],\"id\":3}",
                   tx_hash);

    /* Receipts are large (the logsBloom field alone is 512 hex chars, plus
     * the ERC-20 Transfer log) — use a dedicated heap buffer, a truncated
     * body would fail the JSON parse. */
    const size_t resp_size = 4096U;
    char *resp = static_cast<char *>(malloc(resp_size));
    if (resp == NULL) { return ETH_RPC_RECEIPT_RPC_ERROR; }

    eth_rpc_receipt_result_t verdict = ETH_RPC_RECEIPT_RPC_ERROR;
    if (do_post(body, resp, resp_size)) {
        switch (eth_json_receipt_status(resp)) {
            case ETH_JSON_RECEIPT_PENDING:  verdict = ETH_RPC_RECEIPT_PENDING;  break;
            case ETH_JSON_RECEIPT_SUCCESS:  verdict = ETH_RPC_RECEIPT_SUCCESS;  break;
            case ETH_JSON_RECEIPT_REVERTED: verdict = ETH_RPC_RECEIPT_REVERTED; break;
            case ETH_JSON_RECEIPT_ERROR:    /* fall through */
            default:                        verdict = ETH_RPC_RECEIPT_RPC_ERROR; break;
        }
    }
    free(resp);
    return verdict;
}
