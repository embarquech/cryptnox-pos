/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 */

/**
 * @file ui.cpp
 * @brief Touchscreen UI implementation built on LVGL 8.x.
 *
 * TFT_eSPI is used only as the low-level panel driver (the LVGL flush
 * callback pushes rendered pixels to it); XPT2046_Touchscreen feeds the LVGL
 * pointer input device. All LVGL calls happen on the single UI task — the
 * public ui_show_* entry points (called from the main task) only stage a
 * screen request + its data and raise a dirty flag, which the UI task drains.
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
#include <string.h>
#include <inttypes.h>

#include "lvgl.h"
#include "logo_img.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/ledc.h"

#include "CW_Utils.h"   /* hardened memory primitives (CODING_RULES §1.4) */

static const char *TAG = "ui";

/******************************************************************
 * 2. Hardware — panel (TFT_eSPI) and touch (XPT2046, separate SPI bus)
 ******************************************************************/
#define T_CS    33
#define T_IRQ   36
#define T_CLK   25
#define T_MOSI  32
#define T_MISO  39

#define SCR_W   320
#define SCR_H   240

static TFT_eSPI            tft;
static SPIClass            touchSPI(VSPI);
static XPT2046_Touchscreen touch(T_CS, T_IRQ);

/******************************************************************
 * 3. Theme — Cryptnox brand palette (cryptnox.com global colors)
 ******************************************************************/
#define COL_BG       lv_color_hex(0x101F2E)   /* deep navy — page background  */
#define COL_SURFACE  lv_color_hex(0x15223D)   /* primary navy — cards/steppers */
#define COL_TEXT     lv_color_hex(0xFFFFFF)   /* --e-global-color-text         */
#define COL_DIM      lv_color_hex(0x7C8BA5)   /* muted blue-grey — labels      */
#define COL_ACCENT   lv_color_hex(0x48ACF0)   /* Cryptnox blue — primary action */
#define COL_SUCCESS  lv_color_hex(0x34E3E8)   /* brand cyan — "Sent"           */
#define COL_DANGER   lv_color_hex(0xF6405F)   /* brand red — failures          */
#define COL_BORDER   lv_color_hex(0x243349)

/******************************************************************
 * 4. LVGL display + input plumbing
 ******************************************************************/
#define LV_TICK_PERIOD_MS  2
/* Partial draw buffer — 40 lines (no PSRAM on the CYD, keep it small). */
static lv_color_t        s_buf[SCR_W * 40];
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t      s_disp_drv;
static lv_indev_drv_t     s_indev_drv;

static void disp_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    /* swap=true converts LVGL's native 16-bit order to the panel's. If colours
     * come out byte-swapped/garbled, flip this bool or set CONFIG_LV_COLOR_16_SWAP. */
    tft.pushColors(reinterpret_cast<uint16_t *>(&px->full), w * h, true);
    tft.endWrite();

    lv_disp_flush_ready(drv);
}

static bool touch_to_screen(int16_t *sx, int16_t *sy) {
    if (!touch.tirqTouched() || !touch.touched()) {
        return false;
    }
    TS_Point p = touch.getPoint();
    int16_t mx = map(p.x, 200, 3800, 0, SCR_W);
    int16_t my = map(p.y, 200, 3800, 0, SCR_H);
    if (mx < 0)      { mx = 0; }
    if (mx >= SCR_W) { mx = SCR_W - 1; }
    if (my < 0)      { my = 0; }
    if (my >= SCR_H) { my = SCR_H - 1; }
    *sx = mx;
    *sy = my;
    return true;
}

static void indev_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    (void)drv;
    int16_t x, y;
    if (touch_to_screen(&x, &y)) {
        data->state   = LV_INDEV_STATE_PR;
        data->point.x = x;
        data->point.y = y;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

static void tick_cb(void *arg) {
    (void)arg;
    lv_tick_inc(LV_TICK_PERIOD_MS);
}

/******************************************************************
 * 4b. Backlight — LEDC PWM dimming on the CYD's GPIO 21
 ******************************************************************/
#define BL_GPIO        21
#define BL_LEDC_MODE   LEDC_LOW_SPEED_MODE
#define BL_LEDC_TIMER  LEDC_TIMER_0
#define BL_LEDC_CH     LEDC_CHANNEL_0
#define BL_LEDC_RES    LEDC_TIMER_10_BIT   /* duty 0..1023            */
#define BL_PWM_HZ      5000                /* above the audible range */

static void backlight_set_pct(uint8_t pct) {
    if (pct > 100U) { pct = 100U; }
    uint32_t duty = (1023U * pct) / 100U;
    (void)ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CH, duty);
    (void)ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CH);
}

static void backlight_init(uint8_t pct) {
    ledc_timer_config_t t = {};
    t.speed_mode      = BL_LEDC_MODE;
    t.duty_resolution = BL_LEDC_RES;
    t.timer_num       = BL_LEDC_TIMER;
    t.freq_hz         = BL_PWM_HZ;
    t.clk_cfg         = LEDC_AUTO_CLK;
    (void)ledc_timer_config(&t);

    ledc_channel_config_t c = {};
    c.gpio_num   = BL_GPIO;
    c.speed_mode = BL_LEDC_MODE;
    c.channel    = BL_LEDC_CH;
    c.timer_sel  = BL_LEDC_TIMER;
    c.duty       = 0;
    c.hpoint     = 0;
    (void)ledc_channel_config(&c);

    backlight_set_pct(pct);
}

/******************************************************************
 * 5. Shared state (written by ui_show_* on the main task, read by ui_task)
 ******************************************************************/
static ui_event_cb_t s_cb = NULL;

static volatile bool        s_screen_dirty = true;
static volatile ui_screen_t s_req_screen   = UI_SCREEN_SPLASH;

/* Amount entry — owned by the UI task once running. 1.00 USDC default. */
static uint64_t s_amount_units = 1000000ULL;

/* Confirm-screen payload */
static uint64_t s_confirm_amount = 0ULL;
static char     s_confirm_addr[64] = "";

/* Tx-status payload */
static ui_tx_state_t s_tx_state    = UI_TX_STATE_PLACE_CARD;
static char          s_tx_info[64] = "";

/* Live handle to the amount label so +/- can update it in place. */
static lv_obj_t *s_amount_label = NULL;

/* Backlight level (%), applied at boot and adjustable from the settings menu. */
static uint8_t s_brightness = 80;

/******************************************************************
 * 6. Button actions
 ******************************************************************/
enum BtnAction {
    ACT_MINUS_U, ACT_PLUS_U, ACT_MINUS_C, ACT_PLUS_C,
    ACT_CONFIRM, ACT_CANCEL, ACT_SEND, ACT_NEW,
    ACT_SETTINGS, ACT_CLOSE,
};

/* Settings modal — defined in section 7 (uses the widget helpers). */
static void open_settings(void);
static void close_settings(void);

static void request_screen(ui_screen_t s) {
    s_req_screen   = s;
    s_screen_dirty = true;
}

static void format_amount(uint64_t units, char *out, size_t n) {
    uint64_t whole = units / 1000000ULL;
    uint64_t cents = (units % 1000000ULL) / 10000ULL;
    snprintf(out, n, "%" PRIu64 ".%02" PRIu64, whole, cents);
}

static void update_amount_label(void) {
    if (s_amount_label != NULL) {
        char buf[32];
        format_amount(s_amount_units, buf, sizeof(buf));
        lv_label_set_text(s_amount_label, buf);
    }
}

/* Runs on the UI task (inside lv_timer_handler), so touching shared state and
 * invoking s_cb (which only posts to a queue) is safe here. */
static void btn_event_cb(lv_event_t *e) {
    BtnAction act = static_cast<BtnAction>(
        reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));

    switch (act) {
        case ACT_MINUS_U:
            if (s_amount_units >= 1000000ULL) { s_amount_units -= 1000000ULL; }
            update_amount_label();
            break;
        case ACT_PLUS_U:
            s_amount_units += 1000000ULL;
            if (s_amount_units > 99999000000ULL) { s_amount_units = 99999000000ULL; }
            update_amount_label();
            break;
        case ACT_MINUS_C:
            if (s_amount_units >= 10000ULL) { s_amount_units -= 10000ULL; }
            update_amount_label();
            break;
        case ACT_PLUS_C:
            s_amount_units += 10000ULL;
            if (s_amount_units > 99999000000ULL) { s_amount_units = 99999000000ULL; }
            update_amount_label();
            break;
        case ACT_CONFIRM:
            if (s_cb != NULL && s_amount_units > 0ULL) {
                s_cb(UI_EVENT_AMOUNT_CONFIRMED, s_amount_units);
            }
            break;
        case ACT_CANCEL:
            if (s_cb != NULL) { s_cb(UI_EVENT_CONFIRM_CANCEL, 0); }
            break;
        case ACT_SEND:
            /* Instant feedback: main blocks on the RPC nonce call for a moment
             * before reaching ui_show_tx_status, so jump to the tx screen now. */
            s_tx_state = UI_TX_STATE_PLACE_CARD;
            strncpy(s_tx_info, "Preparing...", sizeof(s_tx_info) - 1);
            s_tx_info[sizeof(s_tx_info) - 1] = '\0';
            request_screen(UI_SCREEN_TX_STATUS);
            if (s_cb != NULL) { s_cb(UI_EVENT_CONFIRM_OK, 0); }
            break;
        case ACT_NEW:
            if (s_cb != NULL) { s_cb(UI_EVENT_TX_RETRY, 0); }
            break;
        case ACT_SETTINGS:
            open_settings();
            break;
        case ACT_CLOSE:
            close_settings();
            break;
    }
}

/******************************************************************
 * 7. Widget helpers
 ******************************************************************/
static lv_obj_t *make_button(lv_obj_t *parent, const char *label, lv_color_t bg,
                             lv_color_t fg, lv_coord_t w, lv_coord_t h,
                             lv_align_t align, lv_coord_t x, lv_coord_t y,
                             BtnAction act, const lv_font_t *font) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_align(btn, align, x, y);

    /* Base look — rounded, flat fill, no default border. */
    lv_obj_set_style_bg_color(btn, bg, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 10, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);

    /* Soft drop shadow for depth (floats the button above the background). */
    lv_obj_set_style_shadow_width(btn, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(btn, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_shadow_ofs_y(btn, 4, LV_PART_MAIN);

    /* Secondary (surface) buttons get a hairline border to lift them off the bg.
     * (Compare the raw RGB565 value — lv_color_eq isn't in this LVGL build.) */
    if (bg.full == COL_SURFACE.full) {
        lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(btn, COL_BORDER, LV_PART_MAIN);
    }

    /* Pressed feedback: darken the fill and sink it into its shadow. */
    lv_obj_set_style_bg_color(btn, lv_color_mix(lv_color_black(), bg, 70),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(btn, 2, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_ofs_y(btn, 1, LV_PART_MAIN | LV_STATE_PRESSED);

    lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(act)));

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, fg, LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, font, LV_PART_MAIN);
    lv_obj_center(lbl);
    return btn;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *txt, lv_color_t color,
                            const lv_font_t *font, lv_align_t align,
                            lv_coord_t x, lv_coord_t y) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_color(lbl, color, LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, font, LV_PART_MAIN);
    lv_obj_align(lbl, align, x, y);
    return lbl;
}

/* Thin horizontal rule under a screen title, for visual structure. */
static void make_divider(lv_obj_t *parent, lv_coord_t y) {
    lv_obj_t *d = lv_obj_create(parent);
    lv_obj_set_size(d, SCR_W - 48, 2);
    lv_obj_align(d, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_color(d, COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(d, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(d, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(d, 0, LV_PART_MAIN);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);
}

static void clear_screen(void) {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, COL_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    s_amount_label = NULL;
}

/******************************************************************
 * 7b. Settings modal (burger menu → brightness slider)
 ******************************************************************/
static lv_obj_t *s_settings = NULL;   /* modal root on the top layer, or NULL */

static void brightness_event_cb(lv_event_t *e) {
    lv_obj_t *sl  = lv_event_get_target(e);
    lv_obj_t *lbl = static_cast<lv_obj_t *>(lv_event_get_user_data(e));
    s_brightness  = static_cast<uint8_t>(lv_slider_get_value(sl));
    backlight_set_pct(s_brightness);
    if (lbl != NULL) {
        char b[8];
        snprintf(b, sizeof(b), "%u%%", static_cast<unsigned>(s_brightness));
        lv_label_set_text(lbl, b);
    }
}

static void backdrop_event_cb(lv_event_t *e) {
    (void)e;
    close_settings();   /* tap outside the card dismisses */
}

static void close_settings(void) {
    if (s_settings != NULL) {
        lv_obj_del(s_settings);
        s_settings = NULL;
    }
}

static void open_settings(void) {
    if (s_settings != NULL) { return; }

    /* Built on the top layer so it floats above any screen and survives the
     * per-screen lv_obj_clean() in clear_screen(). */
    s_settings = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_settings);
    lv_obj_set_size(s_settings, SCR_W, SCR_H);
    lv_obj_set_style_bg_color(s_settings, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_settings, LV_OPA_50, LV_PART_MAIN);
    lv_obj_clear_flag(s_settings, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_settings, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_settings, backdrop_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = lv_obj_create(s_settings);
    lv_obj_set_size(card, 250, 150);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, COL_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 14, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, COL_BORDER, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    /* Absorb taps on the card so they don't reach the backdrop and dismiss it. */
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    make_label(card, "Brightness", COL_TEXT, &lv_font_montserrat_20,
               LV_ALIGN_TOP_LEFT, 4, 6);
    lv_obj_t *pct = make_label(card, "", COL_DIM, &lv_font_montserrat_14,
                               LV_ALIGN_TOP_RIGHT, -4, 10);

    lv_obj_t *sl = lv_slider_create(card);
    lv_obj_set_width(sl, 200);
    lv_obj_align(sl, LV_ALIGN_CENTER, 0, 0);
    lv_slider_set_range(sl, 10, 100);   /* never fully dark */
    lv_slider_set_value(sl, s_brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, COL_ACCENT, LV_PART_KNOB);
    lv_obj_add_event_cb(sl, brightness_event_cb, LV_EVENT_VALUE_CHANGED, pct);

    char b[8];
    snprintf(b, sizeof(b), "%u%%", static_cast<unsigned>(s_brightness));
    lv_label_set_text(pct, b);

    make_button(card, "Close", COL_ACCENT, COL_BG, 120, 40,
                LV_ALIGN_BOTTOM_MID, 0, -8, ACT_CLOSE, &lv_font_montserrat_20);
}

/******************************************************************
 * 8. Screen builders
 ******************************************************************/
/* Small burger button (top-left) that opens the settings modal. */
static void add_menu_button(void) {
    make_button(lv_scr_act(), LV_SYMBOL_LIST, COL_SURFACE, COL_TEXT, 42, 30,
                LV_ALIGN_TOP_LEFT, 4, 4, ACT_SETTINGS, &lv_font_montserrat_20);
}

static void build_splash(void) {
    clear_screen();
    /* The logo is black-on-white; put the whole splash on white so it blends. */
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_white(), LV_PART_MAIN);

    lv_obj_t *logo = lv_img_create(lv_scr_act());
    lv_img_set_src(logo, &logo_img);
    lv_obj_align(logo, LV_ALIGN_CENTER, 0, -22);

    make_label(lv_scr_act(), "Payment terminal", lv_color_black(),
               &lv_font_montserrat_14, LV_ALIGN_CENTER, 0, 80);
}

static void build_amount(void) {
    clear_screen();
    make_label(lv_scr_act(), "Amount", COL_DIM, &lv_font_montserrat_14,
               LV_ALIGN_TOP_MID, 0, 14);
    make_divider(lv_scr_act(), 34);
    add_menu_button();

    char buf[32];
    format_amount(s_amount_units, buf, sizeof(buf));
    s_amount_label = make_label(lv_scr_act(), buf, COL_TEXT,
                                &lv_font_montserrat_48, LV_ALIGN_TOP_MID, 0, 46);
    make_label(lv_scr_act(), "USDC", COL_DIM, &lv_font_montserrat_20,
               LV_ALIGN_TOP_MID, 0, 104);

    /* Stepper row: -1 / +1 on the left, -.01 / +.01 on the right. */
    make_button(lv_scr_act(), "-1", COL_SURFACE, COL_TEXT, 70, 44,
                LV_ALIGN_LEFT_MID, 8, 20, ACT_MINUS_U, &lv_font_montserrat_20);
    make_button(lv_scr_act(), "+1", COL_SURFACE, COL_TEXT, 70, 44,
                LV_ALIGN_LEFT_MID, 8, -28, ACT_PLUS_U, &lv_font_montserrat_20);
    make_button(lv_scr_act(), "-.01", COL_SURFACE, COL_TEXT, 70, 44,
                LV_ALIGN_RIGHT_MID, -8, 20, ACT_MINUS_C, &lv_font_montserrat_20);
    make_button(lv_scr_act(), "+.01", COL_SURFACE, COL_TEXT, 70, 44,
                LV_ALIGN_RIGHT_MID, -8, -28, ACT_PLUS_C, &lv_font_montserrat_20);

    make_button(lv_scr_act(), "Continue", COL_ACCENT, COL_BG, 180, 46,
                LV_ALIGN_BOTTOM_MID, 0, -8, ACT_CONFIRM, &lv_font_montserrat_20);
}

static void build_confirm(void) {
    clear_screen();
    make_label(lv_scr_act(), "Confirm", COL_DIM, &lv_font_montserrat_14,
               LV_ALIGN_TOP_MID, 0, 12);
    make_divider(lv_scr_act(), 32);
    add_menu_button();

    char buf[40];
    format_amount(s_confirm_amount, buf, sizeof(buf));
    strncat(buf, " USDC", sizeof(buf) - strlen(buf) - 1);
    make_label(lv_scr_act(), buf, COL_TEXT, &lv_font_montserrat_28,
               LV_ALIGN_TOP_MID, 0, 44);

    make_label(lv_scr_act(), "To", COL_DIM, &lv_font_montserrat_14,
               LV_ALIGN_TOP_MID, 0, 84);
    lv_obj_t *addr = make_label(lv_scr_act(),
                                s_confirm_addr[0] ? s_confirm_addr : "-",
                                COL_TEXT, &lv_font_montserrat_14,
                                LV_ALIGN_TOP_MID, 0, 104);
    lv_label_set_long_mode(addr, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(addr, 280);
    lv_obj_set_style_text_align(addr, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    make_button(lv_scr_act(), "Cancel", COL_SURFACE, COL_TEXT, 130, 46,
                LV_ALIGN_BOTTOM_LEFT, 12, -8, ACT_CANCEL, &lv_font_montserrat_20);
    make_button(lv_scr_act(), "Send", COL_ACCENT, COL_BG, 130, 46,
                LV_ALIGN_BOTTOM_RIGHT, -12, -8, ACT_SEND, &lv_font_montserrat_20);
}

static void build_tx_status(void) {
    clear_screen();
    make_label(lv_scr_act(), "Transaction", COL_DIM, &lv_font_montserrat_14,
               LV_ALIGN_TOP_MID, 0, 12);
    make_divider(lv_scr_act(), 32);
    add_menu_button();

    const char *state_str = "";
    lv_color_t  color      = COL_TEXT;
    bool        show_new   = false;
    bool        show_cancel = false;
    switch (s_tx_state) {
        case UI_TX_STATE_PLACE_CARD: state_str = "Place card";      show_cancel = true; break;
        case UI_TX_STATE_SIGNING:    state_str = "Signing...";      break;
        case UI_TX_STATE_SENDING:    state_str = "Broadcasting..."; break;
        case UI_TX_STATE_DONE:       state_str = "Sent"; color = COL_SUCCESS; show_new = true; break;
        case UI_TX_STATE_FAILED:     state_str = "Failed"; color = COL_DANGER; show_new = true; break;
    }

    make_label(lv_scr_act(), state_str, color, &lv_font_montserrat_28,
               LV_ALIGN_TOP_MID, 0, 56);

    lv_obj_t *info = make_label(lv_scr_act(), s_tx_info, COL_DIM,
                                &lv_font_montserrat_14, LV_ALIGN_CENTER, 0, 10);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(info, 290);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    if (show_new) {
        make_button(lv_scr_act(), "New payment", COL_ACCENT, COL_BG, 180, 46,
                    LV_ALIGN_BOTTOM_MID, 0, -8, ACT_NEW, &lv_font_montserrat_20);
    } else if (show_cancel) {
        make_button(lv_scr_act(), "Cancel", COL_SURFACE, COL_TEXT, 160, 46,
                    LV_ALIGN_BOTTOM_MID, 0, -8, ACT_CANCEL, &lv_font_montserrat_20);
    }
}

static void render_requested_screen(void) {
    switch (s_req_screen) {
        case UI_SCREEN_SPLASH:    build_splash();    break;
        case UI_SCREEN_AMOUNT:    build_amount();    break;
        case UI_SCREEN_CONFIRM:   build_confirm();   break;
        case UI_SCREEN_TX_STATUS: build_tx_status(); break;
    }
}

/******************************************************************
 * 9. UI task — owns LVGL init and the handler loop
 ******************************************************************/
static void ui_task(void *arg) {
    (void)arg;

    lv_init();

    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);

    touchSPI.begin(T_CLK, T_MISO, T_MOSI, T_CS);
    touch.begin(touchSPI);
    touch.setRotation(1);

    /* Take over the backlight pin with LEDC PWM (after tft.init has touched
     * it) so brightness is dimmable from the settings menu. */
    backlight_init(s_brightness);

    lv_disp_draw_buf_init(&s_draw_buf, s_buf, NULL, SCR_W * 40);
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res  = SCR_W;
    s_disp_drv.ver_res  = SCR_H;
    s_disp_drv.flush_cb = disp_flush;
    s_disp_drv.draw_buf = &s_draw_buf;
    lv_disp_drv_register(&s_disp_drv);

    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type    = LV_INDEV_TYPE_POINTER;
    s_indev_drv.read_cb = indev_read;
    lv_indev_drv_register(&s_indev_drv);

    const esp_timer_create_args_t targs = {
        .callback        = &tick_cb,
        .arg             = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "lv_tick",
        .skip_unhandled_events = true,
    };
    esp_timer_handle_t th;
    if (esp_timer_create(&targs, &th) == ESP_OK) {
        (void)esp_timer_start_periodic(th, LV_TICK_PERIOD_MS * 1000);
    }

    ESP_LOGI(TAG, "UI initialized (LVGL %d.%d + TFT_eSPI/XPT2046)",
             lv_version_major(), lv_version_minor());

    while (true) {
        if (s_screen_dirty) {
            s_screen_dirty = false;
            render_requested_screen();
        }
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/******************************************************************
 * 10. Public API
 ******************************************************************/
extern "C" void ui_init(ui_event_cb_t cb) {
    s_cb           = cb;
    s_req_screen   = UI_SCREEN_SPLASH;
    s_screen_dirty = true;
    /* LVGL needs a generous stack for rendering; pin nothing special. */
    xTaskCreate(ui_task, "ui", 8192, NULL, 4, NULL);
}

extern "C" void ui_show_splash(void) {
    request_screen(UI_SCREEN_SPLASH);
}

extern "C" void ui_show_amount_entry(void) {
    request_screen(UI_SCREEN_AMOUNT);
}

extern "C" void ui_show_confirm(uint64_t amount_units, const char *dest_addr) {
    s_confirm_amount = amount_units;
    if (dest_addr != NULL) {
        strncpy(s_confirm_addr, dest_addr, sizeof(s_confirm_addr) - 1);
        s_confirm_addr[sizeof(s_confirm_addr) - 1] = '\0';
    } else {
        s_confirm_addr[0] = '\0';
    }
    request_screen(UI_SCREEN_CONFIRM);
}

extern "C" void ui_show_tx_status(ui_tx_state_t state, const char *info) {
    s_tx_state = state;
    if (info != NULL) {
        strncpy(s_tx_info, info, sizeof(s_tx_info) - 1);
        s_tx_info[sizeof(s_tx_info) - 1] = '\0';
    } else {
        s_tx_info[0] = '\0';
    }
    request_screen(UI_SCREEN_TX_STATUS);
}
