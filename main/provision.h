/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file provision.h
 * @ingroup device
 * @brief Phone-based setup: a WPA2 SoftAP, a captive portal that opens itself,
 *        and one form per first-run step.
 *
 * Why a captive portal and not "browse to 192.168.4.1": typing a 42-character
 * recipient address on a 240x320 resistive panel is the worst part of setting
 * one of these up, and telling an operator to type an IP address is only
 * marginally better. Scanning a QR code should land them on the form.
 *
 * That takes two servers, not one. A DNS responder answers every lookup with
 * the AP's own address, and the HTTP server deliberately fails each OS's
 * connectivity probe so the phone concludes the network is captive and pops the
 * browser by itself. Miss either half and the phone joins the AP in silence.
 *
 * Threat model. The forms run over plain HTTP — a captive portal cannot serve
 * TLS without a certificate warning that would train operators to click through
 * exactly the warning that matters. So the AP passphrase is the only thing
 * between a stranger in radio range and the form that says where the money goes.
 * It is random per device, shown on the screen, and the AP only runs during
 * setup. On top of that, a submitted payout address is not committed until it is
 * shown on the device screen and accepted there — a phone can propose an
 * address, only the panel can confirm one.
 */

#ifndef PROVISION_H
#define PROVISION_H

#include <stdbool.h>
#include <stddef.h>

#include "ui.h"   /* ui_event_cb_t — submissions are reported as UI events */

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Which question the setup page is currently asking. */
typedef enum {
    PROV_STEP_IDLE,   /**< Nothing to set — the page says setup is done.    */
    PROV_STEP_ADMIN,  /**< Create the admin code.                           */
    PROV_STEP_WIFI,   /**< Join the venue's Wi-Fi.                          */
    PROV_STEP_ADDR,   /**< Set the payout addresses.                        */
} prov_step_t;

/**
 * @brief Raise the SoftAP and start the portal.
 *
 * Idempotent. The AP passphrase is generated once and kept in NVS: the operator
 * may well have photographed the QR code, and a reboot mid-setup should not
 * invalidate it.
 *
 * @param[in] cb Where submissions are reported; the same callback the UI task
 *               uses, so a form and a screen tap are indistinguishable to the
 *               main task. Must outlive the call.
 * @return true if the AP and both servers came up.
 */
bool prov_start(ui_event_cb_t cb);

/** @brief Stop the portal and drop the AP. Safe if never started. */
void prov_stop(void);

/** @brief Tell the portal which form to serve. */
void prov_set_step(prov_step_t step);

/** @brief AP SSID, or "" before prov_start(). Valid until prov_stop(). */
const char *prov_ap_ssid(void);

/** @brief AP passphrase, or "" before prov_start(). */
const char *prov_ap_pass(void);

/**
 * @brief The joining QR payload, "WIFI:T:WPA;S:<ssid>;P:<pass>;;".
 *
 * Both iOS and Android cameras join a network directly from this, which is why
 * one code is enough: the portal takes over from there.
 */
const char *prov_qr_payload(void);

/**
 * @brief Fetch the payout address a phone has proposed but nobody has accepted.
 *
 * @param[out] label   Human name of the network ("Ethereum"), may be NULL.
 * @param[in]  label_n Capacity of @p label.
 * @param[out] addr    The proposed address, may be NULL.
 * @param[in]  addr_n  Capacity of @p addr.
 * @return true if a proposal is pending.
 */
bool prov_addr_pending(char *label, size_t label_n, char *addr, size_t addr_n);

/**
 * @brief Resolve a pending payout-address proposal.
 *
 * @param[in] accept true to commit it to NVS, false to discard it.
 * @return true if an address was committed.
 */
bool prov_addr_commit(bool accept);

#ifdef __cplusplus
}
#endif

#endif /* PROVISION_H */
