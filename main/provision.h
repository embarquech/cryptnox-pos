/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file provision.h
 * @ingroup device
 * @brief The config portal: one web app for setting a terminal up and for
 *        administering it afterwards, in two modes.
 *
 * ## Why a portal at all
 *
 * Typing a 42-character recipient address on a 240x320 resistive panel is the
 * worst part of setting one of these up, and telling an operator to type an IP
 * address is only marginally better. Scanning a QR code should land them on the
 * form.
 *
 * ## The two modes
 *
 * @ref PROV_MODE_WIZARD — a blank terminal, or one whose saved network has become
 * unreachable. There is no network to serve a page on, so the terminal raises its
 * own WPA2 SoftAP and runs a captive portal: a DNS responder answers every lookup
 * with the AP's address, and the HTTP server deliberately fails each OS's
 * connectivity probe so the phone concludes the network is captive and opens the
 * browser by itself. Miss either half and the phone joins in silence. The QR code
 * on the panel carries "WIFI:T:WPA;S:...;P:...;;", which both iOS and Android
 * cameras join from directly — so there is never a URL to type.
 *
 * @ref PROV_MODE_ADMIN — a terminal already on the venue network, opened from
 * behind the admin code in the settings menu. Served over HTTPS on that network,
 * and the QR code on the panel is simply the https:// URL.
 *
 * ## Authorisation: the panel, never the wire
 *
 * The browser has no admin-code field. It asks to be let in, and the terminal
 * shows its admin-code screen; the operator types the code *on the panel*, and the
 * portal session becomes authorised. So the code never crosses the network in
 * either direction, on either mode's transport, and a browser that reaches the
 * page without the terminal in reach can do nothing at all.
 *
 * One exception, and it is the wizard's alone: @ref prov_set_wifi_only, the flow a
 * configured terminal gets when all it has lost is its network. See there.
 *
 * Everything that changes where money goes goes further than that: a browser may
 * *propose* a payout address or a token contract, and the value is then displayed
 * on the panel for somebody to accept there. Nothing is stored until they do.
 * Firmware follows the same rule — an upload is verified and staged, and only an
 * on-screen accept makes it bootable.
 *
 * ## Transport
 *
 * Admin mode is HTTPS (self-signed, per-device key generated on the terminal and
 * kept in NVS — so the browser warns once, and no two terminals share a key).
 *
 * Wizard mode is deliberately plain HTTP, and that is not an oversight:
 *   - A captive-portal probe fetches a bare http:// URL on port 80 and will not
 *     follow us to 443. TLS there means the browser never opens by itself, which
 *     is the entire point of the mode.
 *   - The link is already encrypted. It is a WPA2 SoftAP whose random per-device
 *     passphrase is on the panel in front of the operator, and it only exists
 *     during setup.
 *   - The admin code is not on that link (see above), and the values that are get
 *     confirmed on the panel.
 * So the AP passphrase is the perimeter in wizard mode, and TLS on top of WPA2
 * would buy a certificate warning and nothing else.
 */

#ifndef PROVISION_H
#define PROVISION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "net.h"  /* net_wifi_ap_t — the browser is offered the device's own scan */
#include "ui.h"   /* ui_event_cb_t — submissions are reported as UI events */

#ifdef __cplusplus
extern "C" {
#endif

/** @brief How long the portal stays up before closing itself, minutes. */
#define PROV_WINDOW_MIN  15U

/** @brief Which of the two portals is running. */
typedef enum {
    PROV_MODE_OFF,      /**< Not running.                                     */
    PROV_MODE_WIZARD,   /**< SoftAP + captive portal, for setup.              */
    PROV_MODE_ADMIN,    /**< HTTPS on the joined network, for administration. */
} prov_mode_t;

/**
 * @brief Where the wizard has got to.
 *
 * The order is the flow: create the admin code on the panel, show the QR code,
 * have the browser authorised from the panel, set the payout addresses, join the
 * venue network, done. Only the wizard walks these; admin mode sits on
 * @ref PROV_STEP_ADMIN and serves everything at once.
 */
typedef enum {
    PROV_STEP_IDLE,   /**< Nothing to do — the page says so.                */
    PROV_STEP_AUTH,   /**< Waiting for the admin code to be typed on the panel. */
    PROV_STEP_ADDR,   /**< Payout addresses.                                */
    PROV_STEP_WIFI,   /**< Join the venue's Wi-Fi.                          */
    PROV_STEP_DONE,   /**< Setup finished; thank the operator.              */
    PROV_STEP_ADMIN,  /**< Not a wizard step — the full admin page.         */
} prov_step_t;

/** @brief What the panel is being asked to accept, for @ref prov_pending. */
typedef enum {
    PROV_ASK_NONE,
    PROV_ASK_PAYOUT_ETH,     /**< Ethereum payout address.   */
    PROV_ASK_PAYOUT_TRON,    /**< Tron payout address.       */
    PROV_ASK_CONTRACT_ETH,   /**< ERC-20 token contract.     */
    PROV_ASK_CONTRACT_TRON,  /**< TRC-20 token contract.     */
} prov_ask_t;

/**
 * @brief Raise the portal.
 *
 * Idempotent for the same mode; a different mode is refused rather than silently
 * switched, since the two listen on different interfaces with different
 * transports. Stop it first.
 *
 * In wizard mode the AP passphrase is generated once and kept in NVS: the operator
 * may well have photographed the QR code, and a reboot mid-setup should not
 * invalidate it. It is erased by settings_factory_reset().
 *
 * @param[in] mode Which portal to run.
 * @param[in] cb   Where submissions are reported; the same callback the UI task
 *                 uses, so a form and a screen tap are indistinguishable to the
 *                 main task. Must outlive the call.
 * @return true if the portal is up and reachable. Admin mode returns false when no
 *         network is joined, since there would be no way to reach it.
 */
bool prov_start(prov_mode_t mode, ui_event_cb_t cb);

/** @brief Stop the portal, drop the AP, and withdraw anything unaccepted. */
void prov_stop(void);

/** @brief Which mode is running, or @ref PROV_MODE_OFF. */
prov_mode_t prov_mode(void);

/** @brief Tell the portal which wizard step is current. */
void prov_set_step(prov_step_t step);

/** @brief The current step. */
prov_step_t prov_step(void);

/** @brief Minutes left before the portal closes itself, 0 once it has. */
unsigned prov_window_left_min(void);

/** @brief AP SSID, or "" outside wizard mode. */
const char *prov_ap_ssid(void);

/** @brief AP passphrase, or "" outside wizard mode. */
const char *prov_ap_pass(void);

/**
 * @brief What the panel's QR code should carry.
 *
 * Wizard mode: "WIFI:T:WPA;S:<ssid>;P:<pass>;;" — a camera joins the AP from it
 * and the captive portal takes over, so one code is enough.
 * Admin mode: "https://<ip>/" — the page itself.
 */
const char *prov_qr_payload(void);

/**
 * @brief Whether a browser has asked to be authorised.
 *
 * Set by @c POST /api/auth. The panel answers by taking the admin code and calling
 * @ref prov_auth_resolve. Reported as @ref UI_EVENT_PROV_AUTH.
 */
bool prov_auth_pending(void);

/**
 * @brief Answer a pending authorisation request from the panel.
 *
 * @param[in] grant true if the operator entered the correct admin code.
 */
void prov_auth_resolve(bool grant);

/** @brief Whether the browser session is authorised to change anything. */
bool prov_authed(void);

/**
 * @brief Cut the wizard down to the Wi-Fi step, with no admin code.
 *
 * For a terminal that is already configured and has only lost its network. Call it
 * right after @ref prov_start (which clears it) and before any browser arrives; the
 * first browser to ask is then let in without the panel demanding the code, and the
 * panel drops the step numbering, since steps 1 and 3-4 do not happen.
 *
 * The relaxation is bounded: the perimeter here is the AP's per-device passphrase,
 * which is on the panel in front of whoever is asking, and everything that decides
 * where money goes still has to be accepted on that panel. What it buys is an
 * operator whose till has moved venues typing a password instead of walking three
 * screens to be allowed to.
 */
void prov_set_wifi_only(void);

/** @brief Whether the portal is the cut-down Wi-Fi-only flow. */
bool prov_wifi_only(void);

/**
 * @brief Put a one-line message on the page.
 *
 * For the things only the device can know — a Wi-Fi network that would not join, a
 * step that cannot be left yet. The browser is where the operator is looking, so
 * that is where the reason has to appear; the panel is showing a QR code.
 *
 * @param[in] note Message, copied. NULL or "" clears it.
 */
void prov_set_note(const char *note);

/**
 * @brief Hand the portal a Wi-Fi scan for the browser to choose from.
 *
 * The scan itself stays on the main task — scanning makes the radio hop channels,
 * which briefly drops anyone joined to the SoftAP, so it happens deliberately at
 * known moments (entering the Wi-Fi step, or a rescan the browser asked for) and
 * never inside an HTTP handler.
 *
 * @param[in] aps Scanned networks; copied.
 * @param[in] n   How many.
 */
void prov_set_scan(const net_wifi_ap_t *aps, uint16_t n);

/**
 * @brief Fetch the value a browser proposed but nobody has accepted.
 *
 * @param[out] kind    What is being asked, may be NULL.
 * @param[out] label   Human name for the panel ("Ethereum payout"), may be NULL.
 * @param[in]  label_n Capacity of @p label.
 * @param[out] value   The proposed address, may be NULL.
 * @param[in]  value_n Capacity of @p value.
 * @return true if a proposal is pending.
 */
bool prov_pending(prov_ask_t *kind, char *label, size_t label_n,
                  char *value, size_t value_n);

/**
 * @brief Resolve a pending proposal from the panel.
 *
 * @param[in] accept true to commit it to NVS, false to discard it.
 * @return true if a value was committed — the caller then has to restart to apply
 *         it, since the recipient and contract dual stores are built at boot.
 */
bool prov_pending_commit(bool accept);

/**
 * @brief Propose a value on behalf of the panel itself.
 *
 * Used by the card-derived route: the terminal reads an address off a Cryptnox
 * card, then puts it through the very same accept-on-the-panel handshake a browser
 * submission goes through, so there is one code path that stores an address and one
 * screen that approves one.
 *
 * @param[in] kind What the value is.
 * @param[in] addr The address; checked by the caller.
 * @return false if another proposal is already waiting.
 */
bool prov_propose(prov_ask_t kind, const char *addr);

#ifdef __cplusplus
}
#endif

#endif /* PROVISION_H */
