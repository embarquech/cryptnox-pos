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
#include "civil_time.h"

#include <string.h>
#include <strings.h>    /* strncasecmp */
#include <stdlib.h>     /* strtoull, malloc, free */
#include <stdio.h>      /* snprintf */
#include <time.h>       /* time */
#include <inttypes.h>   /* PRIu64, PRId64 */

/* CW_Utils.h pulls in Arduino.h (via platform_compat.h); it must come before
 * any lwip-including IDF header (esp_http_client.h, esp_netif.h, ...) so that
 * IPAddress.h declares INADDR_NONE before lwip defines it as a macro. */
#include "CW_Utils.h"   /* hardened memory primitives (CODING_RULES §1.4) */

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

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

/* Tolerated disagreement between our clock and the server's Date header.
 * The header has 1 s granularity and provider clocks are NTP-synced, so only
 * request latency sits in between — 5 min is enormously generous while still
 * catching the real attack (back-dating to revive an expired cert moves the
 * clock by weeks). Same convention as Kerberos. */
#define CLOCK_SKEW_MAX_S  300

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

/** @brief Response headers captured during fetch (see @ref http_event_cb). */
typedef struct {
    char date[40];   /**< Date header value, "" if the server sent none.
                          IMF-fixdate is 29 chars; 40 leaves slack.        */
} rpc_resp_hdrs_t;

/**
 * @brief HTTP event hook that captures the response Date header.
 *
 * Response headers are ONLY reachable this way. esp_http_client_get_header()
 * looks up client->request->headers — the headers we send — so it can never
 * return the server's Date, however plausible the name looks.
 *
 * @param[in] evt Event; user_data points at the caller's rpc_resp_hdrs_t.
 * @return ESP_OK always (never fail a transfer over a header we merely want).
 */
static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    if ((evt == NULL) || (evt->event_id != HTTP_EVENT_ON_HEADER) ||
        (evt->user_data == NULL) || (evt->header_key == NULL)) {
        return ESP_OK;
    }

    /* Field names are case-insensitive (RFC 9110 §5.1). */
    if (strcasecmp(evt->header_key, "Date") == 0) {
        rpc_resp_hdrs_t *hdrs = static_cast<rpc_resp_hdrs_t *>(evt->user_data);
        const char      *val  = (evt->header_value != NULL) ? evt->header_value
                                                            : "";
        (void)strncpy(hdrs->date, val, sizeof(hdrs->date) - 1U);
        hdrs->date[sizeof(hdrs->date) - 1U] = '\0';
    }
    return ESP_OK;
}

/**
 * @brief Cross-check the system clock against the server's HTTP Date header.
 *
 * SNTP gave us the clock over plain unauthenticated UDP, so a network
 * attacker can dictate it. This header arrives inside the encrypted,
 * authenticated TLS channel — without the pinned CA's private key it can be
 * neither forged nor altered, which makes it a strictly better time source
 * than the handshake (ServerHello.random's gmt_unix_time is pure random in
 * TLS 1.3 and esp_http_client does not expose it anyway).
 *
 * Must be called after esp_http_client_fetch_headers().
 *
 * A missing or non-conforming header yields "not corroborated", not
 * "disagrees": the response already passed pinned-CA TLS, so an absent
 * header means the provider genuinely omitted it, and failing the payment
 * over that would be a self-inflicted outage. An attacker cannot induce
 * this case without breaking TLS.
 *
 * @param[in] date_hdr Captured Date value; "" when the server sent none.
 * @return false only when a parseable Date disagrees with the local clock by
 *         more than @ref CLOCK_SKEW_MAX_S; true otherwise.
 */
static bool clock_corroborated(const char *date_hdr)
{
    int64_t  server_epoch = 0;
    int64_t  local_epoch;
    int64_t  skew;

    if (date_hdr[0] == '\0') {
        ESP_LOGW(TAG, "no Date header - clock not corroborated");
        return true;
    }

    if (!civil_parse_http_date(date_hdr, &server_epoch)) {
        ESP_LOGW(TAG, "unparseable Date header - clock not corroborated");
        return true;
    }

    local_epoch = static_cast<int64_t>(time(NULL));
    skew = local_epoch - server_epoch;
    if (skew < 0) { skew = -skew; }

    if (skew > CLOCK_SKEW_MAX_S) {
        ESP_LOGE(TAG, "clock off by %" PRId64 " s vs server (local %" PRId64
                      ", server %" PRId64 ") - refusing (spoofed NTP?)",
                 skew, local_epoch, server_epoch);
        return false;
    }

    ESP_LOGD(TAG, "clock corroborated (skew %" PRId64 " s)", skew);
    return true;
}

/**
 * @brief POST a JSON-RPC body to the configured endpoint over HTTPS.
 *
 * Applies HTTP Basic Auth when credentials were set via
 * @ref eth_rpc_set_auth.  The response is read until EOF or buffer-full
 * and is always NUL-terminated.
 *
 * @param[in]  body          JSON request body (NUL-terminated).
 * @param[out] resp_buf      Response buffer, NUL-terminated on return.
 * @param[in]  resp_buf_size Capacity of @p resp_buf.
 * @return true only if at least one byte was read AND the server answered
 *         HTTP 200; false on transport error or non-200 status.
 */
static bool do_post(const char *body, char *resp_buf, size_t resp_buf_size)
{
    bool success = false;

    bool use_auth = ((s_project_id != NULL) && (s_project_id[0] != '\0') &&
                     (s_api_secret != NULL) && (s_api_secret[0] != '\0'));

    rpc_resp_hdrs_t hdrs;
    (void)memset(&hdrs, 0, sizeof(hdrs));

    esp_http_client_config_t cfg;
    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.url               = s_rpc_url;
    cfg.method            = HTTP_METHOD_POST;
    cfg.timeout_ms        = 15000;
    cfg.event_handler     = http_event_cb;   /* captures the Date header */
    cfg.user_data         = &hdrs;
    /* if a cert was pinned via eth_rpc_set_ca_cert(), trust ONLY it —
     * otherwise any of the ~150 CAs in the Mozilla bundle could MITM the RPC. */
    if (s_ca_cert != NULL) {
        cfg.cert_pem = s_ca_cert;
    } else {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
    if (use_auth) {
        cfg.username  = s_project_id;
        cfg.password  = s_api_secret;
        cfg.auth_type = HTTP_AUTH_TYPE_BASIC;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "HTTP client init failed");
        return false;
    }

    (void)esp_http_client_set_header(client, "Content-Type", "application/json");

    int body_len = static_cast<int>(strlen(body));
    esp_err_t err = esp_http_client_open(client, body_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open: %s", esp_err_to_name(err));
        goto cleanup;
    }

    if (esp_http_client_write(client, body, body_len) != body_len) {
        ESP_LOGE(TAG, "HTTP write incomplete");
        goto cleanup;
    }

    {
        int64_t content_length = esp_http_client_fetch_headers(client);
        (void)content_length;  /* may be -1 for chunked; we read until EOF */

        /* Reject the response before reading the body if the authenticated
         * Date proves our SNTP-supplied clock was spoofed. Consistent with
         * the existing no-network-time path: refuse rather than adopt the
         * header's time, so one wrong provider clock can never silently
         * redefine what this terminal treats as "now". */
        if (!clock_corroborated(hdrs.date)) {
            goto cleanup;   /* success stays false */
        }

        int total = 0;
        int read;
        do {
            int space = static_cast<int>(resp_buf_size - 1U) - total;
            if (space <= 0) { break; }
            read = esp_http_client_read(client, resp_buf + total, space);
            if (read > 0) { total += read; }
        } while (read > 0);

        resp_buf[total] = '\0';

        /* a 4xx/5xx body that happens to contain "result" must not
         * be mistaken for a successful JSON-RPC response. */
        int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            ESP_LOGE(TAG, "HTTP status %d: %.*s", status, RESP_LOG_MAX, resp_buf);
        }
        success = ((total > 0) && (status == 200));
    }

cleanup:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return success;
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

    /* ecrecover precompile input: hash(32) || v_uint256(32) || r(32) || s(32) */
    uint8_t input[128];
    (void)memset(input, 0, sizeof(input));
    (void)CW_Utils::safe_memcpy(input, sizeof(input), hash, 32U);
    /* v occupies the last byte of the second 32-byte slot (index 63) */
    (void)CW_Utils::safe_memcpy(input + 64U, sizeof(input) - 64U, r, 32U);
    (void)CW_Utils::safe_memcpy(input + 96U, sizeof(input) - 96U, s, 32U);

    /* Hex-encode the 128-byte input */
    char input_hex[257];
    bytes_to_hex(input, sizeof(input), input_hex);
    input_hex[256] = '\0';

    /* from_addr without "0x" prefix for comparison */
    const char *from_hex = s_from_addr;
    if ((from_hex[0] == '0') && ((from_hex[1] == 'x') || (from_hex[1] == 'X'))) {
        from_hex += 2;
    }

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
        ESP_LOGE(TAG, "ecrecover: neither parity matches from_addr "
                      "(ADDR_FROM / card mismatch?)");
        return ETH_RPC_PARITY_MISMATCH;
    }
    ESP_LOGE(TAG, "ecrecover: no usable RPC response for either parity");
    return ETH_RPC_PARITY_RPC_ERROR;
}

bool eth_rpc_send_raw_tx(const uint8_t *tx, size_t tx_len,
                          char *tx_hash_out, size_t tx_hash_max)
{
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
