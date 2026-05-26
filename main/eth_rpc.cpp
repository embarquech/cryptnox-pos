#include "eth_rpc.h"

#include <string.h>
#include <strings.h>    /* strncasecmp */
#include <stdlib.h>     /* strtoull, malloc, free */
#include <stdio.h>      /* snprintf */
#include <inttypes.h>   /* PRIu64 */

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

static const char *const TAG = "eth_rpc";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_MAX_RETRY      5
#define WIFI_TIMEOUT_MS     30000

/* JSON-RPC response buffer — large enough for any expected response. */
#define RESP_BUF_SIZE  1024U

/* Hex chars per byte */
#define HEX_PER_BYTE  2U

/******************************************************************
 * Module state
 ******************************************************************/

static const char *s_rpc_url   = NULL;
static const char *s_from_addr = NULL;

static EventGroupHandle_t s_wifi_event_group;
static int                s_retry_num = 0;

/******************************************************************
 * WiFi event handler
 ******************************************************************/

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if ((event_base == WIFI_EVENT) && (event_id == WIFI_EVENT_STA_START)) {
        esp_wifi_connect();
    } else if ((event_base == WIFI_EVENT) &&
               (event_id == WIFI_EVENT_STA_DISCONNECTED)) {
        if (s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG, "WiFi retry %d/%d", s_retry_num, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if ((event_base == IP_EVENT) && (event_id == IP_EVENT_STA_GOT_IP)) {
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else {
        /* other events ignored */
    }
}

/******************************************************************
 * HTTP helper
 ******************************************************************/

/*
 * POST 'body' to s_rpc_url over HTTPS and read the response into
 * resp_buf (NUL-terminated on success).  Returns true if data was read.
 */
static bool do_post(const char *body, char *resp_buf, size_t resp_buf_size)
{
    bool success = false;

    esp_http_client_config_t cfg;
    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.url               = s_rpc_url;
    cfg.method            = HTTP_METHOD_POST;
    cfg.timeout_ms        = 15000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "HTTP client init failed");
        return false;
    }

    (void)esp_http_client_set_header(client, "Content-Type", "application/json");

    int body_len = (int)strlen(body);
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
            int space = (int)(resp_buf_size - 1U) - total;
            if (space <= 0) { break; }
            read = esp_http_client_read(client, resp_buf + total, space);
            if (read > 0) { total += read; }
        } while (read > 0);

        resp_buf[total] = '\0';
        success = (total > 0);
    }

cleanup:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return success;
}

/******************************************************************
 * Hex utilities
 ******************************************************************/

static char hex_nibble(uint8_t n)
{
    return (n < 10U) ? (char)('0' + n) : (char)('a' + n - 10U);
}

static void bytes_to_hex(const uint8_t *data, size_t len, char *out)
{
    size_t i;
    for (i = 0U; i < len; i++) {
        out[i * HEX_PER_BYTE]       = hex_nibble((data[i] >> 4U) & 0x0FU);
        out[i * HEX_PER_BYTE + 1U]  = hex_nibble(data[i] & 0x0FU);
    }
}

/******************************************************************
 * Public API
 ******************************************************************/

void eth_rpc_init(const char *rpc_url, const char *from_addr)
{
    s_rpc_url   = rpc_url;
    s_from_addr = from_addr;
}

bool eth_rpc_wifi_connect(const char *ssid, const char *password)
{
    s_wifi_event_group = xEventGroupCreate();
    s_retry_num = 0;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    (void)esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t h_any;
    esp_event_handler_instance_t h_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &h_any));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &h_ip));

    wifi_config_t wifi_cfg;
    (void)memset(&wifi_cfg, 0, sizeof(wifi_cfg));
    (void)strncpy((char *)wifi_cfg.sta.ssid,     ssid,     sizeof(wifi_cfg.sta.ssid)     - 1U);
    (void)strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1U);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

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

    (void)esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, h_ip);
    (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, h_any);
    vEventGroupDelete(s_wifi_event_group);

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

    /* Extract hex value after "result":"0x */
    char *result = strstr(resp, "\"result\":\"0x");
    if (result == NULL) {
        ESP_LOGE(TAG, "nonce: no result in: %s", resp);
        return false;
    }
    result += 12;  /* skip past "result":"0x */

    *nonce_out = strtoull(result, NULL, 16);
    ESP_LOGI(TAG, "Nonce: %" PRIu64, *nonce_out);
    return true;
}

uint8_t eth_rpc_ecrecover_parity(const uint8_t hash[32],
                                  const uint8_t r[32],
                                  const uint8_t s[32])
{
    /* ecrecover precompile input: hash(32) || v_uint256(32) || r(32) || s(32) */
    uint8_t input[128];
    (void)memset(input, 0, sizeof(input));
    (void)memcpy(input, hash, 32U);
    /* v occupies the last byte of the second 32-byte slot (index 63) */
    (void)memcpy(input + 64U, r, 32U);
    (void)memcpy(input + 96U, s, 32U);

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

        /* Response: "result":"0x" + 64 hex chars (32 bytes ABI address).
         * The address occupies the last 40 hex chars (bytes 12-31). */
        char *result = strstr(resp, "\"result\":\"0x");
        if (result == NULL) { continue; }
        result += 12U;  /* skip to hex digits */

        size_t result_hex_len = 0U;
        {
            const char *p = result;
            while ((*p != '"') && (*p != '\0') && (*p != ',')) {
                p++;
                result_hex_len++;
            }
        }

        if (result_hex_len < 64U) {
            /* Empty result — address not recovered (try other v) */
            continue;
        }

        /* ABI address: 24 hex chars of zeros + 40 hex chars of address */
        const char *recovered_hex = result + 24U;

        if (strncasecmp(recovered_hex, from_hex, 40U) == 0) {
            ESP_LOGI(TAG, "v=%u matched ecrecover", v_raw);
            return v_raw;
        }
    }

    ESP_LOGW(TAG, "ecrecover did not match either parity, defaulting v=0");
    return 0U;
}

bool eth_rpc_send_raw_tx(const uint8_t *tx, size_t tx_len,
                          char *tx_hash_out, size_t tx_hash_max)
{
    /* "0x" + 2 hex chars per byte + NUL */
    size_t hex_str_size = 2U + tx_len * HEX_PER_BYTE + 1U;
    char *tx_hex = (char *)malloc(hex_str_size);
    if (tx_hex == NULL) { return false; }

    tx_hex[0] = '0';
    tx_hex[1] = 'x';
    bytes_to_hex(tx, tx_len, tx_hex + 2U);
    tx_hex[hex_str_size - 1U] = '\0';

    /* JSON body */
    size_t body_size = hex_str_size + 128U;
    char *body = (char *)malloc(body_size);
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

    /* Extract "result":"0x..." (the tx hash) */
    char *result = strstr(resp, "\"result\":\"");
    if (result == NULL) {
        ESP_LOGE(TAG, "send_raw_tx: no result in: %s", resp);
        return false;
    }
    result += 10U;  /* skip past "result":" — now at '0x...' */

    char *end = strchr(result, '"');
    if (end == NULL) { return false; }

    size_t hash_len = (size_t)(end - result);
    if ((hash_len + 1U) > tx_hash_max) { return false; }

    (void)memcpy(tx_hash_out, result, hash_len);
    tx_hash_out[hash_len] = '\0';
    return true;
}
