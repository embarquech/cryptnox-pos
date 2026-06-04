/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file ui.h
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
    UI_SCREEN_TX_STATUS,   /**< Card wait / signing / broadcast progress.  */
} ui_screen_t;

/** @brief Events emitted by the UI task towards the main task. */
typedef enum {
    UI_EVENT_AMOUNT_CONFIRMED,  /**< CONFIRM tapped; payload = amount.     */
    UI_EVENT_CONFIRM_OK,        /**< Send tapped on the Confirm screen.    */
    UI_EVENT_CONFIRM_CANCEL,    /**< Cancel tapped (Confirm or card wait). */
    UI_EVENT_TX_RETRY,          /**< New payment tapped after Done/Failed. */
} ui_event_t;

/** @brief States shown on the transaction-status screen. */
typedef enum {
    UI_TX_STATE_PLACE_CARD,  /**< Waiting for the card on the reader. */
    UI_TX_STATE_SIGNING,     /**< Card found, signing in progress.    */
    UI_TX_STATE_SENDING,     /**< Broadcasting the signed tx.         */
    UI_TX_STATE_DONE,        /**< Broadcast succeeded.                */
    UI_TX_STATE_FAILED,      /**< Any failure; info line says why.    */
} ui_tx_state_t;

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

#ifdef __cplusplus
}
#endif

#endif // UI_H
