/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file net.h
 * @ingroup device
 * @brief Network bring-up: Wi-Fi station (init/scan/connect/RSSI) and SNTP
 *        time sync.  No application-protocol logic lives here — the Ethereum
 *        JSON-RPC client is in eth_rpc.h.
 */

#ifndef NET_H
#define NET_H

/******************************************************************
 * 1. Included files
 ******************************************************************/

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/******************************************************************
 * 2. Types
 ******************************************************************/

/** @brief A scanned access point (subset of fields the UI needs). */
typedef struct {
    char   ssid[33];   /**< NUL-terminated SSID (max 32 chars + NUL). */
    int8_t rssi;       /**< Signal strength, dBm (closer to 0 = stronger). */
    bool   open;       /**< true if the network is open (no password). */
} net_wifi_ap_t;

/******************************************************************
 * 3. Public API
 ******************************************************************/

/**
 * @brief Bring up the WiFi driver in station mode (idempotent).
 *
 * Initialises netif, the event loop and the WiFi driver and starts the STA.
 * Call once before scanning or connecting. Safe to call repeatedly.
 */
void net_wifi_init(void);

/**
 * @brief Scan for nearby access points (blocking).
 *
 * @param[out] out  Array to fill with de-duplicated APs.
 * @param[in]  max  Capacity of @p out.
 * @return number of APs written (0 on error or none found).
 */
uint16_t net_wifi_scan(net_wifi_ap_t *out, uint16_t max);

/**
 * @brief Connect to a WiFi network and block until an IP is obtained
 *        (up to 30 s). May be called repeatedly to switch networks.
 *
 * @param[in] ssid     Network SSID.
 * @param[in] password Passphrase (empty string for an open network).
 * @return true on success, false on timeout or repeated association failure.
 */
bool net_wifi_connect(const char *ssid, const char *password);

/**
 * @brief Raise a WPA2 SoftAP beside the station, for phone-based setup.
 *
 * Switches the radio to APSTA so provisioning survives a station connect. One
 * radio, one channel: when the station later associates, the SoftAP is dragged
 * onto the station's channel and any joined phone is dropped. That is expected —
 * the setup page warns before it submits Wi-Fi credentials, and the device
 * screen, not the phone, reports the outcome.
 *
 * @param[in] ssid AP SSID.
 * @param[in] pass WPA2 passphrase; must be at least 8 characters.
 * @return true once the AP interface is configured and up.
 */
bool net_ap_start(const char *ssid, const char *pass);

/** @brief Drop the SoftAP and return the radio to station-only. */
void net_ap_stop(void);

/**
 * @brief Read the RSSI of the currently associated access point.
 *
 * @param[out] rssi_out Signal strength in dBm (closer to 0 = stronger);
 *                      untouched when not associated.
 * @return true if associated and @p rssi_out was written, false otherwise.
 */
bool net_wifi_rssi(int8_t *rssi_out);

/**
 * @brief Block until the system clock has been set via SNTP and sanity-checked.
 *
 * Must be called after net_wifi_connect() and before any HTTPS request:
 * without real time, TLS certificate validity-period checks are meaningless.
 * Those checks are only actually compiled in when CONFIG_MBEDTLS_HAVE_TIME_DATE
 * is set (see sdkconfig.defaults) — HAVE_TIME alone is not enough.
 * SNTP keeps running in the background for periodic resyncs.
 *
 * Callable repeatedly: each call re-subscribes and waits for a fresh packet, so
 * it doubles as a probe for whether the network just joined reaches the internet.
 *
 * SNTP is unauthenticated, so a synced time earlier than the firmware's own
 * build timestamp is rejected: back-dating is what an attacker needs to make
 * an expired certificate look valid again. Moving the clock forward is
 * harmless and is not restricted. A rejection returns false like any other
 * failure, so the caller's existing "no network time" path applies.
 *
 * @param[in] timeout_ms Maximum time to wait for the sync.
 * @return true once the clock is set and passes the lower-bound check, false on
 *         init error, timeout, or a back-dated clock.
 */
bool net_time_sync(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif // NET_H
