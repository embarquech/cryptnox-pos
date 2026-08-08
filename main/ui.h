/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file ui.h
 * @ingroup ui
 * @brief Touchscreen UI API: screens, events and transaction states for the
 *        CYD (ILI9341 + XPT2046) payment flow.
 */

#ifndef UI_H
#define UI_H

/******************************************************************
 * 1. Included files
 ******************************************************************/

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "net.h"   /* net_wifi_ap_t for the network picker */

#ifdef __cplusplus
extern "C" {
#endif

/******************************************************************
 * 2. Types
 ******************************************************************/

/** @brief Top-level screens of the payment flow. */
typedef enum {
    UI_SCREEN_SPLASH,      /**< Boot splash while WiFi/RPC/wallet come up. */
    UI_SCREEN_AMOUNT,      /**< Amount entry with +/- buttons.             */
    UI_SCREEN_CONFIRM,     /**< Amount + destination review.               */
    UI_SCREEN_PIN,         /**< Numeric keypad to enter the card PIN.      */
    UI_SCREEN_WIFI_LIST,   /**< Scanned Wi-Fi networks to pick from.       */
    UI_SCREEN_WIFI_PASS,   /**< Keyboard to enter the Wi-Fi password.      */
    UI_SCREEN_WIFI_CONNECTING, /**< "Connecting…" while main associates.   */
    UI_SCREEN_SETTINGS,    /**< Full-screen settings (tabs).               */
    UI_SCREEN_TX_STATUS,   /**< Card wait / signing / broadcast progress.  */
    UI_SCREEN_BOOT_ERROR,  /**< Startup fault — names the cause, not "Declined". */
    UI_SCREEN_ADMIN_SET,   /**< First-run creation of the admin code.      */
    UI_SCREEN_ADMIN_UNLOCK,/**< Admin code demanded before the settings.   */
    UI_SCREEN_WELCOME,     /**< Greeting opening first-run setup.          */
    UI_SCREEN_RECIP_EDIT,  /**< Keyboard to provision a recipient address. */
} ui_screen_t;

/** @brief Events emitted by the UI task towards the main task. */
typedef enum {
    UI_EVENT_AMOUNT_CONFIRMED,  /**< CONFIRM tapped; payload = amount.     */
    UI_EVENT_CONFIRM_OK,        /**< Send tapped on the Confirm screen.    */
    UI_EVENT_CONFIRM_CANCEL,    /**< Cancel tapped (Confirm or card wait). */
    UI_EVENT_PIN_ENTERED,       /**< PIN keypad validated; fetch via ui_take_pin. */
    UI_EVENT_WIFI_SCAN,         /**< User opened the Wi-Fi picker; main should scan. */
    UI_EVENT_WIFI_TRY,          /**< Wi-Fi creds entered; fetch via ui_take_wifi_creds. */
    UI_EVENT_TX_RETRY,          /**< New payment tapped after Done/Failed. */
    UI_EVENT_ADMIN_SET,         /**< Admin code created and stored (first run). */
    UI_EVENT_WELCOME_DONE,      /**< Start tapped on the welcome screen.   */
} ui_event_t;

/** @brief States shown on the transaction-status screen. */
typedef enum {
    UI_TX_STATE_PLACE_CARD,  /**< Waiting for the card on the reader. */
    UI_TX_STATE_PROCESSING,  /**< Card tapped — opening the secure channel. */
    UI_TX_STATE_SIGNING,     /**< Card found, signing in progress.    */
    UI_TX_STATE_SENDING,     /**< Broadcasting the signed tx.         */
    UI_TX_STATE_CONFIRMING,  /**< Broadcast — waiting for the mined receipt. */
    UI_TX_STATE_DONE,        /**< Receipt mined with status 0x1.      */
    UI_TX_STATE_FAILED,      /**< Any failure; info line says why.    */
} ui_tx_state_t;

/**
 * @brief Startup faults shown on @ref UI_SCREEN_BOOT_ERROR.
 *
 * Bring-up problems, not declined payments — hence their own screen. Wording
 * lives in ui.cpp; the caller only names the fault.
 */
typedef enum {
    UI_BOOT_ERR_NFC,      /**< PN532 did not answer — wiring/power/I2C.       */
    UI_BOOT_ERR_WALLET,   /**< Reader answered, wallet layer failed to start. */
} ui_boot_err_t;

/**
 * @brief Callback invoked from the UI task on user interaction.
 *
 * Runs in the UI task context — keep it short and non-blocking.
 *
 * @param[in] event   Event identifier.
 * @param[in] payload Amount in USDC base units for
 *                    @ref UI_EVENT_AMOUNT_CONFIRMED, 0 otherwise.
 */
typedef void (*ui_event_cb_t)(ui_event_t event, uint64_t payload);

/******************************************************************
 * 3. Public API
 ******************************************************************/

/**
 * @brief Initialise display + touch and start the UI task.
 *
 * @param[in] cb Event callback; must remain valid for the program lifetime.
 */
void ui_init(ui_event_cb_t cb);

/** @brief Switch to the splash screen. */
void ui_show_splash(void);

/** @brief Switch to the amount-entry screen (forces a value redraw). */
void ui_show_amount_entry(void);

/**
 * @brief Switch to the confirm screen.
 *
 * @param[in] amount_units Amount in USDC base units (6 decimals).
 * @param[in] dest_addr    "0x..."-prefixed destination address; copied
 *                         internally, may be NULL for none.
 */
void ui_show_confirm(uint64_t amount_units, const char *dest_addr);

/**
 * @brief Switch to the transaction-status screen.
 *
 * @param[in] state Transaction state to display.
 * @param[in] info  Optional info line (tx hash, error message); copied
 *                  internally, may be NULL for none.
 */
void ui_show_tx_status(ui_tx_state_t state, const char *info);

/**
 * @brief Copy the most recently entered PIN out and wipe the UI's copy.
 *
 * Call once after @ref UI_EVENT_PIN_ENTERED. The internal buffer is
 * secure-wiped on read, so a second call returns 0.
 *
 * @param[out] out  Destination buffer (NUL-terminated on return).
 * @param[in]  n    Capacity of @p out.
 * @return number of PIN digits copied (0 if none available).
 */
size_t ui_take_pin(char *out, size_t n);

/**
 * @brief Show the scanned Wi-Fi networks for the user to pick from.
 *
 * @param[in] aps  Array of scanned APs (copied internally).
 * @param[in] n    Number of entries in @p aps.
 * @param[in] note Optional one-line reason the picker (re)opened, shown above
 *                 the list; copied internally, NULL for none. Applied on every
 *                 call, so a stale note cannot survive a later render.
 */
void ui_show_wifi_list(const net_wifi_ap_t *aps, uint16_t n, const char *note);

/**
 * @brief Show a startup fault, naming the cause and what to do about it.
 *
 * @param[in] kind   Which bring-up step failed.
 * @param[in] detail Optional technical detail for a technician (an esp_err_t
 *                   name, say); copied internally, may be NULL.
 */
void ui_show_boot_error(ui_boot_err_t kind, const char *detail);

/**
 * @brief Show a "Connecting to <ssid>…" screen while main associates.
 *
 * Interactive picker only; unattended boot reports through
 * @ref ui_set_boot_status and stays on the splash.
 */
void ui_show_wifi_connecting(const char *ssid);

/**
 * @brief Set the one-line progress note on the splash screen.
 *
 * Updates the splash in place instead of switching screens. Safe from the main
 * task: applied by the UI task on its next pass.
 *
 * @param[in] step Short label ("Starting NFC reader"), copied internally.
 *                 NULL or "" clears the line.
 */
void ui_set_boot_status(const char *step);

/**
 * @brief Greet the operator at the start of first-run setup.
 *
 * Shown on a virgin or factory-reset terminal, before the Wi-Fi and admin-code
 * steps. Emits @ref UI_EVENT_WELCOME_DONE when Start is tapped.
 */
void ui_show_welcome(void);

/**
 * @brief Run the first-run admin-code creation (enter, then confirm).
 *
 * Deliberately has no way out: everything behind the burger menu — Wi-Fi, fees,
 * factory reset — sits behind this code, so the terminal must not become usable
 * without one. Emits @ref UI_EVENT_ADMIN_SET once stored.
 */
void ui_show_admin_set(void);

/**
 * @brief Fetch the selected SSID + entered password and wipe the UI's copy.
 *
 * Call once after @ref UI_EVENT_WIFI_TRY.
 *
 * @param[out] ssid    SSID buffer (>= 33 bytes).
 * @param[in]  ssid_n  Capacity of @p ssid.
 * @param[out] pass    Password buffer (>= 65 bytes).
 * @param[in]  pass_n  Capacity of @p pass.
 * @return length of the SSID (0 if none available).
 */
size_t ui_take_wifi_creds(char *ssid, size_t ssid_n, char *pass, size_t pass_n);

#ifdef __cplusplus
}
#endif

#endif // UI_H
