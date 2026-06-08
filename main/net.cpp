/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file net.cpp
 * @brief Wi-Fi station bring-up (init/scan/connect/RSSI) and SNTP time sync.
 */

/******************************************************************
 * 1. Included files
 ******************************************************************/

#include "net.h"

#include <string.h>
#include <stdlib.h>     /* malloc, free */

/* CW_Utils.h pulls in Arduino.h (via platform_compat.h); it must come before
 * any lwip-including IDF header (esp_netif.h, ...) so that IPAddress.h
 * declares INADDR_NONE before lwip defines it as a macro. */
#include "CW_Utils.h"   /* hardened memory primitives (CODING_RULES §1.4) */

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"

static const char *const TAG = "net";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_MAX_RETRY      5
#define WIFI_TIMEOUT_MS     30000

/******************************************************************
 * 2. Module state
 ******************************************************************/

static EventGroupHandle_t s_wifi_event_group = NULL;
static int                s_retry_num        = 0;
static bool               s_wifi_inited      = false;

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
         * net_wifi_connect(), not here, so a start with no credentials
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
 * 4. Public API
 ******************************************************************/

void net_wifi_init(void)
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

uint16_t net_wifi_scan(net_wifi_ap_t *out, uint16_t max)
{
    net_wifi_init();
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

bool net_wifi_connect(const char *ssid, const char *password)
{
    net_wifi_init();

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

bool net_wifi_rssi(int8_t *rssi_out)
{
    if (rssi_out == NULL) { return false; }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return false;   /* not associated */
    }
    *rssi_out = ap.rssi;
    return true;
}

bool net_time_sync(uint32_t timeout_ms)
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
