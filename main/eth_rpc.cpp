/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file eth_rpc.cpp
 * @brief WiFi bring-up, SNTP sync and Ethereum JSON-RPC client implementation.
 */

/******************************************************************
 * 1. Included files
 ******************************************************************/

#include "eth_rpc.h"

#include <string.h>
#include <strings.h>    /* strncasecmp */
#include <stdlib.h>     /* strtoull, malloc, free */
#include <stdio.h>      /* snprintf */
#include <inttypes.h>   /* PRIu64 */

/* CW_Utils.h pulls in Arduino.h (via platform_compat.h); it must come before
 * any lwip-including IDF header (esp_http_client.h, esp_netif.h, ...) so that
 * IPAddress.h declares INADDR_NONE before lwip defines it as a macro. */
#include "CW_Utils.h"   /* hardened memory primitives (CODING_RULES §1.4) */

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

static const char *const TAG = "eth_rpc";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_MAX_RETRY      5
#define WIFI_TIMEOUT_MS     30000

/* JSON-RPC response buffer — large enough for any expected response. */
#define RESP_BUF_SIZE  1024U

/* Hex chars per byte */
#define HEX_PER_BYTE  2U

/* F-12: never dump full RPC responses (they can echo credentials embedded
 * in the URL) — log at most this many bytes on parse failures. */
#define RESP_LOG_MAX  80

/* F-09: sanity bound for the account nonce — a real terminal never gets
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

static EventGroupHandle_t s_wifi_event_group = NULL;
static int                s_retry_num         = 0;
static bool               s_wifi_inited       = false;

/******************************************************************
 * 3. WiFi event handler
 ******************************************************************/

/**
 * @brief WiFi/IP event handler driving the connect retry state machine.
 *
 * Retries the association up to @ref WIFI_MAX_RETRY times, then signals
 * @ref WIFI_FAIL_BIT; signals @ref WIFI_CONNECTED_BIT once an IP is bound.
 *
 * @param[in] arg        Unused.
 * @param[in] event_base Event base (WIFI_EVENT or IP_EVENT).
 * @param[in] event_id   Event identifier within @p event_base.
 * @param[in] event_data Unused.
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if ((event_base == WIFI_EVENT) && (event_id == WIFI_EVENT_STA_START)) {
        /* Started — association is initiated explicitly by
         * eth_rpc_wifi_connect(), not here, so a start with no credentials
         * (e.g. before provisioning) doesn't churn through retries. */
    } else if ((event_base == WIFI_EVENT) &&
               (event_id == WIFI_EVENT_STA_DISCONNECTED)) {
        if (s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG, "WiFi retry %d/%d", s_retry_num, WIFI_MAX_RETRY);
        } else if (s_wifi_event_group != NULL) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if ((event_base == IP_EVENT) && (event_id == IP_EVENT_STA_GOT_IP)) {
        s_retry_num = 0;
        if (s_wifi_event_group != NULL) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
    } else {
        /* other events ignored */
    }
}

/******************************************************************
 * 4. HTTP helper
 ******************************************************************/

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
 *         HTTP 200 (F-09); false on transport error or non-200 status.
 */
static bool do_post(const char *body, char *resp_buf, size_t resp_buf_size)
{
    bool success = false;

    bool use_auth = ((s_project_id != NULL) && (s_project_id[0] != '\0') &&
                     (s_api_secret != NULL) && (s_api_secret[0] != '\0'));

    esp_http_client_config_t cfg;
    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.url               = s_rpc_url;
    cfg.method            = HTTP_METHOD_POST;
    cfg.timeout_ms        = 15000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
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

        int total = 0;
        int read;
        do {
            int space = static_cast<int>(resp_buf_size - 1U) - total;
            if (space <= 0) { break; }
            read = esp_http_client_read(client, resp_buf + total, space);
            if (read > 0) { total += read; }
        } while (read > 0);

        resp_buf[total] = '\0';

        /* F-09: a 4xx/5xx body that happens to contain "result" must not
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

/******************************************************************
 * 6. JSON helper
 ******************************************************************/

/**
 * @brief Extract the top-level @c "result" string from a JSON-RPC response.
 *
 * Uses a real JSON parser (cJSON) instead of strstr so that JSON-RPC error
 * objects, HTTP error bodies, or look-alike substrings are rejected (F-09).
 *
 * @param[in]  resp     NUL-terminated response body.
 * @param[out] out      NUL-terminated "result" value on success; untouched
 *                      on failure.
 * @param[in]  out_size Capacity of @p out.
 * @return true on success; false if the body is not valid JSON, "result"
 *         is absent or not a string, or the value does not fit in @p out.
 */
static bool json_get_result_string(const char *resp, char *out, size_t out_size)
{
    bool ok = false;
    cJSON *root = cJSON_Parse(resp);
    if (root == NULL) {
        return false;
    }
    const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (cJSON_IsString(result) && (result->valuestring != NULL)) {
        size_t len = strlen(result->valuestring);
        if ((len + 1U) <= out_size) {
            ok = CW_Utils::safe_memcpy(
                reinterpret_cast<uint8_t *>(out), out_size,
                reinterpret_cast<const uint8_t *>(result->valuestring),
                len + 1U);
        }
    }
    cJSON_Delete(root);
    return ok;
}

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

bool eth_rpc_time_sync(uint32_t timeout_ms)
{
    /* Several reliable servers — phone hotspots often slow or drop NTP (UDP
     * 123) to pool.ntp.org, so fall back to Google / Cloudflare time. */
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(3,
        ESP_SNTP_SERVER_LIST("time.google.com", "pool.ntp.org", "time.cloudflare.com"));
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SNTP init: %s", esp_err_to_name(err));
        return false;
    }

    /* Poll across the budget so a slow first response doesn't fail us. */
    const int   attempts = 3;
    uint32_t    per_wait = timeout_ms / static_cast<uint32_t>(attempts);
    if (per_wait < 2000U) { per_wait = 2000U; }
    bool synced = false;
    for (int i = 0; (i < attempts) && !synced; i++) {
        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(per_wait)) == ESP_OK) {
            synced = true;
        } else {
            ESP_LOGW(TAG, "SNTP: no time yet (%d/%d)", i + 1, attempts);
        }
    }

    if (!synced) {
        ESP_LOGE(TAG, "SNTP sync timed out");
        esp_netif_sntp_deinit();
        return false;
    }

    ESP_LOGI(TAG, "System time synced via SNTP");
    /* Leave SNTP running for periodic background resyncs. */
    return true;
}

void eth_rpc_wifi_init(void)
{
    if (s_wifi_inited) { return; }

    s_wifi_event_group = xEventGroupCreate();
    s_retry_num = 0;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    (void)esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Persistent handlers (never unregistered) so init/connect can repeat. */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_wifi_inited = true;
}

uint16_t eth_rpc_wifi_scan(eth_wifi_ap_t *out, uint16_t max)
{
    eth_rpc_wifi_init();
    if ((out == NULL) || (max == 0U)) { return 0U; }

    wifi_scan_config_t scan_cfg;
    (void)memset(&scan_cfg, 0, sizeof(scan_cfg));   /* all channels, all SSIDs */
    if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK) {
        return 0U;
    }

    uint16_t num = 0U;
    (void)esp_wifi_scan_get_ap_num(&num);
    if (num == 0U) { return 0U; }

    wifi_ap_record_t *recs =
        static_cast<wifi_ap_record_t *>(malloc(num * sizeof(wifi_ap_record_t)));
    if (recs == NULL) { return 0U; }
    (void)esp_wifi_scan_get_ap_records(&num, recs);

    uint16_t count = 0U;
    for (uint16_t i = 0U; (i < num) && (count < max); i++) {
        const char *ssid = reinterpret_cast<const char *>(recs[i].ssid);
        if (ssid[0] == '\0') { continue; }          /* hidden network */

        bool dup = false;                            /* one entry per SSID */
        for (uint16_t j = 0U; j < count; j++) {
            if (strcmp(out[j].ssid, ssid) == 0) { dup = true; break; }
        }
        if (dup) { continue; }

        (void)strncpy(out[count].ssid, ssid, sizeof(out[count].ssid) - 1U);
        out[count].ssid[sizeof(out[count].ssid) - 1U] = '\0';
        out[count].rssi = recs[i].rssi;
        out[count].open = (recs[i].authmode == WIFI_AUTH_OPEN);
        count++;
    }

    free(recs);
    return count;
}

bool eth_rpc_wifi_connect(const char *ssid, const char *password)
{
    eth_rpc_wifi_init();

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_num = 0;

    wifi_config_t wifi_cfg;
    (void)memset(&wifi_cfg, 0, sizeof(wifi_cfg));
    (void)strncpy(reinterpret_cast<char *>(wifi_cfg.sta.ssid),     ssid,     sizeof(wifi_cfg.sta.ssid)     - 1U);
    (void)strncpy(reinterpret_cast<char *>(wifi_cfg.sta.password), password, sizeof(wifi_cfg.sta.password) - 1U);
    /* Open networks have an empty passphrase; otherwise require WPA2+. */
    wifi_cfg.sta.threshold.authmode =
        (password[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    /* The WiFi driver keeps its own copy — scrub the stack copy of the
     * credentials immediately (CODING_RULES §1.4). */
    CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(&wifi_cfg),
                          sizeof(wifi_cfg));

    (void)esp_wifi_disconnect();   /* drop any current AP before (re)connecting */
    (void)esp_wifi_connect();

    ESP_LOGI(TAG, "Connecting to \"%s\"...", ssid);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(WIFI_TIMEOUT_MS));

    bool connected = ((bits & WIFI_CONNECTED_BIT) != 0U);
    if (connected) {
        ESP_LOGI(TAG, "WiFi connected");
    } else {
        ESP_LOGE(TAG, "WiFi connect failed");
    }
    return connected;
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
    if (!json_get_result_string(resp, result, sizeof(result))) {
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
    /* F-09: strtoull saturates silently — reject absurd values outright. */
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
     * mismatch" (F-10). */
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
        if (!json_get_result_string(resp, result, sizeof(result))) {
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

    /* F-10: no silent v=0 fallback — broadcasting with a wrong parity just
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
     * a JSON-RPC error object is reported as a failure (F-09). */
    char result[RESULT_STR_MAX];
    if (!json_get_result_string(resp, result, sizeof(result))) {
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
        cJSON *root = cJSON_Parse(resp);
        if (root != NULL) {
            const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
            if (cJSON_IsNull(result)) {
                verdict = ETH_RPC_RECEIPT_PENDING;   /* not mined yet */
            } else if (cJSON_IsObject(result)) {
                const cJSON *status =
                    cJSON_GetObjectItemCaseSensitive(result, "status");
                if (cJSON_IsString(status) && (status->valuestring != NULL)) {
                    verdict = (strcmp(status->valuestring, "0x1") == 0)
                                  ? ETH_RPC_RECEIPT_SUCCESS
                                  : ETH_RPC_RECEIPT_REVERTED;
                }
            }
            cJSON_Delete(root);
        }
    }
    free(resp);
    return verdict;
}
