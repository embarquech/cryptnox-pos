/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file ui.cpp
 * @brief Touchscreen UI implementation: TFT_eSPI screens, XPT2046 touch
 *        handling and the FreeRTOS UI task.
 */

/******************************************************************
 * 1. Included files
 ******************************************************************/

#include "ui.h"

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <stdio.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "CW_Utils.h"   /* hardened memory primitives (CODING_RULES §1.4) */

static const char *TAG = "ui";

/******************************************************************
 * 2. CYD touch pins (separate SPI bus from TFT)
 ******************************************************************/
#define T_CS    33
#define T_IRQ   36
#define T_CLK   25
#define T_MOSI  32
#define T_MISO  39

static TFT_eSPI            tft;
static SPIClass            touchSPI(VSPI);
static XPT2046_Touchscreen touch(T_CS, T_IRQ);

static ui_event_cb_t s_cb        = NULL;
static ui_screen_t   s_screen    = UI_SCREEN_SPLASH;
static bool          s_redraw    = true;

/* Amount-entry state */
static uint64_t s_amount_units   = 1000000ULL;   /* 1.00 USDC */
static uint64_t s_last_drawn_amt = UINT64_MAX; /* force first draw */

/* Confirm screen data */
static uint64_t s_confirm_amount = 0ULL;
static char     s_confirm_addr[64] = "";

/* Tx status data */
static ui_tx_state_t s_tx_state  = UI_TX_STATE_PLACE_CARD;
static char          s_tx_info[64] = "";

/* Touch debouncing */
static uint32_t s_last_tap_ms = 0;

/******************************************************************
 * 3. Layout constants (320x240 landscape)
 ******************************************************************/
#define SCR_W      320
#define SCR_H      240

#define COL_BG     TFT_BLACK
#define COL_TITLE  TFT_YELLOW
#define COL_TEXT   TFT_WHITE
#define COL_DIM    TFT_LIGHTGREY
#define COL_OK     TFT_GREEN
#define COL_ERR    TFT_RED
#define COL_BTN    TFT_NAVY
#define COL_BTN_HI TFT_DARKGREEN
#define COL_BTN_NO TFT_MAROON

/******************************************************************
 * 4. Button geometry
 ******************************************************************/
struct Btn { int x, y, w, h; const char *label; uint16_t bg; };

/* Amount screen buttons */
static const Btn BTN_MINUS_U  = {  6,  60,  80, 50, "-1",     COL_BTN };
static const Btn BTN_PLUS_U   = {  6, 120,  80, 50, "+1",     COL_BTN };
static const Btn BTN_MINUS_C  = {234,  60,  80, 50, "-.01",   COL_BTN };
static const Btn BTN_PLUS_C   = {234, 120,  80, 50, "+.01",   COL_BTN };
static const Btn BTN_CONFIRM  = {100, 188, 120, 44, "CONFIRM",COL_BTN_HI };

/* Confirm screen buttons */
static const Btn BTN_CANCEL   = { 20, 188, 130, 44, "Cancel", COL_BTN_NO };
static const Btn BTN_SEND     = {170, 188, 130, 44, "Send",   COL_BTN_HI };

/* Tx status buttons */
static const Btn BTN_NEW      = { 80, 188, 160, 44, "New payment", COL_BTN_HI };
static const Btn BTN_CANCEL_W = { 80, 188, 160, 44, "Cancel",      COL_BTN_NO };

/******************************************************************
 * 5. Helpers
 ******************************************************************/
/* Draw "<number> USDC" with big digits (font 6, digits-only) and a smaller
 * "USDC" suffix in font 4 right after it. cx is the centre x coordinate. */
static void draw_amount_centered(uint64_t units, int cy_digits) {
    char num[32];
    uint64_t whole = units / 1000000ULL;
    uint64_t cents = (units % 1000000ULL) / 10000ULL;
    snprintf(num, sizeof(num), "%" PRIu64 ".%02" PRIu64, whole, cents);

    int num_w  = tft.textWidth(num, 6);
    int unit_w = tft.textWidth(" USDC", 4);
    int total  = num_w + unit_w;
    int start_x = (SCR_W - total) / 2;

    tft.setTextColor(COL_TITLE, COL_BG);
    tft.setTextDatum(ML_DATUM);
    tft.drawString(num, start_x, cy_digits, 6);
    tft.setTextDatum(ML_DATUM);
    tft.drawString(" USDC", start_x + num_w, cy_digits + 5, 4);
}

static void draw_btn(const Btn &b) {
    tft.fillRoundRect(b.x, b.y, b.w, b.h, 6, b.bg);
    tft.drawRoundRect(b.x, b.y, b.w, b.h, 6, TFT_WHITE);
    tft.setTextColor(TFT_WHITE, b.bg);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(b.label, b.x + b.w / 2, b.y + b.h / 2, 4);
}

static bool in_btn(const Btn &b, int16_t x, int16_t y) {
    return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

static void draw_title(const char *txt) {
    tft.fillRect(0, 0, SCR_W, 30, COL_BG);
    tft.setTextColor(COL_TITLE, COL_BG);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(txt, SCR_W / 2, 14, 4);
}

/******************************************************************
 * 6. Touch (calibrated raw range → screen coords; landscape rotation 1)
 ******************************************************************/
/* XPT2046_Touchscreen.setRotation(1) already swaps axes for landscape;
 * we just map raw 200..3800 → screen 0..320 / 0..240 like esp32-loot does. */
static bool poll_touch(int16_t *sx, int16_t *sy) {
    if (!touch.tirqTouched() || !touch.touched()) return false;
    TS_Point p = touch.getPoint();

    int16_t mx = map(p.x, 200, 3800, 0, SCR_W);
    int16_t my = map(p.y, 200, 3800, 0, SCR_H);
    if (mx < 0)            { mx = 0; }
    if (mx >= SCR_W)       { mx = SCR_W - 1; }
    if (my < 0)            { my = 0; }
    if (my >= SCR_H)       { my = SCR_H - 1; }

    *sx = mx;
    *sy = my;
    return true;
}

/* Require a "finger up" moment between taps to suppress the chained-button
 * cascade that happens when the user holds the screen or taps rapidly while
 * the screen transitions (Cancel/CONFIRM/Send all sit at the same y). */
static bool s_finger_lifted = true;

static bool tap(int16_t *x, int16_t *y) {
    bool pressed_now = touch.tirqTouched() && touch.touched();
    if (!pressed_now) {
        s_finger_lifted = true;
        return false;
    }
    if (!s_finger_lifted) {
        return false;  /* sustained press — already handled */
    }
    uint32_t now = millis();
    if (now - s_last_tap_ms < 150) {
        return false;
    }
    if (!poll_touch(x, y)) {
        return false;
    }
    s_last_tap_ms    = now;
    s_finger_lifted  = false;
    return true;
}

/******************************************************************
 * 7. Splash
 ******************************************************************/
static void draw_splash(void) {
    tft.fillScreen(COL_BG);
    tft.setTextColor(COL_TITLE, COL_BG);
    tft.setTextDatum(MC_DATUM);
    /* Font 7 / 8 are digits-only on TFT_eSPI, so use font 4 for letters. */
    tft.drawString("CRYPTNOX", SCR_W / 2, 90, 4);
    tft.setTextColor(COL_TEXT, COL_BG);
    tft.drawString("Loading...", SCR_W / 2, 140, 4);
}

/******************************************************************
 * 8. Amount entry
 ******************************************************************/
static void draw_amount_value(void) {
    tft.fillRect(0, 100, SCR_W, 50, COL_BG);
    draw_amount_centered(s_amount_units, 110);
    s_last_drawn_amt = s_amount_units;
}

static void draw_amount_screen(void) {
    tft.fillScreen(COL_BG);
    draw_title("Amount");
    draw_amount_value();
    draw_btn(BTN_MINUS_U);
    draw_btn(BTN_PLUS_U);
    draw_btn(BTN_MINUS_C);
    draw_btn(BTN_PLUS_C);
    draw_btn(BTN_CONFIRM);
}

/**
 * @brief Handle a tap on the amount-entry screen.
 *
 * Adjusts the amount by ±1 / ±0.01 USDC, clamped to [0, 99999] USDC on
 * both increments (F-15), or emits UI_EVENT_AMOUNT_CONFIRMED.
 *
 * @param[in] x Screen x coordinate of the tap.
 * @param[in] y Screen y coordinate of the tap.
 */
static void handle_amount_tap(int16_t x, int16_t y) {
    if (in_btn(BTN_MINUS_U, x, y)) {
        if (s_amount_units >= 1000000ULL) s_amount_units -= 1000000ULL;
    } else if (in_btn(BTN_PLUS_U, x, y)) {
        s_amount_units += 1000000ULL;
        if (s_amount_units > 99999000000ULL) s_amount_units = 99999000000ULL;
    } else if (in_btn(BTN_MINUS_C, x, y)) {
        if (s_amount_units >= 10000ULL) s_amount_units -= 10000ULL;
    } else if (in_btn(BTN_PLUS_C, x, y)) {
        s_amount_units += 10000ULL;
        if (s_amount_units > 99999000000ULL) s_amount_units = 99999000000ULL;
    } else if (in_btn(BTN_CONFIRM, x, y)) {
        if (s_cb != NULL && s_amount_units > 0ULL) {
            s_cb(UI_EVENT_AMOUNT_CONFIRMED, s_amount_units);
        }
    }
}

/******************************************************************
 * 9. Confirm
 ******************************************************************/
static void draw_confirm_screen(void) {
    tft.fillScreen(COL_BG);
    draw_title("Confirm");

    draw_amount_centered(s_confirm_amount, 55);

    tft.setTextColor(COL_DIM, COL_BG);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("To:", SCR_W / 2, 110, 2);

    /* Truncate address to display max ~28 chars per line. */
    tft.setTextColor(COL_TEXT, COL_BG);
    tft.setTextDatum(MC_DATUM);
    /* Split long hex address across 2 lines for readability. */
    char line1[24] = {0}, line2[24] = {0};
    size_t addr_len = strlen(s_confirm_addr);
    size_t half = addr_len > 22 ? 22 : addr_len;
    (void)CW_Utils::safe_memcpy(reinterpret_cast<uint8_t *>(line1), sizeof(line1),
                                reinterpret_cast<const uint8_t *>(s_confirm_addr), half);
    if (addr_len > half) {
        size_t rest = addr_len - half;
        if (rest > 22) rest = 22;
        (void)CW_Utils::safe_memcpy(reinterpret_cast<uint8_t *>(line2), sizeof(line2),
                                    reinterpret_cast<const uint8_t *>(s_confirm_addr + half), rest);
    }
    tft.drawString(line1, SCR_W / 2, 130, 2);
    tft.drawString(line2, SCR_W / 2, 150, 2);

    draw_btn(BTN_CANCEL);
    draw_btn(BTN_SEND);
}

static void handle_confirm_tap(int16_t x, int16_t y) {
    if (in_btn(BTN_CANCEL, x, y)) {
        if (s_cb) s_cb(UI_EVENT_CONFIRM_CANCEL, 0);
    } else if (in_btn(BTN_SEND, x, y)) {
        /* Instant feedback: main is about to block on the RPC nonce call for
         * a couple seconds before reaching ui_show_tx_status, so we jump to
         * the Transaction screen ourselves. Main will overwrite the info
         * text once it reaches the card-wait step. */
        s_tx_state = UI_TX_STATE_PLACE_CARD;
        strncpy(s_tx_info, "Preparing...", sizeof(s_tx_info) - 1);
        s_tx_info[sizeof(s_tx_info) - 1] = '\0';
        s_screen = UI_SCREEN_TX_STATUS;
        s_redraw = true;
        if (s_cb) s_cb(UI_EVENT_CONFIRM_OK, 0);
    }
}

/******************************************************************
 * 10. Tx status
 ******************************************************************/
static void draw_tx_status_screen(void) {
    tft.fillScreen(COL_BG);
    draw_title("Transaction");

    uint16_t color = COL_TITLE;
    const char *state_str = "";
    bool show_retry = false;

    switch (s_tx_state) {
        case UI_TX_STATE_PLACE_CARD: state_str = "Place card"; break;
        case UI_TX_STATE_SIGNING:    state_str = "Signing..."; break;
        case UI_TX_STATE_SENDING:    state_str = "Broadcasting..."; break;
        case UI_TX_STATE_DONE:       state_str = "Sent";    color = COL_OK;  show_retry = true; break;
        case UI_TX_STATE_FAILED:     state_str = "Failed";  color = COL_ERR; show_retry = true; break;
    }

    tft.setTextColor(color, COL_BG);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(state_str, SCR_W / 2, 80, 4);

    /* Info line — break long strings across multiple lines */
    tft.setTextColor(COL_DIM, COL_BG);
    size_t len = strlen(s_tx_info);
    if (len <= 28) {
        tft.drawString(s_tx_info, SCR_W / 2, 140, 2);
    } else {
        /* len > 28 here -- split into two 28-char lines and clamp the tail
         * (s_tx_info is 64 bytes so the tail can reach 35). */
        char line1[32] = {0}, line2[32] = {0};
        (void)CW_Utils::safe_memcpy(reinterpret_cast<uint8_t *>(line1), sizeof(line1),
                                    reinterpret_cast<const uint8_t *>(s_tx_info), 28U);
        size_t rest = (len > 56U) ? 28U : (len - 28U);
        (void)CW_Utils::safe_memcpy(reinterpret_cast<uint8_t *>(line2), sizeof(line2),
                                    reinterpret_cast<const uint8_t *>(s_tx_info + 28), rest);
        tft.drawString(line1, SCR_W / 2, 135, 2);
        tft.drawString(line2, SCR_W / 2, 155, 2);
    }

    if (show_retry) {
        draw_btn(BTN_NEW);
    } else if (s_tx_state == UI_TX_STATE_PLACE_CARD) {
        draw_btn(BTN_CANCEL_W);
    }
}

static void handle_tx_status_tap(int16_t x, int16_t y) {
    bool can_retry = (s_tx_state == UI_TX_STATE_DONE) ||
                     (s_tx_state == UI_TX_STATE_FAILED);
    if (can_retry && in_btn(BTN_NEW, x, y)) {
        if (s_cb) s_cb(UI_EVENT_TX_RETRY, 0);
    } else if ((s_tx_state == UI_TX_STATE_PLACE_CARD) && in_btn(BTN_CANCEL_W, x, y)) {
        /* Instant feedback: switch the screen ourselves; the main task is
         * blocked in wallet.connect and will see the cancel event only when
         * it eventually times out. */
        if (s_cb) s_cb(UI_EVENT_CONFIRM_CANCEL, 0);
        s_screen = UI_SCREEN_AMOUNT;
        s_redraw = true;
        s_last_drawn_amt = UINT64_MAX;
    }
}

/******************************************************************
 * 11. Main UI task — drives touch + redraws
 ******************************************************************/
static void ui_task(void *arg) {
    (void)arg;
    while (true) {
        if (s_redraw) {
            switch (s_screen) {
                case UI_SCREEN_SPLASH:    draw_splash();           break;
                case UI_SCREEN_AMOUNT:    draw_amount_screen();    break;
                case UI_SCREEN_CONFIRM:   draw_confirm_screen();   break;
                case UI_SCREEN_TX_STATUS: draw_tx_status_screen(); break;
            }
            s_redraw = false;
        }

        /* Incremental refresh of amount-entry value as user taps +/- */
        if (s_screen == UI_SCREEN_AMOUNT && s_amount_units != s_last_drawn_amt) {
            draw_amount_value();
        }

        int16_t x, y;
        if (tap(&x, &y)) {
            switch (s_screen) {
                case UI_SCREEN_AMOUNT:    handle_amount_tap(x, y);    break;
                case UI_SCREEN_CONFIRM:   handle_confirm_tap(x, y);   break;
                case UI_SCREEN_TX_STATUS: handle_tx_status_tap(x, y); break;
                default: break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/******************************************************************
 * 12. Public API
 ******************************************************************/
extern "C" void ui_init(ui_event_cb_t cb) {
    s_cb = cb;

    tft.init();
    tft.setRotation(1);
    tft.fillScreen(COL_BG);

    touchSPI.begin(T_CLK, T_MISO, T_MOSI, T_CS);
    touch.begin(touchSPI);
    touch.setRotation(1);

    s_screen = UI_SCREEN_SPLASH;
    s_redraw = true;

    xTaskCreate(ui_task, "ui", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "UI initialized (TFT_eSPI + XPT2046)");
}

extern "C" void ui_show_splash(void) {
    s_screen = UI_SCREEN_SPLASH;
    s_redraw = true;
}

extern "C" void ui_show_amount_entry(void) {
    s_screen = UI_SCREEN_AMOUNT;
    s_redraw = true;
    s_last_drawn_amt = UINT64_MAX;  /* force amount value redraw */
}

extern "C" void ui_show_confirm(uint64_t amount_units, const char *dest_addr) {
    s_confirm_amount = amount_units;
    if (dest_addr) {
        strncpy(s_confirm_addr, dest_addr, sizeof(s_confirm_addr) - 1);
        s_confirm_addr[sizeof(s_confirm_addr) - 1] = '\0';
    } else {
        s_confirm_addr[0] = '\0';
    }
    s_screen = UI_SCREEN_CONFIRM;
    s_redraw = true;
}

extern "C" void ui_show_tx_status(ui_tx_state_t state, const char *info) {
    s_tx_state = state;
    if (info) {
        strncpy(s_tx_info, info, sizeof(s_tx_info) - 1);
        s_tx_info[sizeof(s_tx_info) - 1] = '\0';
    } else {
        s_tx_info[0] = '\0';
    }
    s_screen = UI_SCREEN_TX_STATUS;
    s_redraw = true;
}
