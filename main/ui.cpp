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
#include "logo_small.h"
#include "usdc_logo.h"
#include "card_img.h"
#include "settings.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_system.h"   /* esp_restart() for factory reset */
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

/* Portrait orientation (240x320) — natural for a hand-held POS terminal. */
#define SCR_W   240
#define SCR_H   320

static TFT_eSPI            tft;
static SPIClass            touchSPI(VSPI);
static XPT2046_Touchscreen touch(T_CS, T_IRQ);

/******************************************************************
 * 3. Theme — light, minimal: white bg, black ink
 ******************************************************************/
#define COL_BG       lv_color_hex(0xFFFFFF)   /* white — page background       */
#define COL_SURFACE  lv_color_hex(0xF2F2F2)   /* light grey — cards/secondary  */
#define COL_TEXT     lv_color_hex(0x000000)   /* black — primary text          */
#define COL_DIM      lv_color_hex(0x9A9A9A)   /* grey — secondary labels       */
#define COL_TITLE    lv_color_hex(0x424242)   /* dark grey — screen titles     */
#define COL_ACCENT   lv_color_hex(0x000000)   /* black — primary action button */
#define COL_SUCCESS  lv_color_hex(0x1E9E50)   /* green — "Sent"                */
#define COL_DANGER   lv_color_hex(0xD63A3A)   /* red — failures / reset        */
#define COL_BORDER   lv_color_hex(0xE0E0E0)   /* light grey — hairlines        */

/******************************************************************
 * 3b. Layout metrics — shared so every screen's header lines up
 ******************************************************************/
#define HDR_TITLE_Y     11     /* title offset — optically centred in the
                                  42px band above the divider (montserrat_20) */
#define HDR_DIVIDER_Y   42     /* rule under the title                */
#define ACT_BTN_H       46     /* bottom action-button height         */
#define ACT_BTN_Y       (-8)   /* bottom action-button offset         */
#define MENU_BTN_W      42
#define MENU_BTN_H      30
#define MENU_BTN_X      4
#define MENU_BTN_Y      6

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

/* Swallow taps carried over from the screen we just left: a press is ignored
 * until this tick (ms) AND until the finger has been released at least once.
 * Set on every screen (re)build in render_requested_screen(). */
static uint32_t s_input_block_until = 0;
static bool     s_wait_release      = false;

static void indev_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    (void)drv;
    int16_t x, y;
    bool pressed = touch_to_screen(&x, &y);

    /* After a screen change, ignore a lingering or reflexive tap from the old
     * screen (e.g. cancelling the tx right after validating the PIN). */
    if (lv_tick_get() < s_input_block_until || s_wait_release) {
        if (!pressed) { s_wait_release = false; }   /* finger lifted — re-arm */
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    if (pressed) {
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

static uint8_t s_brightness = 80;   /* backlight %, restored from NVS */

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

/* No ambient-light sensing: the enclosure covers the LDR on GPIO 34, so any
 * reading is of the inside of the case. Brightness is the slider only. */

/******************************************************************
 * 5. Shared state (written by ui_show_* on the main task, read by ui_task)
 ******************************************************************/
static ui_event_cb_t s_cb = NULL;

static volatile bool        s_screen_dirty = true;
static volatile ui_screen_t s_req_screen   = UI_SCREEN_SPLASH;

/* Amount entry — parsed from the keypad string; starts empty (0). */
static uint64_t s_amount_units = 0ULL;

/* Confirm-screen payload */
static uint64_t s_confirm_amount = 0ULL;
static char     s_confirm_addr[64] = "";

/* Tx-status payload */
static ui_tx_state_t s_tx_state    = UI_TX_STATE_PLACE_CARD;
static char          s_tx_info[64] = "";

/* Amount entry — keypad input string (e.g. "12.50") and its display label. */
static lv_obj_t *s_amount_label = NULL;
static lv_obj_t *s_amount_usdc  = NULL;   /* USDC badge, kept beside the amount */
static uint64_t  s_amount_cents = 0;      /* amount entered, in cents          */

/* PIN entry — the textarea (password mode) is the live input; s_pin is the
 * handoff buffer read by main via ui_take_pin() and wiped on read. */
static lv_obj_t *s_pin_ta      = NULL;
static char      s_pin[16]     = {0};
static uint8_t   s_pin_len     = 0;

/* Wi-Fi picker */
#define WIFI_MAX_APS 16
static net_wifi_ap_t s_aps[WIFI_MAX_APS];
static uint16_t      s_ap_count = 0;
static char          s_wifi_ssid[33] = {0};   /* selected network          */
static char          s_wifi_pass[65] = {0};   /* entered passphrase (handoff) */
static char          s_wifi_msg[64]  = {0};   /* "Scanning…" / "Connecting to <ssid>…" */
static lv_obj_t     *s_wifi_pass_ta  = NULL;
static lv_obj_t     *s_wifi_eye_lbl  = NULL;   /* glyph swapped on reveal/hide */

/* Admin code (burger-menu lock). 4 digits on purpose: the threat is a customer
 * left alone with the terminal for a minute, which the escalating penalty below
 * already defeats. Someone with days of unattended physical access is out of
 * scope for this code — the funds are behind the card PIN, not behind it. The
 * merchant can always choose a longer one, up to ADMIN_CODE_MAX. */
#define ADMIN_CODE_MIN   4
#define ADMIN_CODE_MAX   9
static lv_obj_t     *s_admin_ta      = NULL;
static lv_obj_t     *s_admin_note_lbl = NULL;
static char          s_admin_first[ADMIN_CODE_MAX + 1] = {0};  /* 1st of 2 passes */
static char          s_admin_note[48] = {0};
static bool          s_admin_confirming = false;   /* 2nd pass of the creation */
static bool          s_welcome_sent     = false;   /* Start already reported */
/* Penalty clock, monotonic since boot (lv_tick_elaps handles the wrap). The
 * attempt count itself lives in NVS, so power-cycling shortens the current wait
 * but never resets the escalation. */
static uint32_t      s_admin_lock_start = 0;
static uint32_t      s_admin_lock_ms    = 0;

/* Destination info shown on the settings "Tx" tab (set by main, static). */
static const char *s_addr_usdc = NULL;
static const char *s_addr_dest = NULL;

/* Settings bottom-bar buttons (Reset shares the line with Close on About). */
static lv_obj_t *s_reset_btn = NULL;
static lv_obj_t *s_close_btn = NULL;

/* Tx-tab gas fees (Gwei). Loaded from settings on build, edited via +/- steppers,
 * persisted on close. */
static lv_obj_t *s_maxfee_lbl  = NULL;
static lv_obj_t *s_prio_lbl    = NULL;
static uint32_t  s_maxfee_gwei = 0U;
static uint32_t  s_prio_gwei   = 0U;

/******************************************************************
 * 6. Button actions
 ******************************************************************/
enum BtnAction {
    ACT_CONFIRM, ACT_CANCEL, ACT_SEND, ACT_NEW,
    ACT_SETTINGS, ACT_CLOSE, ACT_PIN_CANCEL,
    ACT_WIFI, ACT_WIFI_CANCEL, ACT_WIFI_PASS_REVEAL,
    ACT_ADMIN_CANCEL, ACT_WELCOME_OK,
    ACT_RESET, ACT_RESET_CONFIRM, ACT_RESET_CANCEL,
};

/* Firmware version shown on the splash and the About tab. */
#define APP_VERSION "v1.0"

/* Settings — defined in section 7 (uses the widget helpers). */
static void settings_persist(void);
static void open_reset_confirm(void);
static void close_reset_confirm(void);
static void pop_in(lv_obj_t *obj);   /* defined in section 8 (animations) */
static uint32_t admin_penalty_ms(uint8_t fails);   /* defined with the admin screens */
static ui_screen_t s_settings_return = UI_SCREEN_AMOUNT;   /* screen to go back to */

static void request_screen(ui_screen_t s) {
    s_req_screen   = s;
    s_screen_dirty = true;
}

static void format_amount(uint64_t units, char *out, size_t n) {
    uint64_t whole = units / 1000000ULL;
    uint64_t cents = (units % 1000000ULL) / 10000ULL;
    snprintf(out, n, "%" PRIu64 ".%02" PRIu64, whole, cents);
}

#define AMOUNT_CENTS_MAX  9999999ULL   /* 99999.99 */

static void amount_update_display(void) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%" PRIu64 ".%02" PRIu64,
             s_amount_cents / 100ULL, s_amount_cents % 100ULL);
    if (s_amount_label != NULL) {
        lv_label_set_text(s_amount_label, buf);
        /* Keep the USDC badge glued to the right of the (variable-width) amount. */
        if (s_amount_usdc != NULL) {
            lv_obj_align_to(s_amount_usdc, s_amount_label,
                            LV_ALIGN_OUT_RIGHT_MID, 8, 0);
        }
    }
    s_amount_units = s_amount_cents * 10000ULL;   /* cents -> 6-decimal base units */
}

/* Keypad on the amount screen — cents entry: each digit shifts in
 * from the right (1 -> 0.01, 12 -> 0.12, 1250 -> 12.50). No decimal point. */
static void amount_kbd_cb(lv_event_t *e) {
    lv_obj_t *bm = lv_event_get_target(e);
    const char *txt = lv_btnmatrix_get_btn_text(bm, lv_btnmatrix_get_selected_btn(bm));
    if (txt == NULL) { return; }

    if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
        s_amount_cents /= 10ULL;
    } else if (strcmp(txt, "00") == 0) {
        uint64_t n = s_amount_cents * 100ULL;
        s_amount_cents = (n > AMOUNT_CENTS_MAX) ? AMOUNT_CENTS_MAX : n;
    } else if ((txt[0] >= '0') && (txt[0] <= '9') && (txt[1] == '\0')) {
        uint64_t n = (s_amount_cents * 10ULL) + static_cast<uint64_t>(txt[0] - '0');
        if (n <= AMOUNT_CENTS_MAX) { s_amount_cents = n; }
    }
    amount_update_display();
}

/* Runs on the UI task (inside lv_timer_handler), so touching shared state and
 * invoking s_cb (which only posts to a queue) is safe here. */
static void btn_event_cb(lv_event_t *e) {
    BtnAction act = static_cast<BtnAction>(
        reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));

    switch (act) {
        case ACT_CONFIRM:
            if (s_cb != NULL && s_amount_units > 0ULL) {
                s_cb(UI_EVENT_AMOUNT_CONFIRMED, s_amount_units);
            }
            break;
        case ACT_CANCEL:
            if (s_cb != NULL) { s_cb(UI_EVENT_CONFIRM_CANCEL, 0); }
            break;
        case ACT_SEND:
            /* The PIN is no longer hard-coded: collect it on the keypad screen
             * before signing. */
            request_screen(UI_SCREEN_PIN);
            break;
        case ACT_PIN_CANCEL:
            CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(s_pin), sizeof(s_pin));
            s_pin_len = 0;
            request_screen(UI_SCREEN_CONFIRM);
            break;
        case ACT_NEW:
            if (s_cb != NULL) { s_cb(UI_EVENT_TX_RETRY, 0); }
            break;
        case ACT_SETTINGS:
            s_settings_return = s_req_screen;   /* remember where we came from */
            /* One lock for the whole menu: Wi-Fi, fee caps and the factory reset
             * are all merchant operations, and the reset in particular must not
             * be one tap away from a customer left alone with the terminal.
             *
             * No code stored means first-run setup has not finished, so the tap
             * is ignored rather than let through. main creates the code before
             * anything else, so this state is never reachable for long — but it
             * WAS reachable, by backing out of the first-run Wi-Fi picker onto
             * the amount screen while main was still waiting. */
            if (!settings_has_admin_code()) { break; }
            s_admin_confirming = false;
            s_admin_note[0]    = '\0';
            /* Re-arm the wait from the persisted attempt count. The wait itself
             * has to live in RAM — persisting a deadline would need a trustworthy
             * absolute clock, and the wall clock is exactly what an attacker on
             * the network can move — so a power cycle used to clear it and bring
             * the cost of one guess down to a single reboot. Deriving it here
             * instead makes the escalation survive reboots, at no extra write. */
            s_admin_lock_ms    = admin_penalty_ms(settings_admin_fail_count());
            s_admin_lock_start = lv_tick_get();
            request_screen(UI_SCREEN_ADMIN_UNLOCK);
            break;
        case ACT_WELCOME_OK:
            /* main answers by showing the code screen straight away, so there is
             * nothing to fill here. Guarded all the same: request_screen only
             * takes effect on the UI task's next pass, so a double tap could
             * otherwise emit twice. */
            if (!s_welcome_sent) {
                s_welcome_sent = true;
                if (s_cb != NULL) { s_cb(UI_EVENT_WELCOME_DONE, 0); }
            }
            break;
        case ACT_ADMIN_CANCEL:
            CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(s_admin_first),
                                  sizeof(s_admin_first));
            request_screen(UI_SCREEN_AMOUNT);
            break;
        case ACT_CLOSE:
            settings_persist();
            request_screen(s_settings_return);
            break;
        case ACT_WIFI:
            settings_persist();
            strncpy(s_wifi_msg, "Scanning...", sizeof(s_wifi_msg) - 1);
            s_wifi_msg[sizeof(s_wifi_msg) - 1] = '\0';
            request_screen(UI_SCREEN_WIFI_CONNECTING);
            if (s_cb != NULL) { s_cb(UI_EVENT_WIFI_SCAN, 0); }
            break;
        case ACT_WIFI_CANCEL:
            CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(s_wifi_pass),
                                  sizeof(s_wifi_pass));
            request_screen(UI_SCREEN_AMOUNT);
            break;
        case ACT_WIFI_PASS_REVEAL:
            /* Toggled in place, never via request_screen() — rebuilding the
             * screen would discard what the operator has typed. */
            if (s_wifi_pass_ta != NULL) {
                bool masked = lv_textarea_get_password_mode(s_wifi_pass_ta);
                lv_textarea_set_password_mode(s_wifi_pass_ta, !masked);
                if (s_wifi_eye_lbl != NULL) {
                    /* Glyph shows what the next tap does. */
                    lv_label_set_text(s_wifi_eye_lbl,
                                      masked ? LV_SYMBOL_EYE_CLOSE
                                             : LV_SYMBOL_EYE_OPEN);
                }
            }
            break;
        case ACT_RESET:
            open_reset_confirm();            /* ask before wiping */
            break;
        case ACT_RESET_CONFIRM:
            /* Wipe stored settings (brightness, auto, Wi-Fi) and reboot — the
             * device comes back up into first-run Wi-Fi setup. */
            settings_factory_reset();
            esp_restart();
            break;
        case ACT_RESET_CANCEL:
            close_reset_confirm();
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

    /* Base look — rounded, FLAT fill (kill the default theme's gradient, which
     * washes a dark fill toward light), no default border. */
    lv_obj_set_style_bg_color(btn, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_NONE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 10, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);

    /* Flat — no drop shadow (light, minimal look). */
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);

    /* Secondary (surface) buttons get a hairline border to lift them off the bg.
     * (Compare the raw RGB565 value — lv_color_eq isn't in this LVGL build.) */
    if (bg.full == COL_SURFACE.full) {
        lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(btn, COL_BORDER, LV_PART_MAIN);
    }

    /* Pressed feedback: darken the fill. */
    lv_obj_set_style_bg_color(btn, lv_color_mix(lv_color_black(), bg, 70),
                              LV_PART_MAIN | LV_STATE_PRESSED);

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
    s_amount_usdc  = NULL;
    s_pin_ta       = NULL;   /* deleted by lv_obj_clean — drop the dangling ref */
    s_wifi_pass_ta = NULL;
    s_wifi_eye_lbl   = NULL;
    s_admin_ta       = NULL;
    s_admin_note_lbl = NULL;
    s_reset_btn    = NULL;
    s_close_btn    = NULL;
    s_maxfee_lbl   = NULL;
    s_prio_lbl     = NULL;
}

/******************************************************************
 * 7b. Settings — full-screen page (burger menu)
 ******************************************************************/
static void settings_persist(void) {
    settings_set_brightness(s_brightness);
    settings_set_max_fee_gwei(s_maxfee_gwei);
    settings_set_priority_fee_gwei(s_prio_gwei);
}

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

/* Show the Reset button (and shrink Close) only on the About tab. */
static void tab_change_cb(lv_event_t *e) {
    lv_obj_t *tabbar = lv_event_get_target(e);
    bool about = (lv_btnmatrix_get_selected_btn(tabbar) == 3U);   /* About is tab 3 */
    if (s_reset_btn != NULL) {
        if (about) { lv_obj_clear_flag(s_reset_btn, LV_OBJ_FLAG_HIDDEN); }
        else       { lv_obj_add_flag(s_reset_btn, LV_OBJ_FLAG_HIDDEN); }
    }
    if (s_close_btn != NULL) {
        if (about) {
            lv_obj_set_width(s_close_btn, 108);
            lv_obj_align(s_close_btn, LV_ALIGN_BOTTOM_RIGHT, -10, ACT_BTN_Y);
        } else {
            lv_obj_set_width(s_close_btn, 232);
            lv_obj_align(s_close_btn, LV_ALIGN_BOTTOM_MID, 0, ACT_BTN_Y);
        }
    }
}

/* Gas-fee stepper bounds (Gwei). */
#define FEE_MIN_GWEI   1U
#define FEE_MAX_GWEI   500U
#define FEE_STEP_GWEI  5U

static void fee_update_labels(void) {
    char b[12];
    if (s_maxfee_lbl != NULL) {
        snprintf(b, sizeof(b), "%u", static_cast<unsigned>(s_maxfee_gwei));
        lv_label_set_text(s_maxfee_lbl, b);
    }
    if (s_prio_lbl != NULL) {
        snprintf(b, sizeof(b), "%u", static_cast<unsigned>(s_prio_gwei));
        lv_label_set_text(s_prio_lbl, b);
    }
}

/* user_data encodes which fee and direction: 0 max-, 1 max+, 2 prio-, 3 prio+. */
static void fee_step_cb(lv_event_t *e) {
    intptr_t code = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    uint32_t *v   = (code < 2) ? &s_maxfee_gwei : &s_prio_gwei;
    bool inc      = (code & 1) != 0;

    if (inc) {
        *v = (*v + FEE_STEP_GWEI > FEE_MAX_GWEI) ? FEE_MAX_GWEI : *v + FEE_STEP_GWEI;
    } else {
        *v = (*v < FEE_MIN_GWEI + FEE_STEP_GWEI) ? FEE_MIN_GWEI : *v - FEE_STEP_GWEI;
    }
    /* The tip can never exceed the cap. */
    if (s_prio_gwei > s_maxfee_gwei) { s_prio_gwei = s_maxfee_gwei; }
    fee_update_labels();
}

/* One "caption  [-] value [+]" row in the Tx tab; returns the value label. */
static lv_obj_t *build_fee_row(lv_obj_t *parent, const char *cap, lv_coord_t y,
                               intptr_t dec_code, intptr_t inc_code) {
    make_label(parent, cap, COL_DIM, &lv_font_montserrat_14,
               LV_ALIGN_TOP_LEFT, 0, y);

    lv_obj_t *minus = lv_btn_create(parent);
    lv_obj_set_size(minus, 40, 34);
    lv_obj_align(minus, LV_ALIGN_TOP_LEFT, 0, y + 20);
    lv_obj_set_style_bg_color(minus, COL_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(minus, LV_GRAD_DIR_NONE, LV_PART_MAIN);
    lv_obj_set_style_radius(minus, 8, LV_PART_MAIN);
    lv_obj_set_style_border_width(minus, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(minus, COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(minus, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(minus, fee_step_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(dec_code));
    lv_obj_t *ml = lv_label_create(minus);
    lv_label_set_text(ml, "-");
    lv_obj_set_style_text_color(ml, COL_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(ml, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_center(ml);

    lv_obj_t *val = make_label(parent, "0", COL_TEXT, &lv_font_montserrat_20,
                               LV_ALIGN_TOP_LEFT, 52, y + 24);
    lv_obj_set_width(val, 78);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t *plus = lv_btn_create(parent);
    lv_obj_set_size(plus, 40, 34);
    lv_obj_align(plus, LV_ALIGN_TOP_LEFT, 140, y + 20);
    lv_obj_set_style_bg_color(plus, COL_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(plus, LV_GRAD_DIR_NONE, LV_PART_MAIN);
    lv_obj_set_style_radius(plus, 8, LV_PART_MAIN);
    lv_obj_set_style_border_width(plus, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(plus, COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(plus, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(plus, fee_step_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(inc_code));
    lv_obj_t *pl = lv_label_create(plus);
    lv_label_set_text(pl, "+");
    lv_obj_set_style_text_color(pl, COL_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(pl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_center(pl);

    return val;
}

static void build_settings(void) {
    clear_screen();   /* white, full screen */

    lv_obj_t *tv = lv_tabview_create(lv_scr_act(), LV_DIR_TOP, 42);
    lv_obj_set_size(tv, SCR_W, SCR_H - 54);
    lv_obj_align(tv, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(tv, COL_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(tv, 0, LV_PART_MAIN);

    /* Flat tab bar: white, no boxes, black underline under the active tab. */
    lv_obj_t *tabbar = lv_tabview_get_tab_btns(tv);
    lv_obj_set_style_bg_color(tabbar, COL_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(tabbar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tabbar, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_set_style_text_color(tabbar, COL_DIM, LV_PART_ITEMS);
    lv_obj_set_style_text_color(tabbar, COL_TEXT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_side(tabbar, LV_BORDER_SIDE_BOTTOM,
                                 LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(tabbar, COL_TEXT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_width(tabbar, 2, LV_PART_ITEMS | LV_STATE_CHECKED);

    lv_obj_t *t_screen = lv_tabview_add_tab(tv, "Screen");
    lv_obj_t *t_wifi   = lv_tabview_add_tab(tv, "Wi-Fi");
    lv_obj_t *t_tx     = lv_tabview_add_tab(tv, "Tx");
    lv_obj_t *t_about  = lv_tabview_add_tab(tv, "About");
    lv_obj_t *pages[4] = { t_screen, t_wifi, t_tx, t_about };
    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_bg_color(pages[i], COL_BG, LV_PART_MAIN);   /* flat white */
        lv_obj_set_style_border_width(pages[i], 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(pages[i], 12, LV_PART_MAIN);
        lv_obj_clear_flag(pages[i], LV_OBJ_FLAG_SCROLLABLE);
    }
    /* About and Tx can overflow — let them scroll vertically with a scrollbar. */
    lv_obj_t *scrollable[2] = { t_about, t_tx };
    for (int i = 0; i < 2; i++) {
        lv_obj_add_flag(scrollable[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(scrollable[i], LV_DIR_VER);
        lv_obj_set_scrollbar_mode(scrollable[i], LV_SCROLLBAR_MODE_AUTO);
    }

    /* ── Screen tab: brightness ── */
    make_label(t_screen, "Brightness", COL_TEXT, &lv_font_montserrat_14,
               LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *pct = make_label(t_screen, "", COL_DIM, &lv_font_montserrat_14,
                               LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_t *sl = lv_slider_create(t_screen);
    lv_obj_set_width(sl, 196);
    lv_obj_align(sl, LV_ALIGN_TOP_MID, 0, 32);
    lv_slider_set_range(sl, 10, 100);   /* never fully dark */
    lv_slider_set_value(sl, s_brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, COL_ACCENT, LV_PART_KNOB);
    lv_obj_add_event_cb(sl, brightness_event_cb, LV_EVENT_VALUE_CHANGED, pct);

    char b[8];
    snprintf(b, sizeof(b), "%u%%", static_cast<unsigned>(s_brightness));
    lv_label_set_text(pct, b);

    /* ── Wi-Fi tab: show the currently configured network, then a Scan button ── */
    char cur_ssid[33] = {0};
    char cur_pass[65];
    bool have_wifi = settings_get_wifi(cur_ssid, sizeof(cur_ssid),
                                       cur_pass, sizeof(cur_pass));
    CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(cur_pass), sizeof(cur_pass));

    make_label(t_wifi, "Current network", COL_DIM, &lv_font_montserrat_14,
               LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *cur = make_label(t_wifi, have_wifi ? cur_ssid : "Not configured",
                               COL_TEXT, &lv_font_montserrat_14,
                               LV_ALIGN_TOP_LEFT, 0, 20);
    lv_label_set_long_mode(cur, LV_LABEL_LONG_DOT);
    lv_obj_set_width(cur, 220);

    /* Link quality of the live association (snapshot at settings open),
     * as a caption/value pair like every other field. */
    int8_t rssi = 0;
    if (net_wifi_rssi(&rssi)) {
        const char *qual = (rssi >= -50) ? "Excellent" :
                           (rssi >= -60) ? "Good"      :
                           (rssi >= -70) ? "Fair"      : "Weak";
        char sig[32];
        snprintf(sig, sizeof(sig), "%s (%d dBm)", qual,
                 static_cast<int>(rssi));
        make_label(t_wifi, "Signal", COL_DIM, &lv_font_montserrat_14,
                   LV_ALIGN_TOP_LEFT, 0, 48);
        make_label(t_wifi, sig, COL_TEXT, &lv_font_montserrat_14,
                   LV_ALIGN_TOP_LEFT, 0, 68);
    }

    /* Right below the signal row (y68 + ~18px line + gap) — not pinned to the
     * tab bottom, which left a void under the info block. */
    make_button(t_wifi, LV_SYMBOL_WIFI " Scan networks", COL_ACCENT, COL_BG,
                lv_pct(100), ACT_BTN_H, LV_ALIGN_TOP_MID, 0, 104, ACT_WIFI,
                &lv_font_montserrat_20);

    /* ── Transaction tab: where the funds go (read-only) ── */
    make_label(t_tx, "USDC contract", COL_DIM, &lv_font_montserrat_14,
               LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *a_usdc = make_label(t_tx, (s_addr_usdc != NULL) ? s_addr_usdc : "-",
                                  COL_TEXT, &lv_font_montserrat_14,
                                  LV_ALIGN_TOP_LEFT, 0, 18);
    lv_label_set_long_mode(a_usdc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(a_usdc, 200);

    make_label(t_tx, "Send to", COL_DIM, &lv_font_montserrat_14,
               LV_ALIGN_TOP_LEFT, 0, 64);
    lv_obj_t *a_dest = make_label(t_tx, (s_addr_dest != NULL) ? s_addr_dest : "-",
                                  COL_TEXT, &lv_font_montserrat_14,
                                  LV_ALIGN_TOP_LEFT, 0, 82);
    lv_label_set_long_mode(a_dest, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(a_dest, 200);

    /* Editable EIP-1559 gas fees (Gwei), defaulting to the config.h values. */
    s_maxfee_gwei = settings_get_max_fee_gwei();
    s_prio_gwei   = settings_get_priority_fee_gwei();
    if (s_prio_gwei > s_maxfee_gwei) { s_prio_gwei = s_maxfee_gwei; }
    s_maxfee_lbl  = build_fee_row(t_tx, "Max fee (Gwei)",      128, 0, 1);
    s_prio_lbl    = build_fee_row(t_tx, "Priority fee (Gwei)", 196, 2, 3);
    fee_update_labels();

    /* ── About tab: small C logo, name, version, info ── */
    lv_obj_t *blogo = lv_img_create(t_about);
    lv_img_set_src(blogo, &logo_small);   /* 40px dedicated image */
    lv_obj_align(blogo, LV_ALIGN_TOP_MID, 0, 0);

    make_label(t_about, "cryptnox-pos", COL_TEXT, &lv_font_montserrat_20,
               LV_ALIGN_TOP_MID, 0, 42);
    make_label(t_about, APP_VERSION, COL_DIM, &lv_font_montserrat_14,
               LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_t *about = make_label(t_about,
                                 "USDC payment terminal for Cryptnox cards\n\n"
                                 "Based on cryptnox-sdk-esp32 1.0.0\n"
                                 "(c) Cryptnox 2026 - Educational use only\n\n"
                                 "Licensed under LGPL-3.0-or-later\n\n"
                                 "Third-party: ESP-IDF (Apache-2.0),\n"
                                 "LVGL (MIT), TFT_eSPI (FreeBSD/MIT),\n"
                                 "XPT2046_Touchscreen (MIT)",
                                 COL_DIM, &lv_font_montserrat_14,
                                 LV_ALIGN_TOP_MID, 0, 94);
    lv_label_set_long_mode(about, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(about, 210);
    lv_obj_set_style_text_align(about, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    /* Bottom bar: Close always; on the About tab a Reset joins it on the same
     * line (Reset left, Close right). On other tabs Close is full-width. */
    s_close_btn = make_button(lv_scr_act(), "Close", COL_ACCENT, COL_BG, 232, ACT_BTN_H,
                              LV_ALIGN_BOTTOM_MID, 0, ACT_BTN_Y, ACT_CLOSE,
                              &lv_font_montserrat_20);
    s_reset_btn = make_button(lv_scr_act(), "Reset", COL_DANGER, COL_TEXT, 108, ACT_BTN_H,
                              LV_ALIGN_BOTTOM_LEFT, 10, ACT_BTN_Y, ACT_RESET,
                              &lv_font_montserrat_20);
    lv_obj_add_flag(s_reset_btn, LV_OBJ_FLAG_HIDDEN);   /* shown only on About */
    lv_obj_add_event_cb(lv_tabview_get_tab_btns(tv), tab_change_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
}

/* Confirmation popup before a factory reset (floats above the settings modal). */
static lv_obj_t *s_confirm = NULL;

static void close_reset_confirm(void) {
    if (s_confirm != NULL) {
        lv_obj_del(s_confirm);
        s_confirm = NULL;
    }
}

static void open_reset_confirm(void) {
    if (s_confirm != NULL) { return; }

    s_confirm = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_confirm);
    lv_obj_set_size(s_confirm, SCR_W, SCR_H);
    lv_obj_set_style_bg_color(s_confirm, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_confirm, LV_OPA_70, LV_PART_MAIN);
    lv_obj_clear_flag(s_confirm, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_confirm, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *card = lv_obj_create(s_confirm);
    lv_obj_set_size(card, 224, 210);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, COL_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 14, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, COL_BORDER, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *msg = make_label(card, "Erase all settings\n(Wi-Fi, brightness, fees)\nand reboot?",
                               COL_TEXT, &lv_font_montserrat_14,
                               LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    make_button(card, "Erase", COL_DANGER, COL_TEXT, 196, 40,
                LV_ALIGN_BOTTOM_MID, 0, -58, ACT_RESET_CONFIRM, &lv_font_montserrat_20);
    make_button(card, "Cancel", COL_SURFACE, COL_TEXT, 196, 40,
                LV_ALIGN_BOTTOM_MID, 0, -10, ACT_RESET_CANCEL, &lv_font_montserrat_20);
}

/******************************************************************
 * 8. Screen builders
 ******************************************************************/
/* Borderless icon button (top-left) — just the glyph, no box/shadow. */
static lv_obj_t *make_icon_button(const char *sym, BtnAction act) {
    lv_obj_t *btn = lv_btn_create(lv_scr_act());
    lv_obj_set_size(btn, MENU_BTN_W, MENU_BTN_H);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, MENU_BTN_X, MENU_BTN_Y);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(act)));
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, sym);
    lv_obj_set_style_text_color(lbl, COL_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_center(lbl);
    return btn;
}

/* Burger (top-left) that opens the settings modal — symbol only. */
static void add_menu_button(void) {
    (void)make_icon_button(LV_SYMBOL_LIST, ACT_SETTINGS);
}

/* Identical header on every screen: eyebrow title + rule + burger menu. */
/* Title + divider only — the burger (settings) lives solely on the amount
 * screen so settings can't be opened mid-transaction. */
static void build_header(const char *title) {
    make_label(lv_scr_act(), title, COL_TITLE, &lv_font_montserrat_20,
               LV_ALIGN_TOP_MID, 0, HDR_TITLE_Y);
    make_divider(lv_scr_act(), HDR_DIVIDER_Y);
}

/* First-run greeting. Same white/logo treatment as the splash, but this one waits
 * for a tap: it is the only moment the terminal has the operator's attention
 * before the setup steps start asking for things. */
static void build_welcome(void) {
    clear_screen();
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_white(), LV_PART_MAIN);

    /* Logo is 120x120: -70 puts it 30 px off the top edge and still leaves 18 px
     * before the text. The whole column is hand-balanced — moving one offset
     * eats into a neighbour, the screen has no slack left. */
    lv_obj_t *logo = lv_img_create(lv_scr_act());
    lv_img_set_src(logo, &logo_img);
    lv_obj_align(logo, LV_ALIGN_CENTER, 0, -70);
    pop_in(logo);   /* settled screen, so the flourish is welcome here */

    /* "Thank you for choosing Cryptnox POS." split over two lines: the product
     * name stays black to carry the sentence, the lead-in is grey. */
    make_label(lv_scr_act(), "Thank you for choosing", COL_DIM,
               &lv_font_montserrat_14, LV_ALIGN_CENTER, 0, 16);
    lv_obj_t *brand = make_label(lv_scr_act(), "Cryptnox POS", COL_TEXT,
                                 &lv_font_montserrat_20, LV_ALIGN_CENTER, 0, 44);

    /* Anchored under the brand rather than to the screen centre: this sentence
     * sits within a few pixels of the wrap threshold at 216 px, so an absolute
     * offset would give a different gap depending on whether it takes one line
     * or two. Width and long mode first, so the measurement sees them. */
    lv_obj_t *sub = make_label(lv_scr_act(), "Let's configure your terminal.",
                               COL_DIM, &lv_font_montserrat_14,
                               LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_width(sub, SCR_W - 24);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_update_layout(brand);
    lv_obj_align_to(sub, brand, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

    (void)make_button(lv_scr_act(), "Start", COL_ACCENT, COL_BG,
                      SCR_W - 24, ACT_BTN_H, LV_ALIGN_BOTTOM_MID, 0, -10,
                      ACT_WELCOME_OK, &lv_font_montserrat_20);
}

static void build_splash(void) {
    clear_screen();
    /* The logo is black-on-white; put the whole splash on white so it blends. */
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_white(), LV_PART_MAIN);

    lv_obj_t *logo = lv_img_create(lv_scr_act());
    lv_img_set_src(logo, &logo_img);
    lv_obj_align(logo, LV_ALIGN_CENTER, 0, -36);
    pop_in(logo);   /* same overshoot entrance as the tx check/cross */

    /* Tight under the logo (the image carries its own breathing margin). */
    make_label(lv_scr_act(), "cryptnox-pos", lv_color_black(),
               &lv_font_montserrat_20, LV_ALIGN_CENTER, 0, 40);

    /* Boot feedback — the splash stays up while Wi-Fi/SNTP/RPC come up, so
     * show a discreet spinner instead of looking frozen. */
    lv_obj_t *sp = lv_spinner_create(lv_scr_act(), 1000, 60);
    lv_obj_set_size(sp, 28, 28);
    lv_obj_align(sp, LV_ALIGN_BOTTOM_MID, 0, -48);
    lv_obj_set_style_arc_width(sp, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_width(sp, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(sp, COL_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_arc_color(sp, COL_ACCENT, LV_PART_INDICATOR);
}

static void build_amount(void) {
    clear_screen();
    /* No title/divider here — the keypad needs the vertical space. Keep just
     * the burger (settings) top-left. */
    add_menu_button();

    s_amount_label = make_label(lv_scr_act(), "0.00", COL_TEXT,
                                &lv_font_montserrat_28, LV_ALIGN_TOP_MID, 0, 22);
    s_amount_usdc = lv_img_create(lv_scr_act());
    lv_img_set_src(s_amount_usdc, &usdc_logo);   /* placed beside the amount below */

    /* Numeric keypad: digits, double-zero, backspace (cents entry). */
    static const char *amap[] = {
        "1", "2", "3", "\n",
        "4", "5", "6", "\n",
        "7", "8", "9", "\n",
        "00", "0", LV_SYMBOL_BACKSPACE, ""
    };
    lv_obj_t *kb = lv_btnmatrix_create(lv_scr_act());
    lv_btnmatrix_set_map(kb, amap);
    lv_obj_set_size(kb, 232, 196);
    lv_obj_align(kb, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_bg_opa(kb, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(kb, 0, LV_PART_MAIN);
    /* Minimal keypad: no key boxes — black glyphs on white, grey flash on press. */
    lv_obj_set_style_bg_opa(kb, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, COL_SURFACE, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, COL_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_28, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 8, LV_PART_ITEMS);
    lv_obj_add_event_cb(kb, amount_kbd_cb, LV_EVENT_VALUE_CHANGED, NULL);

    make_button(lv_scr_act(), "Charge", COL_ACCENT, COL_BG, 232, ACT_BTN_H,
                LV_ALIGN_BOTTOM_MID, 0, ACT_BTN_Y, ACT_CONFIRM, &lv_font_montserrat_20);

    amount_update_display();   /* keep s_amount_units in sync with the string */
}

static void build_confirm(void) {
    clear_screen();
    build_header("Send");

    /* Ledger-style transaction review: dim caption / value rows, everything
     * the operator should verify — amount, beneficiary AND the USDC contract
     * the terminal is about to call. */
    char buf[24];
    format_amount(s_confirm_amount, buf, sizeof(buf));

    make_label(lv_scr_act(), "Amount", COL_DIM, &lv_font_montserrat_14,
               LV_ALIGN_TOP_LEFT, 12, 54);
    lv_obj_t *amt = make_label(lv_scr_act(), buf, COL_TEXT, &lv_font_montserrat_28,
                               LV_ALIGN_TOP_LEFT, 12, 72);
    lv_obj_t *cusdc = lv_img_create(lv_scr_act());
    lv_img_set_src(cusdc, &usdc_logo);
    lv_obj_align_to(cusdc, amt, LV_ALIGN_OUT_RIGHT_MID, 8, 0);

    make_label(lv_scr_act(), "To", COL_DIM, &lv_font_montserrat_14,
               LV_ALIGN_TOP_LEFT, 12, 118);
    lv_obj_t *addr = make_label(lv_scr_act(),
                                s_confirm_addr[0] ? s_confirm_addr : "-",
                                COL_TEXT, &lv_font_montserrat_14,
                                LV_ALIGN_TOP_LEFT, 12, 136);
    lv_label_set_long_mode(addr, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(addr, 216);

    make_label(lv_scr_act(), "USDC contract", COL_DIM, &lv_font_montserrat_14,
               LV_ALIGN_TOP_LEFT, 12, 182);
    lv_obj_t *ctr = make_label(lv_scr_act(),
                               (s_addr_usdc != NULL) ? s_addr_usdc : "-",
                               COL_TEXT, &lv_font_montserrat_14,
                               LV_ALIGN_TOP_LEFT, 12, 200);
    lv_label_set_long_mode(ctr, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ctr, 216);

    make_button(lv_scr_act(), "Cancel", COL_SURFACE, COL_TEXT, 104, ACT_BTN_H,
                LV_ALIGN_BOTTOM_LEFT, 10, ACT_BTN_Y, ACT_CANCEL, &lv_font_montserrat_20);
    make_button(lv_scr_act(), "Confirm", COL_ACCENT, COL_BG, 104, ACT_BTN_H,
                LV_ALIGN_BOTTOM_RIGHT, -10, ACT_BTN_Y, ACT_SEND, &lv_font_montserrat_20);
}

/* OK pressed on the keypad: stash the PIN for main and move to the tx screen. */
static void pin_submit(void) {
    if (s_pin_ta == NULL) { return; }
    const char *p = lv_textarea_get_text(s_pin_ta);
    size_t len = (p != NULL) ? strlen(p) : 0U;
    if (len < 4U) { return; }   /* require at least 4 digits */

    strncpy(s_pin, p, sizeof(s_pin) - 1);
    s_pin[sizeof(s_pin) - 1] = '\0';
    s_pin_len = static_cast<uint8_t>(strlen(s_pin));

    s_tx_state = UI_TX_STATE_PLACE_CARD;
    strncpy(s_tx_info, "Preparing...", sizeof(s_tx_info) - 1);
    s_tx_info[sizeof(s_tx_info) - 1] = '\0';
    request_screen(UI_SCREEN_TX_STATUS);
    if (s_cb != NULL) { s_cb(UI_EVENT_PIN_ENTERED, 0); }
}

static void pin_kbd_cb(lv_event_t *e) {
    lv_obj_t *bm = lv_event_get_target(e);
    uint32_t id = lv_btnmatrix_get_selected_btn(bm);
    const char *txt = lv_btnmatrix_get_btn_text(bm, id);
    if ((txt == NULL) || (s_pin_ta == NULL)) { return; }

    if (strcmp(txt, LV_SYMBOL_OK) == 0) {
        pin_submit();
    } else if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
        lv_textarea_del_char(s_pin_ta);
    } else if ((txt[0] >= '0') && (txt[0] <= '9') && (txt[1] == '\0')) {
        lv_textarea_add_char(s_pin_ta, static_cast<uint32_t>(txt[0]));
    }
}

/* Masked one-line code field, centred, with no soft-keyboard popup — the on-screen
 * keypad is the only input. Shared by the card PIN and the admin code. */
static lv_obj_t *make_code_field(uint32_t max_len, lv_coord_t y) {
    lv_obj_t *ta = lv_textarea_create(lv_scr_act());
    lv_textarea_set_password_mode(ta, true);
    /* Mask immediately: LVGL's default grace period would leave each digit in
     * clear for 1500 ms, handing a shoulder-surfer the code one key at a time. */
    lv_textarea_set_password_show_time(ta, 0);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, max_len);
    lv_textarea_set_text(ta, "");
    lv_obj_clear_flag(ta, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(ta, 160);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_text_align(ta, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ta, COL_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_text_color(ta, COL_TEXT, LV_PART_MAIN);
    lv_obj_set_style_border_color(ta, COL_BORDER, LV_PART_MAIN);
    return ta;
}

/* Numeric keypad: no key boxes — black glyphs on white, grey flash on press.
 * The map is static because lv_btnmatrix keeps the pointer. */
static lv_obj_t *make_numeric_keypad(lv_event_cb_t cb) {
    static const char *kbd_map[] = {
        "1", "2", "3", "\n",
        "4", "5", "6", "\n",
        "7", "8", "9", "\n",
        LV_SYMBOL_BACKSPACE, "0", LV_SYMBOL_OK, ""
    };
    lv_obj_t *kb = lv_btnmatrix_create(lv_scr_act());
    lv_btnmatrix_set_map(kb, kbd_map);
    lv_obj_set_size(kb, 232, 210);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_opa(kb, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(kb, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(kb, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, COL_SURFACE, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, COL_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_28, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 8, LV_PART_ITEMS);
    lv_obj_add_event_cb(kb, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return kb;
}

static void build_pin(void) {
    clear_screen();
    /* Wipe any stale PIN from a previous attempt. */
    CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(s_pin), sizeof(s_pin));
    s_pin_len = 0;

    make_label(lv_scr_act(), "Enter PIN", COL_TITLE, &lv_font_montserrat_20,
               LV_ALIGN_TOP_MID, 0, HDR_TITLE_Y);
    /* Back (cancel) icon, top-left like the burger on other screens. */
    (void)make_icon_button(LV_SYMBOL_LEFT, ACT_PIN_CANCEL);

    s_pin_ta = make_code_field(9U, 48);
    (void)make_numeric_keypad(pin_kbd_cb);
}

/**
 * @brief Wait imposed after repeated wrong admin codes.
 *
 * Free for the first three tries, then doubling, capped at 60 s. Never a
 * permanent lock: the code gates the factory reset too, so locking for good
 * would leave no way back in short of a USB reflash.
 */
static uint32_t admin_penalty_ms(uint8_t fails) {
    if (fails < 3U) { return 0U; }
    uint32_t shift = static_cast<uint32_t>(fails) - 3U;
    if (shift > 6U) { shift = 6U; }          /* cap before the shift overflows */
    uint32_t secs = 1U << shift;
    if (secs > 60U) { secs = 60U; }
    return secs * 1000U;
}

/* Seconds left on the penalty, 0 once it has elapsed. */
static uint32_t admin_lock_remaining_s(void) {
    if (s_admin_lock_ms == 0U) { return 0U; }
    const uint32_t elapsed = lv_tick_elaps(s_admin_lock_start);
    if (elapsed >= s_admin_lock_ms) {
        s_admin_lock_ms = 0U;
        return 0U;
    }
    return ((s_admin_lock_ms - elapsed) + 999U) / 1000U;
}

static void admin_set_note(const char *msg) {
    strncpy(s_admin_note, (msg != NULL) ? msg : "", sizeof(s_admin_note) - 1);
    s_admin_note[sizeof(s_admin_note) - 1] = '\0';
    if (s_admin_note_lbl != NULL) {
        lv_label_set_text(s_admin_note_lbl, s_admin_note);
    }
}

static void admin_submit(void) {
    if (s_admin_ta == NULL) { return; }

    char code[ADMIN_CODE_MAX + 1] = {0};
    const char *txt = lv_textarea_get_text(s_admin_ta);
    strncpy(code, (txt != NULL) ? txt : "", sizeof(code) - 1);
    code[sizeof(code) - 1] = '\0';

    if (s_req_screen == UI_SCREEN_ADMIN_SET) {
        if (!s_admin_confirming) {
            if (strlen(code) < ADMIN_CODE_MIN) {
                char msg[sizeof(s_admin_note)];
                (void)snprintf(msg, sizeof(msg), "At least %d digits",
                               ADMIN_CODE_MIN);
                admin_set_note(msg);   /* built, so it cannot drift from the limit */
            } else {
                strncpy(s_admin_first, code, sizeof(s_admin_first) - 1);
                s_admin_first[sizeof(s_admin_first) - 1] = '\0';
                s_admin_confirming = true;
                admin_set_note("");
                request_screen(UI_SCREEN_ADMIN_SET);   /* rebuild, confirm pass */
            }
        } else if (strcmp(code, s_admin_first) == 0) {
            if (!settings_set_admin_code(code)) {
                /* Stay put and say so. Reporting success here would send main on
                 * to Wi-Fi setup and leave a terminal whose menu — factory reset
                 * included — no code can ever open. */
                admin_set_note("Storage error - try again");
                lv_textarea_set_text(s_admin_ta, "");
                CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(code),
                                      sizeof(code));
                return;
            }
            CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(s_admin_first),
                                  sizeof(s_admin_first));
            s_admin_confirming = false;
            admin_set_note("");
            /* Not the amount screen: this is the first-run path, so main answers
             * by bringing the network up and that blocks on a scan for a couple
             * of seconds — the payment keypad would flash up and be replaced by
             * the Wi-Fi list. Say what is actually happening instead. */
            strncpy(s_wifi_msg, "Scanning...", sizeof(s_wifi_msg) - 1);
            s_wifi_msg[sizeof(s_wifi_msg) - 1] = '\0';
            request_screen(UI_SCREEN_WIFI_CONNECTING);
            if (s_cb != NULL) { s_cb(UI_EVENT_ADMIN_SET, 0); }
        } else {
            CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(s_admin_first),
                                  sizeof(s_admin_first));
            s_admin_confirming = false;
            admin_set_note("Codes did not match");
            request_screen(UI_SCREEN_ADMIN_SET);
        }
    } else {
        const uint32_t wait_s = admin_lock_remaining_s();
        char msg[sizeof(s_admin_note)];
        if (strlen(code) < ADMIN_CODE_MIN) {
            /* Too short to be any stored code, so don't spend an attempt on it.
             * Otherwise a few stray taps on OK push the counter into the penalty
             * and the merchant waits a minute for a menu nobody attacked. */
            admin_set_note("Enter your code");
        } else if (wait_s > 0U) {
            (void)snprintf(msg, sizeof(msg), "Too many tries - wait %us",
                           static_cast<unsigned>(wait_s));
            admin_set_note(msg);
        } else if (settings_check_admin_code(code)) {
            s_admin_lock_ms = 0U;
            request_screen(UI_SCREEN_SETTINGS);
        } else {
            const uint32_t penalty = admin_penalty_ms(settings_admin_fail_count());
            if (penalty > 0U) {
                s_admin_lock_start = lv_tick_get();
                s_admin_lock_ms    = penalty;
                (void)snprintf(msg, sizeof(msg), "Wrong code - wait %us",
                               static_cast<unsigned>(penalty / 1000U));
                admin_set_note(msg);
            } else {
                admin_set_note("Wrong code");
            }
            lv_textarea_set_text(s_admin_ta, "");
        }
    }
    CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(code), sizeof(code));
}

static void admin_kbd_cb(lv_event_t *e) {
    lv_obj_t *bm = lv_event_get_target(e);
    uint32_t id = lv_btnmatrix_get_selected_btn(bm);
    const char *txt = lv_btnmatrix_get_btn_text(bm, id);
    if ((txt == NULL) || (s_admin_ta == NULL)) { return; }

    if (strcmp(txt, LV_SYMBOL_OK) == 0) {
        admin_submit();
    } else if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
        lv_textarea_del_char(s_admin_ta);
    } else if ((txt[0] >= '0') && (txt[0] <= '9') && (txt[1] == '\0')) {
        lv_textarea_add_char(s_admin_ta, static_cast<uint32_t>(txt[0]));
    }
}

/* Shared body of both admin screens; only the title and the way out differ. */
static void build_admin_screen(const char *title, bool allow_cancel) {
    clear_screen();
    make_label(lv_scr_act(), title, COL_TITLE, &lv_font_montserrat_20,
               LV_ALIGN_TOP_MID, 0, HDR_TITLE_Y);
    if (allow_cancel) {
        (void)make_icon_button(LV_SYMBOL_LEFT, ACT_ADMIN_CANCEL);
    }

    s_admin_ta = make_code_field(ADMIN_CODE_MAX, 44);

    /* Note band above the keypad: wrong code, mismatch, or the remaining wait. */
    s_admin_note_lbl = make_label(lv_scr_act(), s_admin_note, COL_DANGER,
                                  &lv_font_montserrat_14, LV_ALIGN_TOP_MID, 0, 82);
    (void)make_numeric_keypad(admin_kbd_cb);
}

static void build_admin_set(void) {
    /* No way out: first-run setup is mandatory, since the whole menu — including
     * the factory reset — hides behind this code. */
    build_admin_screen(s_admin_confirming ? "Confirm code" : "Set admin code",
                       false);
}

static void build_admin_unlock(void) {
    build_admin_screen("Admin code", true);
}

/* Header with a back arrow (to amount entry) instead of the burger. */
static void build_header_back(const char *title) {
    make_label(lv_scr_act(), title, COL_TITLE, &lv_font_montserrat_20,
               LV_ALIGN_TOP_MID, 0, HDR_TITLE_Y);
    make_divider(lv_scr_act(), HDR_DIVIDER_Y);
    (void)make_icon_button(LV_SYMBOL_LEFT, ACT_WIFI_CANCEL);
}

static void wifi_item_cb(lv_event_t *e) {
    intptr_t idx = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    if ((idx < 0) || (idx >= s_ap_count)) { return; }
    strncpy(s_wifi_ssid, s_aps[idx].ssid, sizeof(s_wifi_ssid) - 1);
    s_wifi_ssid[sizeof(s_wifi_ssid) - 1] = '\0';
    request_screen(UI_SCREEN_WIFI_PASS);
}

static void build_wifi_list(void) {
    clear_screen();
    build_header_back("Wi-Fi");

    if (s_ap_count == 0U) {
        make_label(lv_scr_act(), "No networks found", COL_DIM,
                   &lv_font_montserrat_14, LV_ALIGN_CENTER, 0, 0);
        make_button(lv_scr_act(), "Rescan", COL_ACCENT, COL_BG, 140, ACT_BTN_H,
                    LV_ALIGN_BOTTOM_MID, 0, ACT_BTN_Y, ACT_WIFI,
                    &lv_font_montserrat_20);
        return;
    }

    lv_obj_t *list = lv_list_create(lv_scr_act());
    lv_obj_set_size(list, SCR_W - 12, SCR_H - 52);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_set_style_bg_color(list, COL_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list, 4, LV_PART_MAIN);

    for (uint16_t i = 0U; i < s_ap_count; i++) {
        lv_obj_t *btn = lv_list_add_btn(list, LV_SYMBOL_WIFI, s_aps[i].ssid);
        lv_obj_set_style_bg_color(btn, COL_SURFACE, LV_PART_MAIN);
        lv_obj_set_style_text_color(btn, COL_TEXT, LV_PART_MAIN);
        lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
        lv_obj_add_event_cb(btn, wifi_item_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(i)));
    }
}

static void wifi_pass_kb_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {            /* keyboard check-mark */
        if (s_wifi_pass_ta != NULL) {
            const char *p = lv_textarea_get_text(s_wifi_pass_ta);
            strncpy(s_wifi_pass, (p != NULL) ? p : "", sizeof(s_wifi_pass) - 1);
            s_wifi_pass[sizeof(s_wifi_pass) - 1] = '\0';
        }
        snprintf(s_wifi_msg, sizeof(s_wifi_msg), "Connecting to %s...", s_wifi_ssid);
        request_screen(UI_SCREEN_WIFI_CONNECTING);
        if (s_cb != NULL) { s_cb(UI_EVENT_WIFI_TRY, 0); }
    } else if (code == LV_EVENT_CANCEL) {     /* keyboard close */
        request_screen(UI_SCREEN_WIFI_LIST);
    }
}

static void build_wifi_pass(void) {
    clear_screen();
    make_label(lv_scr_act(), s_wifi_ssid, COL_TEXT, &lv_font_montserrat_14,
               LV_ALIGN_TOP_MID, 0, 6);

    s_wifi_pass_ta = lv_textarea_create(lv_scr_act());
    lv_textarea_set_one_line(s_wifi_pass_ta, true);
    lv_textarea_set_text(s_wifi_pass_ta, "");
    lv_textarea_set_placeholder_text(s_wifi_pass_ta, "Password");
    /* Masked by default: this screen faces the customer. The eye beside it
     * reveals on demand, for checking a long passphrase. */
    lv_textarea_set_password_mode(s_wifi_pass_ta, true);
    /* Mask immediately, with no grace period. LVGL otherwise leaves each new
     * character in clear for LV_TEXTAREA_DEF_PWD_SHOW_TIME (1500 ms here), which
     * on a customer-facing screen hands a shoulder-surfer the whole passphrase
     * one key at a time — the reveal is the eye's job, on the merchant's terms. */
    lv_textarea_set_password_show_time(s_wifi_pass_ta, 0);
    lv_obj_set_width(s_wifi_pass_ta, SCR_W - 24 - MENU_BTN_W);
    lv_obj_align(s_wifi_pass_ta, LV_ALIGN_TOP_LEFT, 12, 28);
    lv_obj_set_style_bg_color(s_wifi_pass_ta, COL_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_wifi_pass_ta, COL_TEXT, LV_PART_MAIN);

    /* make_icon_button() places itself top-left for the burger; move it beside
     * the field. Keep the label handle so the glyph can be swapped in place. */
    lv_obj_t *eye = make_icon_button(LV_SYMBOL_EYE_OPEN, ACT_WIFI_PASS_REVEAL);
    lv_obj_align_to(eye, s_wifi_pass_ta, LV_ALIGN_OUT_RIGHT_MID, 6, 0);
    s_wifi_eye_lbl = lv_obj_get_child(eye, 0);

    lv_obj_t *kb = lv_keyboard_create(lv_scr_act());
    lv_keyboard_set_textarea(kb, s_wifi_pass_ta);
    lv_obj_add_event_cb(kb, wifi_pass_kb_cb, LV_EVENT_ALL, NULL);
}

static void build_wifi_connecting(void) {
    clear_screen();
    build_header("Wi-Fi");
    make_label(lv_scr_act(), s_wifi_msg, COL_TEXT, &lv_font_montserrat_20,
               LV_ALIGN_CENTER, 0, 0);
}

/* Animate an object's zoom (256 = 100%) — used for the success-check pop. */
static void zoom_anim_cb(void *obj, int32_t v) {
    lv_obj_set_style_transform_zoom(static_cast<lv_obj_t *>(obj), v, LV_PART_MAIN);
}

/* Pop-in: scale from small to full with a slight overshoot/bounce. */
static void pop_in(lv_obj_t *obj) {
    lv_obj_update_layout(obj);
    lv_obj_set_style_transform_pivot_x(obj, lv_obj_get_width(obj) / 2, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(obj, lv_obj_get_height(obj) / 2, LV_PART_MAIN);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, zoom_anim_cb);
    lv_anim_set_values(&a, 10, 256);          /* ~4% -> 100% (pronounced pop) */
    lv_anim_set_time(&a, 420);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_anim_start(&a);
}

/* "<amount> [USDC logo]" centred at offset y from the top. */
static void tx_amount_row(const char *amt, const lv_font_t *font, lv_coord_t y) {
    lv_obj_t *al = make_label(lv_scr_act(), amt, COL_TEXT, font,
                              LV_ALIGN_TOP_MID, -16, y);
    lv_obj_t *u  = lv_img_create(lv_scr_act());
    lv_img_set_src(u, &usdc_logo);
    lv_obj_align_to(u, al, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
}

static void build_tx_status(void) {
    clear_screen();
    build_header("Transaction");

    char amt[24];
    format_amount(s_confirm_amount, amt, sizeof(amt));

    if (s_tx_state == UI_TX_STATE_PLACE_CARD) {
        lv_obj_t *card = lv_img_create(lv_scr_act());
        lv_img_set_src(card, &card_img);
        lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 64);

        make_label(lv_scr_act(), "Tap your card", COL_TEXT, &lv_font_montserrat_20,
                   LV_ALIGN_TOP_MID, 0, 172);

        char total[40];
        snprintf(total, sizeof(total), "Total %s", amt);
        tx_amount_row(total, &lv_font_montserrat_14, 206);

        make_button(lv_scr_act(), "Cancel", COL_SURFACE, COL_TEXT, 232, ACT_BTN_H,
                    LV_ALIGN_BOTTOM_MID, 0, ACT_BTN_Y, ACT_CANCEL, &lv_font_montserrat_20);
        return;
    }

    if (s_tx_state == UI_TX_STATE_DONE) {
        lv_obj_t *chk = make_label(lv_scr_act(), LV_SYMBOL_OK, COL_SUCCESS,
                                   &lv_font_montserrat_48, LV_ALIGN_TOP_MID, 0, 96);
        pop_in(chk);
        make_label(lv_scr_act(), "Approved", COL_TEXT, &lv_font_montserrat_20,
                   LV_ALIGN_TOP_MID, 0, 160);
        tx_amount_row(amt, &lv_font_montserrat_28, 192);

        make_button(lv_scr_act(), "New sale", COL_ACCENT, COL_BG, 232, ACT_BTN_H,
                    LV_ALIGN_BOTTOM_MID, 0, ACT_BTN_Y, ACT_NEW, &lv_font_montserrat_20);
        return;
    }

    if (s_tx_state == UI_TX_STATE_FAILED) {
        lv_obj_t *cross = make_label(lv_scr_act(), LV_SYMBOL_CLOSE, lv_color_hex(0xEC5B5B),
                                     &lv_font_montserrat_48, LV_ALIGN_TOP_MID, 0, 96);
        pop_in(cross);
        make_label(lv_scr_act(), "Declined", COL_TEXT,
                   &lv_font_montserrat_20, LV_ALIGN_TOP_MID, 0, 160);
        lv_obj_t *info = make_label(lv_scr_act(), s_tx_info, COL_DIM,
                                    &lv_font_montserrat_14, LV_ALIGN_TOP_MID, 0, 192);
        lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(info, 216);
        lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

        make_button(lv_scr_act(), "New sale", COL_ACCENT, COL_BG, 232, ACT_BTN_H,
                    LV_ALIGN_BOTTOM_MID, 0, ACT_BTN_Y, ACT_NEW, &lv_font_montserrat_20);
        return;
    }

    /* PROCESSING / SIGNING / SENDING — animated spinner + status text. The
     * spinner keeps turning because lv_timer_handler runs on the UI task while
     * the main task is busy connecting/signing/broadcasting. */
    const char *state_str =
        (s_tx_state == UI_TX_STATE_PROCESSING) ? "Processing" :
        (s_tx_state == UI_TX_STATE_SIGNING)    ? "Signing"    :
        (s_tx_state == UI_TX_STATE_CONFIRMING) ? "Confirming" : "Authorizing";

    lv_obj_t *sp = lv_spinner_create(lv_scr_act(), 1000, 60);
    lv_obj_set_size(sp, 60, 60);
    lv_obj_align(sp, LV_ALIGN_TOP_MID, 0, 82);
    lv_obj_set_style_arc_width(sp, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(sp, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(sp, COL_SURFACE, LV_PART_MAIN);      /* track */
    lv_obj_set_style_arc_color(sp, COL_ACCENT, LV_PART_INDICATOR);  /* moving arc */

    make_label(lv_scr_act(), state_str, COL_TEXT, &lv_font_montserrat_20,
               LV_ALIGN_TOP_MID, 0, 160);

    lv_obj_t *info = make_label(lv_scr_act(), s_tx_info, COL_DIM,
                                &lv_font_montserrat_14, LV_ALIGN_TOP_MID, 0, 192);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(info, 216);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

static void render_requested_screen(void) {
    switch (s_req_screen) {
        case UI_SCREEN_SPLASH:    build_splash();    break;
        case UI_SCREEN_AMOUNT:    build_amount();    break;
        case UI_SCREEN_CONFIRM:   build_confirm();   break;
        case UI_SCREEN_PIN:       build_pin();       break;
        case UI_SCREEN_WIFI_LIST: build_wifi_list(); break;
        case UI_SCREEN_WIFI_PASS: build_wifi_pass(); break;
        case UI_SCREEN_WIFI_CONNECTING: build_wifi_connecting(); break;
        case UI_SCREEN_SETTINGS:  build_settings();  break;
        case UI_SCREEN_TX_STATUS: build_tx_status(); break;
        case UI_SCREEN_ADMIN_SET:    build_admin_set();    break;
        case UI_SCREEN_ADMIN_UNLOCK: build_admin_unlock(); break;
        case UI_SCREEN_WELCOME:      build_welcome();      break;
    }

    /* Guard the freshly built screen against a tap carried over from the
     * previous one (see indev_read). The "Tap card" screen gets a longer
     * window because its Cancel button is destructive. */
    uint32_t lockout = 450U;
    if (s_req_screen == UI_SCREEN_TX_STATUS &&
        s_tx_state == UI_TX_STATE_PLACE_CARD) {
        lockout = 900U;
    }
    s_input_block_until = lv_tick_get() + lockout;
    s_wait_release      = true;
}

/******************************************************************
 * 9. UI task — owns LVGL init and the handler loop
 ******************************************************************/
static void ui_task(void *arg) {
    (void)arg;

    lv_init();

    tft.init();
    tft.setRotation(0);          /* portrait, 240x320 */
    tft.invertDisplay(true);     /* CYD ILI9341 panel renders inverted otherwise */

    /* CYD "milky gamma" fix: the 1-USB ILI9341_2 panels ship with a gamma
     * curve that crushes smooth gradients into visible bands — solid colours
     * and text look fine, but anti-aliased greys (e.g. the logo edges) come
     * out blocky. Re-select a built-in gamma curve via GAMMASET (0x26) to get
     * a clean ramp. See TFT_eSPI discussion #3018.
     */
    tft.writecommand(0x26);      /* GAMMASET */
    tft.writedata(0x02);
    delay(120);
    tft.writecommand(0x26);
    tft.writedata(0x01);

    tft.fillScreen(TFT_BLACK);

    touchSPI.begin(T_CLK, T_MISO, T_MOSI, T_CS);
    touch.begin(touchSPI);
    touch.setRotation(0);        /* match the panel orientation */

    /* Take over the backlight pin with LEDC PWM (after tft.init has touched
     * it) so brightness is dimmable from the settings menu. Restore the saved
     * level from NVS (defaults to 80% if never set). */
    s_brightness = settings_get_brightness();
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
    /* LVGL rendering + nested event callbacks (tabview/modal) + NVS calls
     * are stack-heavy; give the task plenty of headroom. */
    xTaskCreate(ui_task, "ui", 16384, NULL, 4, NULL);
}

extern "C" void ui_show_splash(void) {
    request_screen(UI_SCREEN_SPLASH);
}

extern "C" void ui_show_amount_entry(void) {
    s_amount_cents = 0U;      /* fresh entry each time */
    s_amount_units = 0U;
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

extern "C" size_t ui_take_pin(char *out, size_t n) {
    if ((out == NULL) || (n == 0U)) { return 0U; }
    size_t len = s_pin_len;
    if (len > (n - 1U)) { len = n - 1U; }
    (void)CW_Utils::safe_memcpy(reinterpret_cast<uint8_t *>(out), n,
                                reinterpret_cast<const uint8_t *>(s_pin), len);
    out[len] = '\0';
    /* The UI no longer needs the PIN — wipe its copy. */
    CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(s_pin), sizeof(s_pin));
    s_pin_len = 0;
    return len;
}

extern "C" void ui_show_wifi_list(const net_wifi_ap_t *aps, uint16_t n) {
    s_ap_count = (n > WIFI_MAX_APS) ? WIFI_MAX_APS : n;
    for (uint16_t i = 0U; i < s_ap_count; i++) {
        s_aps[i] = aps[i];
    }
    request_screen(UI_SCREEN_WIFI_LIST);
}

extern "C" void ui_set_addresses(const char *usdc_contract, const char *dest_addr) {
    s_addr_usdc = usdc_contract;
    s_addr_dest = dest_addr;
}

extern "C" void ui_show_wifi_connecting(const char *ssid) {
    snprintf(s_wifi_msg, sizeof(s_wifi_msg), "Connecting to %s...",
             (ssid != NULL) ? ssid : "");
    request_screen(UI_SCREEN_WIFI_CONNECTING);
}

extern "C" void ui_show_welcome(void) {
    s_welcome_sent = false;
    request_screen(UI_SCREEN_WELCOME);
}

extern "C" void ui_show_admin_set(void) {
    s_admin_confirming = false;
    s_admin_note[0]    = '\0';
    CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(s_admin_first),
                          sizeof(s_admin_first));
    request_screen(UI_SCREEN_ADMIN_SET);
}

extern "C" size_t ui_take_wifi_creds(char *ssid, size_t ssid_n,
                                     char *pass, size_t pass_n) {
    if ((ssid == NULL) || (pass == NULL) || (ssid_n == 0U) || (pass_n == 0U)) {
        return 0U;
    }
    strncpy(ssid, s_wifi_ssid, ssid_n - 1U);
    ssid[ssid_n - 1U] = '\0';
    strncpy(pass, s_wifi_pass, pass_n - 1U);
    pass[pass_n - 1U] = '\0';
    /* Wipe the UI's copy of the passphrase. */
    CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(s_wifi_pass), sizeof(s_wifi_pass));
    return strlen(ssid);
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
