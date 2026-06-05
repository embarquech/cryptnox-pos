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
#include "settings.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_system.h"   /* esp_restart() for factory reset */
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"

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
 * 3b. Layout metrics — shared so every screen's header lines up
 ******************************************************************/
#define HDR_TITLE_Y     12     /* eyebrow title offset from the top   */
#define HDR_DIVIDER_Y   42     /* rule under the title                */
#define CONTENT_Y       58     /* first content row, below the rule   */
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

/* Brightness state (here rather than §5 so the LDR helper below can see it). */
static uint8_t s_brightness     = 80;     /* manual backlight %, restored from NVS */
static bool    s_auto_brightness = false; /* follow the ambient light sensor        */

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

/* ── Ambient light sensor (LDR) on GPIO 34 = ADC1 channel 6 ── */
#define LDR_ADC_UNIT     ADC_UNIT_1
#define LDR_ADC_CHANNEL  ADC_CHANNEL_6
#define LDR_RAW_MIN      150     /* ~dark room   — calibrate on hardware */
#define LDR_RAW_MAX      3200    /* ~bright room — calibrate on hardware */
#define LDR_PCT_MIN      15      /* never go fully dark in auto mode     */
#define LDR_PCT_MAX      100

static adc_oneshot_unit_handle_t s_adc = NULL;

static void ldr_init(void) {
    adc_oneshot_unit_init_cfg_t u = {};
    u.unit_id  = LDR_ADC_UNIT;
    u.ulp_mode = ADC_ULP_MODE_DISABLE;
    if (adc_oneshot_new_unit(&u, &s_adc) != ESP_OK) { s_adc = NULL; return; }

    adc_oneshot_chan_cfg_t c = {};
    c.atten    = ADC_ATTEN_DB_12;
    c.bitwidth = ADC_BITWIDTH_DEFAULT;
    (void)adc_oneshot_config_channel(s_adc, LDR_ADC_CHANNEL, &c);
}

/* Map the ambient-light reading to a backlight %. Brighter room -> brighter
 * screen; if it's backwards on your unit, swap LDR_PCT_MIN/MAX. */
static uint8_t ldr_brightness_pct(void) {
    int raw = 0;
    if ((s_adc == NULL) ||
        (adc_oneshot_read(s_adc, LDR_ADC_CHANNEL, &raw) != ESP_OK)) {
        return s_brightness;   /* fall back to the manual level */
    }
    if (raw < LDR_RAW_MIN) { raw = LDR_RAW_MIN; }
    if (raw > LDR_RAW_MAX) { raw = LDR_RAW_MAX; }
    int pct = LDR_PCT_MIN + (raw - LDR_RAW_MIN) * (LDR_PCT_MAX - LDR_PCT_MIN)
                              / (LDR_RAW_MAX - LDR_RAW_MIN);
    return static_cast<uint8_t>(pct);
}

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
static char      s_amount_str[12] = {0};

/* PIN entry — the textarea (password mode) is the live input; s_pin is the
 * handoff buffer read by main via ui_take_pin() and wiped on read. */
static lv_obj_t *s_pin_ta      = NULL;
static char      s_pin[16]     = {0};
static uint8_t   s_pin_len     = 0;

/* Wi-Fi picker */
#define WIFI_MAX_APS 16
static eth_wifi_ap_t s_aps[WIFI_MAX_APS];
static uint16_t      s_ap_count = 0;
static char          s_wifi_ssid[33] = {0};   /* selected network          */
static char          s_wifi_pass[65] = {0};   /* entered passphrase (handoff) */
static char          s_wifi_msg[64]  = {0};   /* "Scanning…" / "Connecting to <ssid>…" */
static lv_obj_t     *s_wifi_pass_ta  = NULL;

/* Destination info shown on the settings "Tx" tab (set by main, static). */
static const char *s_addr_usdc = NULL;
static const char *s_addr_dest = NULL;

/******************************************************************
 * 6. Button actions
 ******************************************************************/
enum BtnAction {
    ACT_CONFIRM, ACT_CANCEL, ACT_SEND, ACT_NEW,
    ACT_SETTINGS, ACT_CLOSE, ACT_PIN_CANCEL,
    ACT_WIFI, ACT_WIFI_CANCEL, ACT_RESET, ACT_RESET_CONFIRM, ACT_RESET_CANCEL,
};

/* Settings modal — defined in section 7 (uses the widget helpers). */
static void open_settings(void);
static void close_settings(void);
static void open_reset_confirm(void);
static void close_reset_confirm(void);

static void request_screen(ui_screen_t s) {
    s_req_screen   = s;
    s_screen_dirty = true;
}

static void format_amount(uint64_t units, char *out, size_t n) {
    uint64_t whole = units / 1000000ULL;
    uint64_t cents = (units % 1000000ULL) / 10000ULL;
    snprintf(out, n, "%" PRIu64 ".%02" PRIu64, whole, cents);
}

/* Parse the typed string (e.g. "12.5") into USDC base units (6 decimals,
 * 2 entered), clamped to the 99999 cap. */
static uint64_t amount_str_to_units(const char *s) {
    uint64_t whole = 0U;
    uint64_t frac  = 0U;
    int      fdig  = 0;
    bool     dot   = false;
    for (const char *p = s; *p != '\0'; p++) {
        if (*p == '.') { dot = true; continue; }
        if ((*p < '0') || (*p > '9')) { continue; }
        if (!dot) {
            whole = (whole * 10U) + static_cast<uint64_t>(*p - '0');
            if (whole > 99999U) { whole = 99999U; }
        } else if (fdig < 2) {
            frac = (frac * 10U) + static_cast<uint64_t>(*p - '0');
            fdig++;
        }
    }
    if (fdig == 1) { frac *= 10U; }   /* "5.2" -> 20 cents */
    uint64_t units = (whole * 1000000ULL) + (frac * 10000ULL);
    if (units > 99999000000ULL) { units = 99999000000ULL; }
    return units;
}

static void amount_update_display(void) {
    if (s_amount_label != NULL) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%s USDC",
                 (s_amount_str[0] != '\0') ? s_amount_str : "0");
        lv_label_set_text(s_amount_label, buf);
    }
    s_amount_units = amount_str_to_units(s_amount_str);
}

/* Numeric keypad on the amount screen: digits, one '.', backspace. */
static void amount_kbd_cb(lv_event_t *e) {
    lv_obj_t *bm = lv_event_get_target(e);
    uint32_t id = lv_btnmatrix_get_selected_btn(bm);
    const char *txt = lv_btnmatrix_get_btn_text(bm, id);
    if (txt == NULL) { return; }

    size_t len = strlen(s_amount_str);
    if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
        if (len > 0U) { s_amount_str[len - 1U] = '\0'; }
    } else if (strcmp(txt, ".") == 0) {
        if ((len > 0U) && (len < sizeof(s_amount_str) - 1U) &&
            (strchr(s_amount_str, '.') == NULL)) {
            s_amount_str[len]      = '.';
            s_amount_str[len + 1U] = '\0';
        }
    } else if ((txt[0] >= '0') && (txt[0] <= '9') && (txt[1] == '\0')) {
        const char *dot = strchr(s_amount_str, '.');
        if (dot != NULL) {
            if (strlen(dot + 1) >= 2U) { return; }   /* max 2 decimals */
        } else if (len >= 5U) {
            return;                                   /* max 99999 whole */
        }
        if (len < sizeof(s_amount_str) - 1U) {
            s_amount_str[len]      = txt[0];
            s_amount_str[len + 1U] = '\0';
        }
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
            open_settings();
            break;
        case ACT_CLOSE:
            close_settings();
            break;
        case ACT_WIFI:
            close_settings();
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
    s_pin_ta       = NULL;   /* deleted by lv_obj_clean — drop the dangling ref */
    s_wifi_pass_ta = NULL;
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

/* Toggle automatic (light-sensor) brightness; disables the manual slider.
 * No flash write here — persistence happens once on close so rapid toggling
 * stays responsive (an NVS commit would stall the UI task for ~tens of ms). */
static void auto_brightness_cb(lv_event_t *e) {
    lv_obj_t *cb = lv_event_get_target(e);
    lv_obj_t *sl = static_cast<lv_obj_t *>(lv_event_get_user_data(e));
    s_auto_brightness = lv_obj_has_state(cb, LV_STATE_CHECKED);
    if (s_auto_brightness) {
        if (sl != NULL) { lv_obj_add_state(sl, LV_STATE_DISABLED); }
    } else {
        if (sl != NULL) { lv_obj_clear_state(sl, LV_STATE_DISABLED); }
        backlight_set_pct(s_brightness);   /* back to the manual level */
    }
}

static void backdrop_event_cb(lv_event_t *e) {
    (void)e;
    close_settings();   /* tap outside the card dismisses */
}

static void close_settings(void) {
    if (s_settings != NULL) {
        /* Persist both settings once, on close — keeps toggling/dragging snappy. */
        settings_set_brightness(s_brightness);
        settings_set_auto_brightness(s_auto_brightness);
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
    lv_obj_set_size(card, 228, 290);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, COL_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 14, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    /* Absorb taps on the card so they don't reach the backdrop and dismiss it. */
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    /* Tabbed layout: Screen / Wi-Fi / About. */
    lv_obj_t *tv = lv_tabview_create(card, LV_DIR_TOP, 34);
    lv_obj_set_size(tv, 228, 244);
    lv_obj_align(tv, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(tv, COL_SURFACE, LV_PART_MAIN);

    lv_obj_t *tabbar = lv_tabview_get_tab_btns(tv);
    lv_obj_set_style_bg_color(tabbar, COL_BG, LV_PART_MAIN);
    lv_obj_set_style_text_color(tabbar, COL_DIM, LV_PART_ITEMS);
    lv_obj_set_style_text_color(tabbar, COL_ACCENT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(tabbar, COL_ACCENT, LV_PART_ITEMS | LV_STATE_CHECKED);

    lv_obj_t *t_screen = lv_tabview_add_tab(tv, "Screen");
    lv_obj_t *t_wifi   = lv_tabview_add_tab(tv, "Wi-Fi");
    lv_obj_t *t_tx     = lv_tabview_add_tab(tv, "Tx");
    lv_obj_t *t_about  = lv_tabview_add_tab(tv, "About");
    lv_obj_t *pages[4] = { t_screen, t_wifi, t_tx, t_about };
    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_bg_color(pages[i], COL_SURFACE, LV_PART_MAIN);
        lv_obj_set_style_pad_all(pages[i], 8, LV_PART_MAIN);
        lv_obj_clear_flag(pages[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(pages[i], LV_OBJ_FLAG_CLICKABLE);
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

    lv_obj_t *chk = lv_checkbox_create(t_screen);
    lv_checkbox_set_text(chk, "Auto (light sensor)");
    lv_obj_align(chk, LV_ALIGN_TOP_LEFT, 0, 70);
    lv_obj_set_style_text_color(chk, COL_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(chk, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(chk, COL_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (s_auto_brightness) {
        lv_obj_add_state(chk, LV_STATE_CHECKED);
        lv_obj_add_state(sl, LV_STATE_DISABLED);
    }
    lv_obj_add_event_cb(chk, auto_brightness_cb, LV_EVENT_VALUE_CHANGED, sl);

    /* ── Wi-Fi tab ── */
    make_label(t_wifi, "Connect to a Wi-Fi network", COL_DIM,
               &lv_font_montserrat_14, LV_ALIGN_TOP_MID, 0, 4);
    make_button(t_wifi, LV_SYMBOL_WIFI " Scan networks", COL_ACCENT, COL_BG,
                220, 44, LV_ALIGN_CENTER, 0, 6, ACT_WIFI, &lv_font_montserrat_20);

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

    /* ── About tab ── */
    make_label(t_about, "cryptnox-pos", COL_TEXT, &lv_font_montserrat_20,
               LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_t *about = make_label(t_about,
                                 "USDC payment terminal for Cryptnox cards\n\n"
                                 "Based on cryptnox-sdk-esp32 1.0.0\n"
                                 "(c) Cryptnox 2026\n"
                                 "Educational use only",
                                 COL_DIM, &lv_font_montserrat_14,
                                 LV_ALIGN_TOP_MID, 0, 28);
    lv_label_set_long_mode(about, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(about, 200);
    lv_obj_set_style_text_align(about, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    /* Factory reset lives here, in About. */
    make_button(t_about, "Reset", COL_DANGER, COL_TEXT, 150, 40,
                LV_ALIGN_BOTTOM_MID, 0, 0, ACT_RESET, &lv_font_montserrat_20);

    /* Close is shared across tabs; Reset lives in the About tab only. */
    make_button(card, "Close", COL_ACCENT, COL_BG, 140, 38,
                LV_ALIGN_BOTTOM_MID, 0, -6, ACT_CLOSE, &lv_font_montserrat_20);
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
    lv_obj_set_size(card, 224, 168);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, COL_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 14, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, COL_BORDER, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *msg = make_label(card, "Erase all settings\n(Wi-Fi, brightness)\nand reboot?",
                               COL_TEXT, &lv_font_montserrat_14,
                               LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    make_button(card, "Cancel", COL_SURFACE, COL_TEXT, 96, 40,
                LV_ALIGN_BOTTOM_LEFT, 10, -10, ACT_RESET_CANCEL, &lv_font_montserrat_20);
    make_button(card, "Erase", COL_DANGER, COL_TEXT, 96, 40,
                LV_ALIGN_BOTTOM_RIGHT, -10, -10, ACT_RESET_CONFIRM, &lv_font_montserrat_20);
}

/******************************************************************
 * 8. Screen builders
 ******************************************************************/
/* Small burger button (top-left) that opens the settings modal. */
static void add_menu_button(void) {
    make_button(lv_scr_act(), LV_SYMBOL_LIST, COL_SURFACE, COL_TEXT,
                MENU_BTN_W, MENU_BTN_H, LV_ALIGN_TOP_LEFT, MENU_BTN_X, MENU_BTN_Y,
                ACT_SETTINGS, &lv_font_montserrat_20);
}

/* Identical header on every screen: eyebrow title + rule + burger menu. */
static void build_header(const char *title) {
    make_label(lv_scr_act(), title, COL_DIM, &lv_font_montserrat_14,
               LV_ALIGN_TOP_MID, 0, HDR_TITLE_Y);
    make_divider(lv_scr_act(), HDR_DIVIDER_Y);
    add_menu_button();
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
    /* No title/divider here — the keypad needs the vertical space. Keep just
     * the burger (settings) top-left. */
    add_menu_button();

    char buf[24];
    snprintf(buf, sizeof(buf), "%s USDC",
             (s_amount_str[0] != '\0') ? s_amount_str : "0");
    s_amount_label = make_label(lv_scr_act(), buf, COL_TEXT,
                                &lv_font_montserrat_28, LV_ALIGN_TOP_MID, 0, 8);

    /* Numeric keypad: digits, decimal point, backspace. */
    static const char *amap[] = {
        "1", "2", "3", "\n",
        "4", "5", "6", "\n",
        "7", "8", "9", "\n",
        ".", "0", LV_SYMBOL_BACKSPACE, ""
    };
    lv_obj_t *kb = lv_btnmatrix_create(lv_scr_act());
    lv_btnmatrix_set_map(kb, amap);
    lv_obj_set_size(kb, 232, 200);
    lv_obj_align(kb, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_bg_opa(kb, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(kb, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(kb, COL_SURFACE, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, COL_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_20, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 8, LV_PART_ITEMS);
    lv_obj_add_event_cb(kb, amount_kbd_cb, LV_EVENT_VALUE_CHANGED, NULL);

    make_button(lv_scr_act(), "Charge", COL_ACCENT, COL_BG, 232, ACT_BTN_H,
                LV_ALIGN_BOTTOM_MID, 0, ACT_BTN_Y, ACT_CONFIRM, &lv_font_montserrat_20);

    amount_update_display();   /* keep s_amount_units in sync with the string */
}

static void build_confirm(void) {
    clear_screen();
    build_header("Confirm");

    char buf[40];
    format_amount(s_confirm_amount, buf, sizeof(buf));
    strncat(buf, " USDC", sizeof(buf) - strlen(buf) - 1);
    make_label(lv_scr_act(), buf, COL_TEXT, &lv_font_montserrat_28,
               LV_ALIGN_TOP_MID, 0, CONTENT_Y);

    make_label(lv_scr_act(), "To", COL_DIM, &lv_font_montserrat_14,
               LV_ALIGN_TOP_MID, 0, CONTENT_Y + 34);
    lv_obj_t *addr = make_label(lv_scr_act(),
                                s_confirm_addr[0] ? s_confirm_addr : "-",
                                COL_TEXT, &lv_font_montserrat_14,
                                LV_ALIGN_TOP_MID, 0, CONTENT_Y + 54);
    lv_label_set_long_mode(addr, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(addr, 216);
    lv_obj_set_style_text_align(addr, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    make_button(lv_scr_act(), "Cancel", COL_SURFACE, COL_TEXT, 104, ACT_BTN_H,
                LV_ALIGN_BOTTOM_LEFT, 10, ACT_BTN_Y, ACT_CANCEL, &lv_font_montserrat_20);
    make_button(lv_scr_act(), "Send", COL_ACCENT, COL_BG, 104, ACT_BTN_H,
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

static void build_pin(void) {
    clear_screen();
    /* Wipe any stale PIN from a previous attempt. */
    CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(s_pin), sizeof(s_pin));
    s_pin_len = 0;

    make_label(lv_scr_act(), "Enter PIN", COL_DIM, &lv_font_montserrat_14,
               LV_ALIGN_TOP_MID, 0, HDR_TITLE_Y);
    /* Back (cancel) button, top-left like the burger on other screens. */
    make_button(lv_scr_act(), LV_SYMBOL_LEFT, COL_SURFACE, COL_TEXT,
                MENU_BTN_W, MENU_BTN_H, LV_ALIGN_TOP_LEFT, MENU_BTN_X, MENU_BTN_Y,
                ACT_PIN_CANCEL, &lv_font_montserrat_20);

    /* Masked input field (password mode renders bullets). */
    s_pin_ta = lv_textarea_create(lv_scr_act());
    lv_textarea_set_password_mode(s_pin_ta, true);
    lv_textarea_set_one_line(s_pin_ta, true);
    lv_textarea_set_max_length(s_pin_ta, 9);
    lv_textarea_set_text(s_pin_ta, "");
    lv_obj_clear_flag(s_pin_ta, LV_OBJ_FLAG_CLICKABLE);   /* no soft-keyboard popup */
    lv_obj_set_width(s_pin_ta, 160);
    lv_obj_align(s_pin_ta, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_set_style_text_align(s_pin_ta, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_pin_ta, COL_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_pin_ta, COL_TEXT, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_pin_ta, COL_BORDER, LV_PART_MAIN);

    /* Numeric keypad. The map is static — lv_btnmatrix keeps the pointer. */
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
    lv_obj_set_style_bg_color(kb, COL_SURFACE, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, COL_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_20, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 8, LV_PART_ITEMS);
    lv_obj_add_event_cb(kb, pin_kbd_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

/* Header with a back arrow (to amount entry) instead of the burger. */
static void build_header_back(const char *title) {
    make_label(lv_scr_act(), title, COL_DIM, &lv_font_montserrat_14,
               LV_ALIGN_TOP_MID, 0, HDR_TITLE_Y);
    make_divider(lv_scr_act(), HDR_DIVIDER_Y);
    make_button(lv_scr_act(), LV_SYMBOL_LEFT, COL_SURFACE, COL_TEXT,
                MENU_BTN_W, MENU_BTN_H, LV_ALIGN_TOP_LEFT, MENU_BTN_X, MENU_BTN_Y,
                ACT_WIFI_CANCEL, &lv_font_montserrat_20);
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
    lv_obj_set_width(s_wifi_pass_ta, SCR_W - 24);
    lv_obj_align(s_wifi_pass_ta, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_bg_color(s_wifi_pass_ta, COL_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_wifi_pass_ta, COL_TEXT, LV_PART_MAIN);

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

static void build_tx_status(void) {
    clear_screen();
    build_header("Transaction");

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
               LV_ALIGN_TOP_MID, 0, CONTENT_Y);

    lv_obj_t *info = make_label(lv_scr_act(), s_tx_info, COL_DIM,
                                &lv_font_montserrat_14, LV_ALIGN_CENTER, 0, 10);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(info, 216);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    if (show_new) {
        make_button(lv_scr_act(), "New payment", COL_ACCENT, COL_BG, 180, ACT_BTN_H,
                    LV_ALIGN_BOTTOM_MID, 0, ACT_BTN_Y, ACT_NEW, &lv_font_montserrat_20);
    } else if (show_cancel) {
        make_button(lv_scr_act(), "Cancel", COL_SURFACE, COL_TEXT, 160, ACT_BTN_H,
                    LV_ALIGN_BOTTOM_MID, 0, ACT_BTN_Y, ACT_CANCEL, &lv_font_montserrat_20);
    }
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
    tft.setRotation(0);          /* portrait, 240x320 */
    tft.fillScreen(TFT_BLACK);

    touchSPI.begin(T_CLK, T_MISO, T_MOSI, T_CS);
    touch.begin(touchSPI);
    touch.setRotation(0);        /* match the panel orientation */

    /* Take over the backlight pin with LEDC PWM (after tft.init has touched
     * it) so brightness is dimmable from the settings menu. Restore the saved
     * level from NVS (defaults to 80% if never set). */
    s_brightness      = settings_get_brightness();
    s_auto_brightness = settings_get_auto_brightness();
    backlight_init(s_brightness);
    ldr_init();

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

    uint32_t auto_tick = 0;
    while (true) {
        if (s_screen_dirty) {
            s_screen_dirty = false;
            render_requested_screen();
        }
        lv_timer_handler();

        /* Auto-brightness: sample the LDR ~twice a second and track it. */
        if (s_auto_brightness && (++auto_tick >= 100U)) {
            auto_tick = 0;
            backlight_set_pct(ldr_brightness_pct());
        }

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
    /* LVGL rendering + nested event callbacks (tabview/modal) + NVS/ADC calls
     * are stack-heavy; give the task plenty of headroom. */
    xTaskCreate(ui_task, "ui", 16384, NULL, 4, NULL);
}

extern "C" void ui_show_splash(void) {
    request_screen(UI_SCREEN_SPLASH);
}

extern "C" void ui_show_amount_entry(void) {
    s_amount_str[0] = '\0';   /* fresh entry each time */
    s_amount_units  = 0U;
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

extern "C" void ui_show_wifi_list(const eth_wifi_ap_t *aps, uint16_t n) {
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
