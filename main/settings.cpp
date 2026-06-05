/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file settings.cpp
 * @brief NVS-backed implementation of the persistent device settings.
 */

#include "settings.h"

#include <string.h>
#include "nvs.h"
#include "esp_log.h"

static const char *const TAG = "settings";

#define NS_SETTINGS   "settings"
#define K_BRIGHTNESS  "bright"
#define K_WIFI_SSID   "wifi_ssid"
#define K_WIFI_PASS   "wifi_pass"

#define DEFAULT_BRIGHTNESS  80U

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
