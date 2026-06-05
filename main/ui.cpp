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

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

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
 * 3. Dark-fintech theme
 ******************************************************************/
#define COL_BG       lv_color_hex(0x0E1116)   /* slate near-black     */
#define COL_SURFACE  lv_color_hex(0x1A1F27)   /* cards / steppers     */
#define COL_TEXT     lv_color_hex(0xFFFFFF)
#define COL_DIM      lv_color_hex(0x8A93A0)
#define COL_ACCENT   lv_color_hex(0x00D68F)   /* mint — primary action */
#define COL_DANGER   lv_color_hex(0xFF5A5F)
#define COL_BORDER   lv_color_hex(0x2A313C)

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

/******************************************************************
 * 6. Button actions
 ******************************************************************/
enum BtnAction {
    ACT_MINUS_U, ACT_PLUS_U, ACT_MINUS_C, ACT_PLUS_C,
    ACT_CONFIRM, ACT_CANCEL, ACT_SEND, ACT_NEW,
};

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
    lv_obj_set_style_bg_color(btn, bg, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
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

static void clear_screen(void) {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, COL_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    s_amount_label = NULL;
}

/******************************************************************
 * 8. Screen builders
 ******************************************************************/
static void build_splash(void) {
    clear_screen();
    make_label(lv_scr_act(), "CRYPTNOX", COL_TEXT, &lv_font_montserrat_28,
               LV_ALIGN_CENTER, 0, -16);
    make_label(lv_scr_act(), "Payment terminal", COL_DIM, &lv_font_montserrat_14,
               LV_ALIGN_CENTER, 0, 20);
}

static void build_amount(void) {
    clear_screen();
    make_label(lv_scr_act(), "Amount", COL_DIM, &lv_font_montserrat_14,
               LV_ALIGN_TOP_MID, 0, 14);

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

    char buf[40];
    format_amount(s_confirm_amount, buf, sizeof(buf));
    strncat(buf, " USDC", sizeof(buf) - strlen(buf) - 1);
    make_label(lv_scr_act(), buf, COL_TEXT, &lv_font_montserrat_28,
               LV_ALIGN_TOP_MID, 0, 38);

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

    const char *state_str = "";
    lv_color_t  color      = COL_TEXT;
    bool        show_new   = false;
    bool        show_cancel = false;
    switch (s_tx_state) {
        case UI_TX_STATE_PLACE_CARD: state_str = "Place card";      show_cancel = true; break;
        case UI_TX_STATE_SIGNING:    state_str = "Signing...";      break;
        case UI_TX_STATE_SENDING:    state_str = "Broadcasting..."; break;
        case UI_TX_STATE_DONE:       state_str = "Sent"; color = COL_ACCENT; show_new = true; break;
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
