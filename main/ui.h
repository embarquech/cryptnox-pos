#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_SCREEN_SPLASH,
    UI_SCREEN_AMOUNT,
    UI_SCREEN_CONFIRM,
    UI_SCREEN_TX_STATUS,
} ui_screen_t;

typedef enum {
    UI_EVENT_AMOUNT_CONFIRMED,
    UI_EVENT_CONFIRM_OK,
    UI_EVENT_CONFIRM_CANCEL,
    UI_EVENT_TX_RETRY,
} ui_event_t;

typedef enum {
    UI_TX_STATE_PLACE_CARD,
    UI_TX_STATE_SIGNING,
    UI_TX_STATE_SENDING,
    UI_TX_STATE_DONE,
    UI_TX_STATE_FAILED,
} ui_tx_state_t;

typedef void (*ui_event_cb_t)(ui_event_t event, uint64_t payload);

void ui_init(ui_event_cb_t cb);
void ui_show_splash(void);
void ui_show_amount_entry(void);
void ui_show_confirm(uint64_t amount_units, const char *dest_addr);
void ui_show_tx_status(ui_tx_state_t state, const char *info);

#ifdef __cplusplus
}
#endif
