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
#include "chain_icons.h"
#include "settings.h"
#include "provision.h"   /* QR payload + the pending payout-address handshake */
#include "ota.h"         /* running version + the update window and its handshake */

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

/* Splash logo X trim, tuned by eye on the panel: the bitmap is geometrically
 * centred, but 0 reads as too far right. A calibration value — do not "correct"
 * it from the image geometry. */
#define LOGO_X_NUDGE (-4)

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
#define COL_TRON     lv_color_hex(0xE7392E)   /* Tron red — TRX asset badge    */
#define COL_USDT     lv_color_hex(0x26A17B)   /* Tether green — USDT badge     */
#define COL_ETH      lv_color_hex(0x627EEA)   /* Ethereum periwinkle — net chip*/

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
/* Asset selector — on the amount's own row, hard right. Centred above the figure
 * it read as a heading for it; beside it, it reads as the currency of the figure,
 * which is what somebody about to charge 12.50 is looking for.
 *
 * Sized off the 36px badge it carries: 8 pad + 36 badge + 12 gap + 10 arrow + 10
 * pad = 76 wide, and 3px of air above and below the badge = 42 tall. (BADGE_SZ is
 * defined with the icon helpers, further down — kept numeric here so the metrics
 * read as one block.) The narrow width is the same minus the chevron and its gap:
 * once digits are on screen they get the room, because "this opens something" is
 * worth less than the amount being legible. */
#define ASSET_BTN_W     76
#define ASSET_BTN_W_SM  52     /* 8 pad + 36 badge + 8 pad, no chevron */
#define ASSET_BTN_H     42
#define ASSET_BTN_X     (-6)   /* right edge inset                     */
#define ASSET_BTN_Y     47     /* the figure's row is centred on THIS  */
/* The figure's row shares the selector's line, clear of the keypad at 90 — but
 * neither of its offsets is a constant any more, because both depend on what has
 * been typed. See amount_row_place(). */
#define ASSET_BTN_PAD   8      /* badge inset from the left edge       */
#define TAB_PAD         12     /* settings tab page padding           */
#define TAB_W           (SCR_W - (2 * TAB_PAD))   /* usable tab width */

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

/* Amount entry — keypad input string (e.g. "12.50") and its display label. The
 * cents are their own label so they can be set in a smaller font past 100; below
 * that they stay in the main label and this one holds "". */
static lv_obj_t *s_amount_label = NULL;
static lv_obj_t *s_amount_cents_label = NULL;
/* The ticker beside the figure, empty while the figure is 0.00 — "0.00 USDC" is
 * not a price, it is a placeholder wearing a unit. */
static lv_obj_t *s_amount_ticker_label = NULL;
/* The flex row the three of them sit in — kept because its placement is not
 * fixed: see amount_row_place(). */
static lv_obj_t *s_amount_row = NULL;
static uint64_t  s_amount_cents = 0;      /* amount entered, in cents          */

/* The asset selector on the amount row, and the chevron it drops when it has to
 * make room. Both die with the screen — cleared in clear_screen(). */
static lv_obj_t *s_asset_btn   = NULL;
static lv_obj_t *s_asset_arrow = NULL;

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
static char          s_wifi_note[64] = {0};   /* why the picker reopened (may be empty) */
static lv_obj_t     *s_wifi_pass_ta  = NULL;
static lv_obj_t     *s_wifi_eye_lbl  = NULL;   /* glyph swapped on reveal/hide */

/* Progress screen (UI_SCREEN_WIFI_CONNECTING), two pieces rather than one
 * preformatted line: the name is stored raw so the label elides it by real
 * glyph width, which no character budget can do for every SSID. */
static char          s_wifi_caption[24] = {0};  /* "Scanning..." / "Connecting to" */
static char          s_wifi_name[33]    = {0};  /* network name; empty for none    */

/* Splash progress line. Written by the main task, applied by the UI task
 * (LVGL is not thread-safe) — same hand-off shape as request_screen(). */
static char          s_boot_step[40]     = {0};
static lv_obj_t     *s_boot_step_lbl     = NULL;
static volatile bool s_boot_step_dirty   = false;

/* Phone setup (UI_SCREEN_PROV). The step is an int, not a prov_step_t, for the
 * same reason ui_show_prov takes one — provision.h includes ui.h. Both are
 * written by the main task and read by the UI task, like the splash line above. */
static volatile int  s_prov_step         = 0;
static volatile bool s_addr_modal_dirty  = false;
/* Firmware uploaded from a browser and waiting to be accepted here. Same
 * handoff as the address modal above: the HTTP task never touches LVGL. */
static volatile bool s_ota_modal_dirty   = false;

/* Startup fault (UI_SCREEN_BOOT_ERROR). */
static ui_boot_err_t s_boot_err            = UI_BOOT_ERR_NFC;
static char          s_boot_detail[64]     = {0};

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
static char          s_welcome_sub[48]  = {0};     /* line under the brand */
/* Penalty clock, monotonic since boot (lv_tick_elaps handles the wrap). The
 * attempt count itself lives in NVS, so power-cycling shortens the current wait
 * but never resets the escalation. */
static uint32_t      s_admin_lock_start = 0;
static uint32_t      s_admin_lock_ms    = 0;
/* What a correct code on the unlock screen is for. The escalating penalty, the
 * note band and the keypad are identical either way — only the thing the code
 * opens differs, so one screen serves both rather than a near-copy that could
 * drift on the lockout. */
static bool          s_admin_for_portal = false;

/* Whether the PIN keypad is collecting a card PIN for a *read* (deriving a payout
 * address) rather than for a payment. Same reason as above: the card refuses to
 * export a public key without a verified PIN, so the screen is the same one. */
static bool          s_pin_for_card     = false;

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
    /* Config portal: accept/reject a proposed value, finish the wizard. */
    ACT_PROV_OK, ACT_PROV_NO, ACT_PROV_FINISH,
    ACT_RESET, ACT_RESET_CONFIRM, ACT_MODAL_CLOSE,
    /* Asset selector (amount screen and the Tx tab): network, then the coin. */
    ACT_NET_PICK, ACT_NET_ETH, ACT_NET_POLY, ACT_NET_TRON,
    /* The admin web page: open it (QR to scan), close it, resolve an upload. */
    ACT_PORTAL, ACT_PORTAL_CLOSE, ACT_OTA_OK, ACT_OTA_NO,
    /* One action per chain, contiguous, so an action's offset from here IS its
     * pos_chain_t (ACT_CHAIN_OF below, decoded in btn_event_cb). Seven assets
     * would otherwise be seven enumerators and a seven-armed switch that says
     * nothing the picker's own table does not already say. KEEP LAST: the block
     * runs to POS_CHAIN__COUNT and nothing may share its numbers. */
    ACT_CHAIN_BASE,
};

/** The action that selects @p chain. */
#define ACT_CHAIN_OF(chain) \
    static_cast<BtnAction>(ACT_CHAIN_BASE + static_cast<int>(chain))

/* Settings — defined in section 7 (uses the widget helpers). */
static void settings_persist(void);
static void open_reset_confirm(void);
/* The three networks the picker offers. Not pos_chain_t: step 1 of the picker is
 * a network, and several chains share one (USDC and USDT are both Ethereum). */
typedef enum { UI_NET_ETH = 0, UI_NET_POLY, UI_NET_TRON } ui_net_t;

static void open_network_picker(void);
static void open_coin_picker(ui_net_t net);
static void open_portal_window(void);
static void open_ota_gone(void);
static void close_modal(void);
static void pop_in(lv_obj_t *obj);   /* defined in section 8 (animations) */
/* Defined with the icon helpers; called from amount_update_display() above them. */
static void asset_btn_set_compact(bool compact);
static uint32_t admin_penalty_ms(uint8_t fails);   /* defined with the admin screens */
static ui_screen_t s_settings_return = UI_SCREEN_AMOUNT;   /* screen to go back to */
/* Which tab a (re)built settings page opens on. Zeroed when the burger opens the
 * page, kept when something inside it forces a rebuild — picking an asset on the
 * Tx tab has to come back to the Tx tab, not to Screen. */
static uint16_t s_settings_tab = 0U;

#define SETTINGS_TAB_COUNT  4
#define TAB_ABOUT           3

/* The chain is read straight from NVS wherever it is needed — the asset badge,
 * the picker pill and main's signing path all ask the same question, so there is
 * no UI-side copy to keep in sync. */
static bool chain_is_tron(void) {
    return pos_chain_is_tron(settings_get_chain());
}

/** Ticker of the asset being charged, for the selector and the amount screens. */
static const char *asset_name(void) {
    switch (settings_get_chain()) {
        case POS_CHAIN_TRON_NILE:  return "TRX";
        case POS_CHAIN_ETH_NATIVE: return "ETH";
        case POS_CHAIN_POLY_NATIVE:return "POL";
        case POS_CHAIN_TRON_USDT:
        case POS_CHAIN_ETH_USDT:
        case POS_CHAIN_POLY_USDT:  return "USDT";
        default:                   return "USDC";
    }
}

/** Which network that asset lives on — the selector's subtitle. */
static const char *asset_network(void) {
    if (chain_is_tron()) { return "Tron Nile"; }
    return pos_chain_is_polygon(settings_get_chain()) ? "Polygon Amoy"
                                                      : "Ethereum Sepolia";
}

/** Caption for the address row above "Send to": TRX has no contract to show. */
static const char *asset_caption(void) {
    switch (settings_get_chain()) {
        /* A network's own coin has no contract to show, so the row names the
         * asset instead — the same thing TRX has always done. */
        case POS_CHAIN_TRON_NILE:
        case POS_CHAIN_ETH_NATIVE:
        case POS_CHAIN_POLY_NATIVE: return "Asset";
        case POS_CHAIN_TRON_USDT:
        case POS_CHAIN_TRON_USDC:   return "Token contract";
        case POS_CHAIN_ETH_USDT:
        case POS_CHAIN_POLY_USDT:   return "USDT contract";
        default:                    return "USDC contract";
    }
}

static void request_screen(ui_screen_t s) {
    s_req_screen   = s;
    s_screen_dirty = true;
}

/* Sets both pieces at once, so no caller can leave a stale name behind. */
static void set_wifi_progress(const char *caption, const char *name) {
    strncpy(s_wifi_caption, (caption != NULL) ? caption : "",
            sizeof(s_wifi_caption) - 1);
    s_wifi_caption[sizeof(s_wifi_caption) - 1] = '\0';
    strncpy(s_wifi_name, (name != NULL) ? name : "", sizeof(s_wifi_name) - 1);
    s_wifi_name[sizeof(s_wifi_name) - 1] = '\0';
}

static void format_amount(uint64_t units, char *out, size_t n) {
    uint64_t whole = units / 1000000ULL;
    uint64_t cents = (units % 1000000ULL) / 10000ULL;
    snprintf(out, n, "%" PRIu64 ".%02" PRIu64, whole, cents);
}

#define AMOUNT_CENTS_MAX  9999999ULL   /* 99999.99 */
/* 18.44 — POS_AMOUNT_UNITS_MAX_NATIVE expressed in the keypad's cents. */
#define AMOUNT_CENTS_MAX_NATIVE  (POS_AMOUNT_UNITS_MAX_NATIVE / 10000ULL)

/**
 * Ceiling on what the keypad will accept, for the asset currently selected.
 *
 * ETH and POL are 18-decimal and the signed value is a uint64 of wei, so a sale
 * stops at 18.44 of either (see POS_AMOUNT_UNITS_MAX_NATIVE). Enforced here, on
 * the way in, rather than at the confirm step: an operator who can key 99999.99
 * and only then be told no has been allowed to make a mistake the keypad could
 * simply have declined, in front of a customer.
 */
static uint64_t amount_cents_max(void) {
    return pos_chain_is_native_evm(settings_get_chain()) ? AMOUNT_CENTS_MAX_NATIVE
                                                         : AMOUNT_CENTS_MAX;
}

/* From 100.00 up, the cents move to the small font. Five figures and cents in
 * montserrat_28 is ~150 of 240 pixels, and the asset button now shares the row —
 * so the change gives up its size to the part anybody actually reads. Below 100
 * there is room for all of it, and "7.50" with shrunken cents would just look
 * like a typographic tic. */
#define AMOUNT_SMALL_CENTS_FROM  10000ULL   /* 100.00 in cents */

/* Air between the figure and the selector, so they read as two things. */
#define AMOUNT_ROW_GAP  8

/**
 * Place the figure's row: on the screen's centre line, and on the coin's.
 *
 * Vertically: the row's measured height against the selector's 42, so the two
 * boxes share a centre line and the digits' middle IS the coin's. The y used to be
 * that sum written out by hand and it was wrong — 50 against a 30px line box put
 * the figure's middle at 64 against the coin's 67. Measured now, so it also
 * survives a change of font. (Centring the label's box centres the digits:
 * montserrat_28 puts the baseline 25 into a 30px box and draws '0' 20 tall, so the
 * ink runs 5..25 and its middle is the box's. True of this font, not a rule — a
 * font with a deeper descender would want the ink measured instead.)
 *
 * Horizontally: the screen's centre, the same one the keypad and the Charge button
 * below already sit on. An earlier cut centred the row in the line *left of the
 * selector* instead, which is collision-proof but puts the figure 41px left of
 * everything under it, and a till whose amount does not line up with its own keypad
 * looks broken in a way no measurement will talk you out of.
 *
 * Which leaves the collision to handle, because a screen-centred row runs out of
 * room before the digits do: "99999" with small cents and a ticker measures ~173px
 * and a centred row has ~108px before the selector. So the fit is measured, not
 * predicted, and what gives way is chosen to keep the flicker out of the common
 * case:
 *
 * What gives way is the centring, by as little as it takes. Measured on the panel:
 * "0.00" lands on 119 of 120, a typical amount with its ticker on 116, and it
 * drifts left from there as the figure grows — 94 at nine characters, and 1px off
 * the left edge at 99999.99, which is where sliding runs out.
 *
 * The ticker does NOT give way. An earlier cut dropped it to protect the centring
 * and it vanished as the amount grew, which is the one thing on this row that must
 * not happen: the figure is money and the ticker says which money. The clamp below
 * only stops the row clipping off the left edge, and no ticker this build can show
 * reaches it.
 */
static void amount_row_place(void) {
    if (s_amount_row == NULL) { return; }

    /* The selector narrows once there is an amount to read (asset_btn_set_compact),
     * so where it starts depends on the value too. */
    const lv_coord_t pill = (s_amount_cents != 0ULL) ? ASSET_BTN_W_SM : ASSET_BTN_W;
    const lv_coord_t stop = SCR_W + ASSET_BTN_X - pill - AMOUNT_ROW_GAP;

    /* Centred, then measured there — whether it fits is a question about the string
     * that was just set, not about arithmetic. */
    lv_obj_align(s_amount_row, LV_ALIGN_TOP_MID, 0, ASSET_BTN_Y);
    lv_obj_update_layout(s_amount_row);

    lv_area_t   r;
    lv_obj_get_coords(s_amount_row, &r);
    lv_coord_t  x = (r.x2 > stop) ? (stop - r.x2) : 0;

    /* Last resort, and only against clipping: a row that would start off-screen
     * gives up its ticker rather than draw a half figure. "" rather than the hidden
     * flag, the same way amount_update_display drops it at 0.00 — an empty label
     * measures zero and the row re-centres itself, and the next update_display puts
     * it back. */
    if (((r.x1 + x) < 0) && (s_amount_ticker_label != NULL)) {
        lv_label_set_text(s_amount_ticker_label, "");
        lv_obj_update_layout(s_amount_row);
        lv_obj_get_coords(s_amount_row, &r);
        x = (r.x2 > stop) ? (stop - r.x2) : 0;
    }

    lv_obj_align(s_amount_row, LV_ALIGN_TOP_MID, x,
                 ASSET_BTN_Y + ((ASSET_BTN_H - lv_area_get_height(&r)) / 2));
}

static void amount_update_display(void) {
    char buf[24];
    const bool split = (s_amount_cents >= AMOUNT_SMALL_CENTS_FROM);

    if (s_amount_label != NULL) {
        if (split) {
            snprintf(buf, sizeof(buf), "%" PRIu64, s_amount_cents / 100ULL);
        } else {
            snprintf(buf, sizeof(buf), "%" PRIu64 ".%02" PRIu64,
                     s_amount_cents / 100ULL, s_amount_cents % 100ULL);
        }
        lv_label_set_text(s_amount_label, buf);
    }
    if (s_amount_cents_label != NULL) {
        snprintf(buf, sizeof(buf), ".%02" PRIu64, s_amount_cents % 100ULL);
        /* "" rather than the hidden flag: an empty label measures zero and the
         * flex row re-centres on its own, with no rule to remember about how
         * hidden children are laid out. */
        lv_label_set_text(s_amount_cents_label, split ? buf : "");
    }

    /* Nothing entered yet: the selector is the only thing to do on the screen, so
     * it keeps its chevron. Once there is an amount, it gets out of its way — and
     * the figure takes its unit beside it, which is the room the chevron freed.
     *
     * Leading space instead of the row's pad_column, which would also push the
     * small cents off the digits they belong to. The row is content-sized flex, so
     * it re-centres the three labels itself — nothing here places anything. */
    asset_btn_set_compact(s_amount_cents != 0ULL);
    if (s_amount_ticker_label != NULL) {
        if (s_amount_cents != 0ULL) {
            snprintf(buf, sizeof(buf), " %s", asset_name());
            lv_label_set_text(s_amount_ticker_label, buf);
        } else {
            lv_label_set_text(s_amount_ticker_label, "");
        }
    }

    /* Last: the pill has just changed width and the labels have just changed text,
     * and the row is placed against both. */
    amount_row_place();

    s_amount_units = s_amount_cents * 10000ULL;   /* cents -> 6-decimal base units */
}

/* Keypad on the amount screen — cents entry: each digit shifts in
 * from the right (1 -> 0.01, 12 -> 0.12, 1250 -> 12.50). No decimal point. */
static void amount_kbd_cb(lv_event_t *e) {
    lv_obj_t *bm = lv_event_get_target(e);
    const char *txt = lv_btnmatrix_get_btn_text(bm, lv_btnmatrix_get_selected_btn(bm));
    if (txt == NULL) { return; }

    const uint64_t cap = amount_cents_max();
    if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
        s_amount_cents /= 10ULL;
    } else if (strcmp(txt, "00") == 0) {
        uint64_t n = s_amount_cents * 100ULL;
        s_amount_cents = (n > cap) ? cap : n;
    } else if ((txt[0] >= '0') && (txt[0] <= '9') && (txt[1] == '\0')) {
        uint64_t n = (s_amount_cents * 10ULL) + static_cast<uint64_t>(txt[0] - '0');
        if (n <= cap) { s_amount_cents = n; }
    }
    amount_update_display();
}

/* Runs on the UI task (inside lv_timer_handler), so touching shared state and
 * invoking s_cb (which only posts to a queue) is safe here. */
static void btn_event_cb(lv_event_t *e) {
    BtnAction act = static_cast<BtnAction>(
        reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));

    /* The coin rows carry their chain in the action itself, so adding an asset is
     * a row in the picker's table and nothing here. */
    if ((act >= ACT_CHAIN_BASE) && (act < ACT_CHAIN_OF(POS_CHAIN__COUNT))) {
        settings_set_chain(static_cast<pos_chain_t>(act - ACT_CHAIN_BASE));
        /* The entered amount outlives the picker, so switching from a 6-decimal
         * token to an 18-decimal coin can leave a figure on screen that the new
         * asset cannot carry. Clamp it to the new ceiling here — the alternative
         * is a keypad showing 500.00 that refuses every further digit. */
        if (s_amount_cents > amount_cents_max()) {
            s_amount_cents = amount_cents_max();
        }
        close_modal();
        /* Rebuild whatever screen the picker was opened from — the amount
         * screen's selector and ticker name the chain, and so do the Tx tab's
         * asset row, contract address and fee rows. s_req_screen is that screen:
         * a modal is drawn over the current one, never instead of it. The entered
         * amount survives either way; it lives in s_amount_cents, not in the
         * widgets. */
        request_screen(s_req_screen);
        return;
    }

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
             * before signing. Cleared explicitly, so a card read abandoned by any
             * route cannot leave the payment keypad reporting the wrong event. */
            s_pin_for_card = false;
            request_screen(UI_SCREEN_PIN);
            break;
        case ACT_PIN_CANCEL:
            CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(s_pin), sizeof(s_pin));
            s_pin_len = 0;
            if (s_pin_for_card) {
                /* Reported so the waiting main task stops waiting; the card read
                 * has no confirm screen to fall back to. */
                s_pin_for_card = false;
                if (s_cb != NULL) { s_cb(UI_EVENT_CONFIRM_CANCEL, 0); }
                request_screen(UI_SCREEN_PROV);
            } else {
                request_screen(UI_SCREEN_CONFIRM);
            }
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
            s_settings_tab     = 0U;   /* a fresh open starts on Screen */
            s_admin_confirming = false;
            s_admin_for_portal = false;
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
            if (s_admin_for_portal) {
                /* Refuse the browser rather than leave it polling "waiting for the
                 * admin code" for as long as the page stays open. Back to the QR
                 * screen, which is where the wizard was. */
                prov_auth_resolve(false);
                s_admin_for_portal = false;
                request_screen((prov_mode() == PROV_MODE_WIZARD)
                               ? UI_SCREEN_PROV : UI_SCREEN_AMOUNT);
            } else {
                request_screen(UI_SCREEN_AMOUNT);
            }
            break;
        case ACT_CLOSE:
            settings_persist();
            request_screen(s_settings_return);
            break;
        case ACT_WIFI:
            settings_persist();
            set_wifi_progress("Scanning...", NULL);
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
        case ACT_MODAL_CLOSE:
            close_modal();
            break;
        case ACT_PROV_OK:
        case ACT_PROV_NO:
            /* The panel is the only place a payout address or a token contract can
             * be accepted; the browser that proposed one only got as far as this
             * modal. Commit (or drop) before closing, so it cannot outlive the
             * card. */
            (void)prov_pending_commit(act == ACT_PROV_OK);
            close_modal();
            break;
        case ACT_PROV_FINISH:
            if (s_cb != NULL) { s_cb(UI_EVENT_PROV_FINISH, 0); }
            break;
        case ACT_PORTAL:
            open_portal_window();
            break;
        case ACT_PORTAL_CLOSE:
            /* Closing the card closes the page: a config endpoint should not
             * outlive the operator standing in front of the terminal. */
            prov_stop();
            close_modal();
            break;
        case ACT_OTA_NO:
            /* Refused. The staging is dropped and the page goes with it, so a
             * declined image cannot be re-offered to whoever wanders past next. */
            close_modal();
            (void)ota_commit(false);
            prov_stop();
            break;
        case ACT_OTA_OK:
            /* The panel is the only place firmware can be installed; the browser
             * that uploaded it only got as far as this modal. Does not return on
             * success — it reboots into the new slot. */
            close_modal();
            if (!ota_commit(true)) {
                /* It said Install and nothing happened, which is exactly the
                 * report this whole path came from. Two ways to get here now that
                 * the portal closing no longer withdraws a staged image: the
                 * terminal rebooted since the upload (staging is RAM), or the slot
                 * refused to become bootable. Either way, say so — a card that
                 * just closes is indistinguishable from a successful update that
                 * silently did not happen. */
                prov_stop();
                open_ota_gone();
            }
            break;
        case ACT_NET_PICK:
            open_network_picker();
            break;
        case ACT_NET_ETH:
        case ACT_NET_POLY:
        case ACT_NET_TRON:
            /* Step 2: which coin on the network just picked. */
            open_coin_picker((act == ACT_NET_TRON) ? UI_NET_TRON :
                             (act == ACT_NET_POLY) ? UI_NET_POLY : UI_NET_ETH);
            break;
        case ACT_CHAIN_BASE:
            /* Unreachable — the chain block above returns. Named only so -Wswitch
             * keeps checking that every other action still has a case here. */
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

/* Round icon disc with a glyph — the coin mark in a pill's left slot, and (at
 * CHIP_SZ) the network chip clipped to its corner. Never clickable: it sits
 * inside a pill button, and a clickable child would swallow that tap. */
#define COIN_SZ   30
#define CHIP_SZ   16
#define BADGE_SZ  (COIN_SZ + 6)   /* room for the chip to hang off the corner */

static lv_obj_t *make_glyph_disc(lv_obj_t *parent, const char *sym,
                                 lv_color_t bg, lv_coord_t sz) {
    lv_obj_t *d = lv_obj_create(parent);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, sz, sz);
    lv_obj_set_style_bg_color(d, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = lv_label_create(d);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_color(l, COL_BG, LV_PART_MAIN);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(l);
    return d;
}

/* Coin mark, optionally with a network chip on its bottom-right corner, the way
 * wallets badge a token with the chain it lives on — so "USDC" (Sepolia) and a
 * TRC-20 are told apart by the icon and not only by the subtitle.
 *
 * All four marks are real logos (tools/gen_chain_icons.py draws the TRON
 * triangle, the Tether stem and the Ethereum octahedron; Circle artwork is
 * Circle artwork). The chips carry their white ring in the bitmap.
 *
 * @p chip may be NULL — a bare network mark needs no chip of itself, and the
 * box then shrinks to COIN_SZ.
 *
 * The COIN is centred in the box and the chip hangs off the corner, so the box's
 * centre IS the mark's centre: every caller centres the box against a figure or
 * inside a pill and gets the mark centred, with no per-site nudge for the 6px the
 * chip adds. (It used to sit top-left, which drew the mark 3px high everywhere.)
 *
 * Returns the box for the caller to position; the children ride along. */
static lv_obj_t *make_icon_box(lv_obj_t *parent, const lv_img_dsc_t *coin_src,
                               const lv_img_dsc_t *chip_src) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, (chip_src != NULL) ? BADGE_SZ : COIN_SZ,
                         (chip_src != NULL) ? BADGE_SZ : COIN_SZ);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *coin = lv_img_create(box);
    lv_img_set_src(coin, coin_src);
    lv_obj_center(coin);

    if (chip_src != NULL) {
        lv_obj_t *chip = lv_img_create(box);
        lv_img_set_src(chip, chip_src);
        lv_obj_align(chip, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    }
    return box;
}

/* The selected asset. A native coin IS its network, so it carries no chip —
 * the chip only says "this token lives over there", which is meaningless
 * stacked on the network's own logo. */
static lv_obj_t *make_asset_badge(lv_obj_t *parent, pos_chain_t chain) {
    switch (chain) {
        /* The network's own coin wears its network mark and no chip: a chip says
         * "this token, on that network", and there is no second thing to say when
         * the asset IS the network. */
        case POS_CHAIN_TRON_NILE: return make_icon_box(parent, &icon_tron, NULL);
        case POS_CHAIN_ETH_NATIVE: return make_icon_box(parent, &icon_eth, NULL);
        case POS_CHAIN_POLY_NATIVE:return make_icon_box(parent, &icon_poly, NULL);
        case POS_CHAIN_TRON_USDT: return make_icon_box(parent, &icon_usdt, &chip_tron);
        case POS_CHAIN_TRON_USDC: return make_icon_box(parent, &icon_usdc, &chip_tron);
        case POS_CHAIN_ETH_USDT:  return make_icon_box(parent, &icon_usdt, &chip_eth);
        case POS_CHAIN_POLY_USDC: return make_icon_box(parent, &icon_usdc, &chip_poly);
        case POS_CHAIN_POLY_USDT: return make_icon_box(parent, &icon_usdt, &chip_poly);
        default:                  return make_icon_box(parent, &icon_usdc, &chip_eth);
    }
}

/* The asset selector itself: the badge above turned into a tappable pill, at the
 * right-hand end of the amount's row. Wordless — the ticker is spelled out on the
 * picker it opens, on the Tx tab and on the confirm screen, and a compact mark
 * leaves the amount the biggest thing on the screen. (A pill carrying the ticker
 * as well does not fit beside a five-figure amount on 240px, which is what put it
 * on a line of its own in the first place.)
 *
 * Mark hard left, arrow hard right — aligned, never measured: the box has no
 * width until LVGL runs a layout pass, and asking anyway put the icon on the
 * wrong side of the button. A chipped box is 6px wider than a bare one, so its
 * mark starts 3px further in; not worth a per-chain constant. */
static lv_obj_t *make_asset_button(void) {
    /* make_button carries the fill/press/border/event wiring; this one is an
     * icon row, so its label is pushed right to serve as the arrow. */
    s_asset_btn = make_button(lv_scr_act(), LV_SYMBOL_RIGHT, COL_SURFACE,
                              COL_TEXT, ASSET_BTN_W, ASSET_BTN_H,
                              LV_ALIGN_TOP_RIGHT, ASSET_BTN_X, ASSET_BTN_Y,
                              ACT_NET_PICK, &lv_font_montserrat_14);
    lv_obj_set_style_radius(s_asset_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    /* Zero the theme's button padding first, exactly as make_pill does. Child
     * aligns are relative to the CONTENT area, so the default ~12px inset shoved
     * the badge inward until it sat on top of the arrow — and the badge is added
     * last, so it wins the draw order and the arrow simply vanished. */
    lv_obj_set_style_pad_all(s_asset_btn, 0, LV_PART_MAIN);
    s_asset_arrow = lv_obj_get_child(s_asset_btn, 0);
    lv_obj_align(s_asset_arrow, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_align(make_asset_badge(s_asset_btn, settings_get_chain()),
                 LV_ALIGN_LEFT_MID, ASSET_BTN_PAD, 0);
    return s_asset_btn;
}

/* Chevron off and the pill narrowed to its badge, so a five-figure amount has
 * the middle of the screen. Right-aligned, so only the left edge moves — the
 * thing under the thumb stays where it was. */
static void asset_btn_set_compact(bool compact) {
    if (s_asset_btn == NULL) { return; }   /* not the amount screen */
    lv_obj_set_width(s_asset_btn, compact ? ASSET_BTN_W_SM : ASSET_BTN_W);
    if (s_asset_arrow != NULL) {
        if (compact) { lv_obj_add_flag(s_asset_arrow, LV_OBJ_FLAG_HIDDEN); }
        else         { lv_obj_clear_flag(s_asset_arrow, LV_OBJ_FLAG_HIDDEN); }
    }
}

/* The bare network mark, for the network picker's own rows — no coin is chosen
 * at that step, so there is nothing to badge it with. */
static lv_obj_t *make_net_badge(lv_obj_t *parent, ui_net_t net) {
    return make_icon_box(parent,
                         (net == UI_NET_TRON) ? &icon_tron :
                         (net == UI_NET_POLY) ? &icon_poly : &icon_eth, NULL);
}

/* "Tap here" mark — the four widening arcs every contactless reader wears, drawn
 * by LVGL instead of shipped as a bitmap. It replaces a traced hand-and-card icon
 * of unclear provenance: arcs are geometry, so there is no artwork to license,
 * and ~18 KB of .rodata comes back. (The plain arcs only — EMVCo's Contactless
 * Symbol is a registered mark and is not what this draws.)
 *
 * All four are concentric on the same point, so only their right-hand quadrant is
 * ink; TAP_MARK_X shifts that ink back over the screen's centre line. Aligned
 * TOP_MID at @p y, occupying TAP_MARK_SZ square — the same box the bitmap had. */
#define TAP_MARK_SZ   96
#define TAP_MARK_X    (-TAP_MARK_SZ / 4)   /* ink is the right half — recentre */
#define TAP_MARK_W    6                    /* stroke                          */
#define TAP_MARK_SPAN 45                    /* +/- degrees about 3 o'clock     */

static void make_tap_mark(lv_coord_t y) {
    static const lv_coord_t sz[] = { TAP_MARK_SZ, 70, 44, 18 };
    for (unsigned i = 0U; i < (sizeof(sz) / sizeof(sz[0])); i++) {
        lv_obj_t *a = lv_arc_create(lv_scr_act());
        lv_obj_remove_style(a, NULL, LV_PART_KNOB);   /* not a control */
        lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(a, sz[i], sz[i]);
        lv_arc_set_bg_angles(a, 360 - TAP_MARK_SPAN, TAP_MARK_SPAN);
        lv_obj_set_style_arc_width(a, TAP_MARK_W, LV_PART_MAIN);
        lv_obj_set_style_arc_color(a, COL_TEXT, LV_PART_MAIN);
        lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
        lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
        /* Same centre for every ring: TOP_MID shares the x, the offset shares
         * the y (each smaller box inset by half the difference). */
        lv_obj_align(a, LV_ALIGN_TOP_MID, TAP_MARK_X,
                     y + ((TAP_MARK_SZ - sz[i]) / 2));
    }
}

/* Selector row in the style of button_style.png: full-radius grey pill, round
 * icon on the left, title over a dim subtitle, chevron on the right. The icon
 * is left to the caller (asset badge or glyph disc):
 *
 *   lv_obj_t *p = make_pill(...);
 *   lv_obj_align(make_asset_badge(p, chain), LV_ALIGN_LEFT_MID, PILL_ICON_X, 0);
 *
 * @p w is in pixels on purpose — lv_pct() cannot be measured for the subtitle. */
#define PILL_H       52
#define PILL_TEXT_X  52          /* clears the BADGE_SZ icon at PILL_ICON_X */
#define PILL_ICON_X  10
/* Right-hand reserve for the chevron (drawn at -14, ~8px wide). Was 30, which
 * ate enough of a 196px pill to dot-elide "Tron Nile TRC-20"; 24 clears the
 * glyph and gives the subtitle the room back. */
#define PILL_TEXT_PAD_R  24

static lv_obj_t *make_pill(lv_obj_t *parent, const char *title, const char *sub,
                           lv_coord_t w, lv_coord_t y, BtnAction act,
                           bool leaf = false) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, PILL_H);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_color(btn, COL_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_NONE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn,
                              lv_color_mix(lv_color_black(), COL_SURFACE, 20),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(act)));

    if (sub != NULL) {
        make_label(btn, title, COL_TEXT, &lv_font_montserrat_20,
                   LV_ALIGN_TOP_LEFT, PILL_TEXT_X, 7);
        lv_obj_t *sl = make_label(btn, sub, COL_DIM, &lv_font_montserrat_14,
                                  LV_ALIGN_TOP_LEFT, PILL_TEXT_X, 30);
        /* A leaf row's subtitle is left at LV_SIZE_CONTENT — it is one of our
         * own fixed strings and it must be readable in full. Only the rows that
         * open something get a width cap, because those show an SSID, which can
         * be 32 characters and has to elide somewhere. */
        if (!leaf) {
            lv_obj_set_width(sl, w - PILL_TEXT_X - PILL_TEXT_PAD_R);
            lv_label_set_long_mode(sl, LV_LABEL_LONG_DOT);
        }
    } else {
        make_label(btn, title, COL_TEXT, &lv_font_montserrat_20,
                   LV_ALIGN_LEFT_MID, PILL_TEXT_X, 0);
    }
    /* The chevron means "this opens something". A leaf row is the choice
     * itself, so it gets no chevron — and hands the space to the subtitle. */
    if (!leaf) {
        make_label(btn, LV_SYMBOL_RIGHT, COL_DIM, &lv_font_montserrat_14,
                   LV_ALIGN_RIGHT_MID, -14, 0);
    }
    return btn;
}

static void clear_screen(void) {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, COL_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    s_amount_label = NULL;
    s_amount_cents_label = NULL;
    s_amount_row   = NULL;
    s_asset_btn    = NULL;
    s_asset_arrow  = NULL;
    s_amount_ticker_label = NULL;
    s_pin_ta       = NULL;   /* deleted by lv_obj_clean — drop the dangling ref */
    s_wifi_pass_ta = NULL;
    s_wifi_eye_lbl   = NULL;
    s_boot_step_lbl  = NULL;
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

/* Show the Reset button (and shrink Close) only on the About tab. Driven by the
 * tab index rather than by an event, so the rebuild path can call it directly
 * instead of faking a tab change. */
static void settings_bottom_bar(uint16_t tab) {
    const bool about = (tab == TAB_ABOUT);
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

static void tab_change_cb(lv_event_t *e) {
    lv_obj_t *tabbar = lv_event_get_target(e);
    /* Only a real press names a button; anything else reports
     * LV_BTNMATRIX_BTN_NONE (0xFFFF), which must not be mistaken for a tab. */
    uint16_t sel = lv_btnmatrix_get_selected_btn(tabbar);
    if (sel >= SETTINGS_TAB_COUNT) { return; }
    s_settings_tab = sel;
    settings_bottom_bar(sel);
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
        lv_obj_set_style_pad_all(pages[i], TAB_PAD, LV_PART_MAIN);
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

    /* Read-only, and a caption/value pair rather than a pill: the network is set
     * from the Configure page below, so a grey pill with a chevron — this page's
     * own "tap me" — was an invitation to type a venue passphrase on a resistive
     * panel, which is the thing the browser flow exists to avoid. The on-device
     * picker still exists for the one case that cannot go through a browser: the
     * terminal cannot re-join and raises it by itself (see main's wifi_picker). */
    make_label(t_wifi, "Network", COL_DIM, &lv_font_montserrat_14,
               LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *wl = make_label(t_wifi, have_wifi ? cur_ssid : "Not configured",
                              COL_TEXT, &lv_font_montserrat_14,
                              LV_ALIGN_TOP_LEFT, 0, 20);
    /* An SSID is 32 arbitrary characters; elide it on real glyph widths. */
    lv_obj_set_width(wl, TAB_W);
    lv_label_set_long_mode(wl, LV_LABEL_LONG_DOT);

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

    /* Everything else lives in a browser, and this is the row that gets you
     * there: it raises the config page and shows a QR code to scan. Same row on
     * the About tab, because "update the firmware" and "change the addresses" are
     * two things on one page and an operator should reach it from either. */
    /* Subtitle held to make_pill's cap, like the Update row on the About tab:
     * TAB_W - PILL_TEXT_X - PILL_TEXT_PAD_R = 140px, and montserrat_14 renders
     * "Scan to open in a browser" at 184px — so it arrived dot-elided, which on
     * the row that explains the feature reads as a bug. This is 131px, and it
     * says where the operator ends up rather than naming a QR code they have not
     * been shown yet; the screen this opens is what shows them the code. Reads as
     * a pair with the About tab's "From a browser". */
    lv_obj_t *cpill = make_pill(t_wifi, "Configure", "Open in a browser",
                                TAB_W, 100, ACT_PORTAL);
    lv_obj_align(make_glyph_disc(cpill, LV_SYMBOL_SETTINGS, COL_ACCENT, COIN_SZ),
                 LV_ALIGN_LEFT_MID, PILL_ICON_X, 0);

    /* ── Transaction tab: which asset, what it will call, and where the funds go.
     * The asset can be switched from here as well as from the amount screen: the
     * operator needs it there during a shift, and an administrator setting the
     * terminal up expects to find it with the other transaction settings. ── */
    const bool tron = chain_is_tron();
    /* Ticker over network, not "Asset" over "USDC on Ethereum Sepolia": that line
     * measured ~178px against make_pill's 140px cap, so the row that says which
     * chain the terminal is charging on arrived dot-elided as "USDC on Ethereum
     * Sep...". Split across the pill's two lines both fit at full length, and the
     * badge beside them already says "asset". */
    lv_obj_t *apill = make_pill(t_tx, asset_name(), asset_network(),
                                TAB_W, 0, ACT_NET_PICK);
    lv_obj_align(make_asset_badge(apill, settings_get_chain()),
                 LV_ALIGN_LEFT_MID, PILL_ICON_X, 0);

    make_label(t_tx, asset_caption(), COL_DIM,
               &lv_font_montserrat_14, LV_ALIGN_TOP_LEFT, 0, 62);
    lv_obj_t *a_usdc = make_label(t_tx, (s_addr_usdc != NULL) ? s_addr_usdc : "-",
                                  COL_TEXT, &lv_font_montserrat_14,
                                  LV_ALIGN_TOP_LEFT, 0, 80);
    lv_label_set_long_mode(a_usdc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(a_usdc, 200);

    make_label(t_tx, "Send to", COL_DIM, &lv_font_montserrat_14,
               LV_ALIGN_TOP_LEFT, 0, 126);
    lv_obj_t *a_dest = make_label(t_tx, (s_addr_dest != NULL) ? s_addr_dest : "-",
                                  COL_TEXT, &lv_font_montserrat_14,
                                  LV_ALIGN_TOP_LEFT, 0, 144);
    lv_label_set_long_mode(a_dest, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(a_dest, 200);

    /* Say out loud when the recipient is the compile-time one rather than an
     * address somebody chose. This is the row an operator checks to answer "where
     * does my money go", and "the default" is a different answer from "mine".
     * The address above is still shown, because it is what the row is for — but
     * the sale is refused at the confirm step, so say that and not "in use". */
    if (!settings_has_payout(tron)) {
        lv_obj_t *w = make_label(t_tx, "Not configured - this terminal cannot "
                                       "take payments. Set it from the "
                                       "Configure page.",
                                 COL_DANGER, &lv_font_montserrat_14,
                                 LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_width(w, 200);
        lv_label_set_long_mode(w, LV_LABEL_LONG_WRAP);
        lv_obj_update_layout(a_dest);
        lv_obj_align_to(w, a_dest, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 4);
    }

    /* Load the fees whether or not the steppers are built — settings_persist()
     * writes these back on Close, and zeroes would break the Ethereum path. */
    s_maxfee_gwei = settings_get_max_fee_gwei();
    s_prio_gwei   = settings_get_priority_fee_gwei();
    if (s_prio_gwei > s_maxfee_gwei) { s_prio_gwei = s_maxfee_gwei; }
    if (tron) {
        /* Tron pays for a transfer in bandwidth and energy, not in a fee the
         * operator sets — the TRC-20 fee cap is compile-time (config.h), so
         * there is still nothing to tune here. */
        lv_obj_t *n = make_label(t_tx,
                                 (settings_get_chain() == POS_CHAIN_TRON_NILE)
                                 ? "TRX transfers are paid in bandwidth - "
                                   "no gas fee to set."
                                 : "Token transfers burn the card's energy, "
                                   "capped in config.h - no gas fee to set.",
                                 COL_DIM, &lv_font_montserrat_14,
                                 LV_ALIGN_TOP_LEFT, 0, 200);
        lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(n, 200);
    } else {
        /* Pushed down by the asset row added above; the tab scrolls, so the only
         * thing these offsets have to do is not overlap. */
        s_maxfee_lbl = build_fee_row(t_tx, "Max fee (Gwei)",      196, 0, 1);
        s_prio_lbl   = build_fee_row(t_tx, "Priority fee (Gwei)", 264, 2, 3);
        fee_update_labels();
        /* Say it here rather than let the numbers lie: these two are shared with
         * Ethereum, and on Polygon the firmware lifts the tip to its floor
         * (config.h) because Amoy drops anything under ~25 Gwei. Without this row
         * the tab shows 20 while the signed transaction carries 30. */
        if (pos_chain_is_polygon(settings_get_chain())) {
            lv_obj_t *n = make_label(t_tx,
                                     "On Polygon the tip is raised to at least "
                                     "the config.h floor (30 Gwei).",
                                     COL_DIM, &lv_font_montserrat_14,
                                     LV_ALIGN_TOP_LEFT, 0, 332);
            lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(n, 200);
        }
    }

    /* ── About tab: small C logo, name, version, info ── */
    lv_obj_t *blogo = lv_img_create(t_about);
    lv_img_set_src(blogo, &logo_small);   /* 40px dedicated image */
    lv_obj_align(blogo, LV_ALIGN_TOP_MID, 0, 0);

    make_label(t_about, "cryptnox-pos", COL_TEXT, &lv_font_montserrat_20,
               LV_ALIGN_TOP_MID, 0, 42);
    /* Straight out of the running image's header rather than a #define, so that
     * after an update this reads as the firmware that is actually executing. */
    make_label(t_about, ota_running_version(), COL_DIM, &lv_font_montserrat_14,
               LV_ALIGN_TOP_MID, 0, 70);

    /* An update that installed, booted and was then reverted leaves this tab
     * reading the old version with nothing to say why — which is how "I updated
     * it and nothing happened" gets reported about a mechanism that worked
     * exactly as designed. One red line, under the version it is about. It clears
     * itself: the next update overwrites the slot this verdict is read from.
     *
     * The rows below shift down by the line's height when it is there, rather
     * than the line being squeezed into the gap: this tab scrolls, so there is
     * somewhere for them to go, and a warning overlapping the Update button is
     * worse than no warning. */
    const bool reverted = ota_last_update_failed();
    if (reverted) {
        make_label(t_about, "Last update rolled back", COL_DANGER,
                   &lv_font_montserrat_14, LV_ALIGN_TOP_MID, 0, 90);
    }
    const lv_coord_t y_update = reverted ? 122 : 94;
    const lv_coord_t y_about  = reverted ? 184 : 156;

    /* The update row. Tapping it opens the config page on the venue network and
     * shows a QR code to scan, so it belongs behind the admin code with the rest
     * of the settings. Same action as the Configure row on the Wi-Fi tab — one
     * page, reachable from wherever the operator went looking for it. */
    /* Subtitle kept short deliberately: make_pill caps it at
     * TAB_W - PILL_TEXT_X - PILL_TEXT_PAD_R = 140 px and dot-elides the rest,
     * and a row whose own label is cut off reads as a bug. */
    lv_obj_t *upill = make_pill(t_about, "Update", "From a browser",
                                TAB_W, y_update, ACT_PORTAL);
    lv_obj_align(make_glyph_disc(upill, LV_SYMBOL_DOWNLOAD, COL_ACCENT, COIN_SZ),
                 LV_ALIGN_LEFT_MID, PILL_ICON_X, 0);

    lv_obj_t *about = make_label(t_about,
                                 "ETH, POL, TRX, USDC and USDT\n"
                                 "terminal on Ethereum Sepolia,\n"
                                 "Polygon Amoy and Tron Nile,\n"
                                 "for Cryptnox cards\n\n"
                                 "Based on cryptnox-sdk-esp32 1.0.0\n"
                                 "(c) Cryptnox 2026 - Educational use only\n\n"
                                 "Licensed under LGPL-3.0-or-later\n\n"
                                 "Third-party: ESP-IDF (Apache-2.0),\n"
                                 "LVGL (MIT), TFT_eSPI (FreeBSD/MIT),\n"
                                 "XPT2046_Touchscreen (MIT)",
                                 COL_DIM, &lv_font_montserrat_14,
                                 LV_ALIGN_TOP_MID, 0, y_about);
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
    /* Same full-radius pill shape as the selector rows above. */
    lv_obj_set_style_radius(s_close_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(s_reset_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_add_event_cb(lv_tabview_get_tab_btns(tv), tab_change_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    /* Back to the tab the operator was on. A rebuild is how this page applies a
     * changed asset, and landing on Screen afterwards reads as the page having
     * closed and reopened by itself. */
    if (s_settings_tab != 0U) {
        lv_tabview_set_act(tv, s_settings_tab, LV_ANIM_OFF);
    }
    settings_bottom_bar(s_settings_tab);
}

/* Modal overlays (factory-reset confirmation, network picker). One at a time:
 * both are full-screen and both are opened from the settings page. */
static lv_obj_t *s_modal = NULL;

/* Whether the modal on screen belongs to the config portal. The admin page shuts
 * itself down after PROV_WINDOW_MIN whether or not anybody is watching, and a card
 * left behind after that would wedge the terminal: these overlays swallow every
 * touch, so a terminal nobody came back to could not take a payment again until it
 * was power-cycled. The UI task uses this to clear the card when the window closes
 * underneath it. */
static bool s_portal_modal = false;

static void close_modal(void) {
    if (s_modal != NULL) {
        lv_obj_del(s_modal);
        s_modal = NULL;
    }
    s_portal_modal = false;
}

/* Dimmed overlay + centred card on the top layer, so it floats above the
 * settings page. Returns the card for the caller to fill. */
static lv_obj_t *open_modal(lv_coord_t w, lv_coord_t h) {
    close_modal();

    s_modal = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_modal);
    lv_obj_set_size(s_modal, SCR_W, SCR_H);
    lv_obj_set_style_bg_color(s_modal, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_modal, LV_OPA_70, LV_PART_MAIN);
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *card = lv_obj_create(s_modal);
    lv_obj_set_size(card, w, h);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, COL_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 14, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 8, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    return card;
}

static void open_reset_confirm(void) {
    lv_obj_t *card = open_modal(224, 210);

    lv_obj_t *msg = make_label(card,
                               "Erase all settings\n"
                               "(Wi-Fi, brightness, fees)\n"
                               "and reboot?",
                               COL_TEXT, &lv_font_montserrat_14,
                               LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    make_button(card, "Erase", COL_DANGER, COL_TEXT, 196, 40,
                LV_ALIGN_BOTTOM_MID, 0, -50, ACT_RESET_CONFIRM, &lv_font_montserrat_20);
    make_button(card, "Cancel", COL_SURFACE, COL_TEXT, 196, 40,
                LV_ALIGN_BOTTOM_MID, 0, -2, ACT_MODAL_CLOSE, &lv_font_montserrat_20);
}

/**
 * Open the config page and show the operator how to reach it.
 *
 * A QR code, not a typed address: the page is served on the venue network on a
 * device with no name to look up, so the address is the only way anyone can find
 * it — and reading a dotted quad off a 2.8" panel into a phone is the step that
 * goes wrong. The URL is printed underneath for the camera that will not play
 * along, and because a laptop has no camera to point.
 */
/* Card geometry for the two update modals. 228 wide leaves 212 inside the 8 px
 * pad; wrapped text gets 200 so it clears the rounded corners.
 *
 * Neither card has a hand-measured layout, because the version, the running
 * version and the address are all substituted at runtime and none of them has a
 * known length: blocks stack with lv_obj_align_to(), then ota_fit_card() grows
 * the card to whatever height the text turned out to need. Sized by eye, these
 * fitted on the strings they were written with and put text under the buttons on
 * the next ones. */
#define OTA_CARD_W   228
#define OTA_TEXT_W   200
#define OTA_GAP      8
#define OTA_CARD_PAD 8   /* open_modal()'s pad_all */

/** Wrapped, centred body text stacked under @p above (or the card top). */
static lv_obj_t *ota_text(lv_obj_t *card, lv_obj_t *above, const char *txt,
                          lv_color_t col, const lv_font_t *font) {
    lv_obj_t *l = make_label(card, txt, col, font, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_width(l, OTA_TEXT_W);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    if (above == NULL) {
        lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 4);
    } else {
        /* align_to() reads the base's current coordinates, and a label that has
         * just been given a width has not been laid out yet — without this the
         * wrapped height is still the pre-wrap one and every block below stacks
         * against a stale bottom edge. */
        lv_obj_update_layout(card);
        lv_obj_align_to(l, above, LV_ALIGN_OUT_BOTTOM_MID, 0, OTA_GAP);
    }
    return l;
}

/**
 * Resize the card to the text it ended up with, leaving room for the buttons.
 *
 * @p buttons_h is how far the button block reaches up from the content bottom —
 * 42 for one 40 px button at -2, 86 for two. Clamped to the screen, so a
 * pathological string elides off the bottom rather than drawing a card taller
 * than the panel.
 */
static void ota_fit_card(lv_obj_t *card, lv_obj_t *last, lv_coord_t buttons_h) {
    lv_obj_update_layout(card);
    lv_coord_t need = lv_obj_get_y(last) + lv_obj_get_height(last)
                      + OTA_GAP + buttons_h + (2 * OTA_CARD_PAD);
    if (need > (SCR_H - 16)) { need = SCR_H - 16; }
    lv_obj_set_height(card, need);
    lv_obj_center(card);
}

static void open_portal_window(void) {
    /* Started here, on the UI task, the same way this screen already calls
     * settings_factory_reset() and prov_pending_commit() directly. The event
     * callback is handed over so a submission can come back to the panel. */
    const bool up = prov_start(PROV_MODE_ADMIN, s_cb);

    if (!up) {
        lv_obj_t *card = open_modal(OTA_CARD_W, 200);
        lv_obj_t *m = ota_text(card, NULL,
                 "The terminal's own Wi-Fi would not come up, so there is "
                 "nothing for a phone to join.\n\nRestart and try again.",
                 COL_TEXT, &lv_font_montserrat_14);
        ota_fit_card(card, m, 42);
        make_button(card, "Close", COL_SURFACE, COL_TEXT, OTA_TEXT_W, 40,
                    LV_ALIGN_BOTTOM_MID, 0, -2, ACT_MODAL_CLOSE,
                    &lv_font_montserrat_20);
        return;
    }

    lv_obj_t *card = open_modal(OTA_CARD_W, 300);

    /* No caption over the code. A QR code on a payment terminal does not need to be
     * told to a person holding a phone, and the line was the one thing on the card
     * that named a device: the credentials underneath are equally for a laptop.
     *
     * The same "WIFI:..." code the setup screen shows: the page is on the
     * terminal's own AP, so joining it is the whole journey and a camera does that
     * from the code directly. 104 px is 26 modules at 4 px, which holds the SSID
     * and the passphrase. The white quiet zone is not optional: a code drawn hard
     * against an edge is a code half the scanners in the world will not see. */
    lv_obj_t *qr = lv_qrcode_create(card, 104, COL_TEXT, COL_BG);
    if (qr != NULL) {
        const char *payload = prov_qr_payload();
        (void)lv_qrcode_update(qr, payload, strlen(payload));
        lv_obj_set_style_border_color(qr, COL_BG, LV_PART_MAIN);
        lv_obj_set_style_border_width(qr, 4, LV_PART_MAIN);
        /* Top of the card, where the caption used to start — same 4px inset
         * ota_text() gives a first block. */
        lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 4);
    }

    /* The credentials in text too — a laptop has no camera to point, and a code
     * that will not scan in a dim bar still leaves ten characters to type. */
    char creds[80];
    snprintf(creds, sizeof(creds), "SSID: %s\nPassword: %s",
             prov_ap_ssid(), prov_ap_pass());
    lv_obj_t *url = ota_text(card, qr, creds, COL_TEXT, &lv_font_montserrat_14);

    /* Two short lines, broken by hand, because the card cannot grow: 304px is
     * ota_fit_card's ceiling, and the 112px code, the credentials and the Done
     * button leave about 45px of it — three lines at most. The paragraph
     * that used to sit here ran to nine and simply stopped mid-sentence at the
     * card's edge. Each line is kept under OTA_TEXT_W (200px, ~27 characters at
     * montserrat_14) so neither wraps into a fourth. What was dropped is not lost:
     * "the admin code is typed here, never there" is the first thing the browser
     * page itself says, to the person who needs to read it. */
    char note[64];
    snprintf(note, sizeof(note),
             "Venue Wi-Fi off while open\nCloses in %u min",
             prov_window_left_min());
    lv_obj_t *n = ota_text(card, url, note, COL_DIM, &lv_font_montserrat_14);
    ota_fit_card(card, n, 42);

    make_button(card, "Done", COL_SURFACE, COL_TEXT, OTA_TEXT_W, 40,
                LV_ALIGN_BOTTOM_MID, 0, -2, ACT_PORTAL_CLOSE,
                &lv_font_montserrat_20);

    s_portal_modal = true;   /* set last: open_modal() cleared it */
}

/**
 * Accept or refuse firmware that the update page has uploaded.
 *
 * The upload has already been verified — SHA-256, and the signature on a signed
 * build — so this is not asking whether the image is genuine. It is asking
 * whether the person holding the terminal wants this version on it, which is a
 * different question and the one a browser cannot answer. A version that goes
 * backwards is called out: the image is properly signed either way, and
 * returning a terminal to firmware with a known fault is a plausible thing for
 * somebody to be talked into.
 */
/**
 * Install was tapped and there was nothing left to install.
 *
 * The one screen this flow was missing. Everything else about an update reports
 * itself — the upload has a percentage, the verification has a version, the
 * install has a reboot — but the gap between "accepted on the panel" and "nothing
 * happened" was silent, so a terminal that quietly stayed on the old firmware
 * looked exactly like one that had updated.
 */
static void open_ota_gone(void) {
    lv_obj_t *card = open_modal(OTA_CARD_W, 200);
    lv_obj_t *m = ota_text(card, NULL,
             "That firmware is no longer waiting to be installed.\n\n"
             "Nothing changed - this terminal is still on the version it was. "
             "Open Update again and send the file once more.",
             COL_TEXT, &lv_font_montserrat_14);
    ota_fit_card(card, m, 42);
    make_button(card, "Close", COL_SURFACE, COL_TEXT, OTA_TEXT_W, 40,
                LV_ALIGN_BOTTOM_MID, 0, -2, ACT_MODAL_CLOSE,
                &lv_font_montserrat_20);
}

static void build_ota_confirm(void) {
    char version[40] = "?";
    bool older = false;
    if (!ota_staged(version, sizeof(version), &older)) { return; }

    lv_obj_t *card = open_modal(OTA_CARD_W, 252);

    /* The downgrade warning is the caption, not an extra paragraph in the body:
     * one red line above the version says it, and the body stays the same length
     * either way — which is what keeps this card a fixed height. */
    lv_obj_t *cap = ota_text(card, NULL,
                             older ? "This is an OLDER version" : "New firmware",
                             older ? COL_DANGER : COL_DIM,
                             &lv_font_montserrat_14);
    /* 28 pt fits about twelve characters on one line. A version longer than that
     * is a `git describe` string rather than a release tag, and wrapping it at
     * this size costs two more lines than the card can spare — so step down. */
    lv_obj_t *ver = ota_text(card, cap, version, COL_TEXT,
                             (strlen(version) > 12U) ? &lv_font_montserrat_20
                                                     : &lv_font_montserrat_28);

    char body[128];
    snprintf(body, sizeof(body),
             "Running %s.\n\nThe terminal restarts now. Not during a payment.",
             ota_running_version());
    lv_obj_t *m = ota_text(card, ver, body, COL_DIM, &lv_font_montserrat_14);
    ota_fit_card(card, m, 86);

    make_button(card, "Install", COL_ACCENT, COL_BG, OTA_TEXT_W, 40,
                LV_ALIGN_BOTTOM_MID, 0, -46, ACT_OTA_OK, &lv_font_montserrat_20);
    make_button(card, "Discard", COL_SURFACE, COL_TEXT, OTA_TEXT_W, 40,
                LV_ALIGN_BOTTOM_MID, 0, -2, ACT_OTA_NO, &lv_font_montserrat_20);

    s_portal_modal = true;
}

/* Asset selection, in two steps: the network, then the coin on it. Two modals
 * rather than one flat list of chains — a chain is a (network, coin) pair, and
 * merging them made "USDT Tron Nile TRC-20" a single row the operator had to
 * parse. Each card is styled like the selector that opened it, so the choice
 * looks like the thing being chosen.
 *
 * The card is 228 wide (212 inside the 8px pad), so the pills run to 206. */
#define PICK_W  206

/**
 * Grey a row out and make it inert.
 *
 * For a network whose payout address nobody has set. Shown rather than hidden: a
 * network that silently vanishes reads as a firmware that lost a feature, where a
 * dimmed row saying why tells the operator what to go and do.
 */
static void pill_disable(lv_obj_t *p) {
    lv_obj_clear_flag(p, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(p, LV_OPA_50, LV_PART_MAIN);
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(p); i++) {
        lv_obj_set_style_text_color(lv_obj_get_child(p, i), COL_DIM, LV_PART_MAIN);
    }
}

/** Step 1 — the network. */
static void open_network_picker(void) {
    static const struct {
        const char *name;
        const char *sub;
        ui_net_t    net;    /* which network mark to draw */
        BtnAction   act;
    } NETS[] = {
        { "Ethereum", "Sepolia testnet", UI_NET_ETH,  ACT_NET_ETH  },
        { "Polygon",  "Amoy testnet",    UI_NET_POLY, ACT_NET_POLY },
        { "Tron",     "Nile testnet",    UI_NET_TRON, ACT_NET_TRON },
    };
    const size_t n = sizeof(NETS) / sizeof(NETS[0]);

    /* Same growth rule as step 2 — header + rows + the Cancel button — now that
     * there are three networks and a hardcoded height would clip one. */
    lv_obj_t *card = open_modal(228,
        static_cast<lv_coord_t>(86 + (n * (PILL_H + 2))));

    make_label(card, "Network", COL_DIM, &lv_font_montserrat_14,
               LV_ALIGN_TOP_MID, 0, 2);

    for (size_t i = 0; i < n; i++) {
        /* A network with no payout address of its own is not offered. Otherwise a
         * terminal set up for Ethereum and interrupted before Tron would quietly
         * take Tron payments to the compile-time recipient — somebody else's
         * address — and look entirely normal doing it. */
        /* Polygon spends the Ethereum payout address — same EVM account, so the
         * one the operator stored works on both networks. */
        const bool have = settings_has_payout(NETS[i].net == UI_NET_TRON);
        lv_coord_t y = static_cast<lv_coord_t>(22 + (i * (PILL_H + 2)));
        /* "No payout address" measured 133px against this pill's
         * PICK_W - PILL_TEXT_X - PILL_TEXT_PAD_R = 130px cap, so the one row
         * that explains why a network is greyed out arrived elided. */
        lv_obj_t  *p = make_pill(card, NETS[i].name,
                                 have ? NETS[i].sub : "No payout set",
                                 PICK_W, y, NETS[i].act);
        lv_obj_align(make_net_badge(p, NETS[i].net),
                     LV_ALIGN_LEFT_MID, PILL_ICON_X, 0);
        if (!have) { pill_disable(p); }
    }

    make_button(card, "Cancel", COL_SURFACE, COL_TEXT, PICK_W, 40,
                LV_ALIGN_BOTTOM_MID, 0, -2, ACT_MODAL_CLOSE,
                &lv_font_montserrat_20);
}

/** Step 2 — the coin on the network chosen in step 1. */
static void open_coin_picker(ui_net_t net) {
    /* One row type and one table per network, picked below — a third network was
     * what made the pair of `tron ? A[i].x : B[i].x` lines untenable. No action
     * column: the chain is the action (ACT_CHAIN_OF). */
    typedef struct {
        const char *name;
        const char *sub;
        pos_chain_t chain;
    } coin_t;
    /* Native coin first on every network, as Tron has always listed TRX. */
    static const coin_t ETH_COINS[] = {
        { "ETH",  "Native coin", POS_CHAIN_ETH_NATIVE  },
        { "USDC", "ERC-20",      POS_CHAIN_ETH_SEPOLIA },
        { "USDT", "ERC-20",      POS_CHAIN_ETH_USDT    },
    };
    static const coin_t POLY_COINS[] = {
        { "POL",  "Native coin", POS_CHAIN_POLY_NATIVE },
        { "USDC", "ERC-20",      POS_CHAIN_POLY_USDC   },
        { "USDT", "ERC-20",      POS_CHAIN_POLY_USDT   },
    };
    static const coin_t TRON_COINS[] = {
        { "TRX",  "Native coin", POS_CHAIN_TRON_NILE },
        { "USDT", "TRC-20",      POS_CHAIN_TRON_USDT },
        { "USDC", "TRC-20",      POS_CHAIN_TRON_USDC },
    };

    const coin_t *coins;
    size_t        n;
    const char   *title;
    switch (net) {
        case UI_NET_TRON:
            coins = TRON_COINS;
            n     = sizeof(TRON_COINS) / sizeof(TRON_COINS[0]);
            title = "Coin on Tron";
            break;
        case UI_NET_POLY:
            coins = POLY_COINS;
            n     = sizeof(POLY_COINS) / sizeof(POLY_COINS[0]);
            title = "Coin on Polygon";
            break;
        case UI_NET_ETH:
        default:
            coins = ETH_COINS;
            n     = sizeof(ETH_COINS) / sizeof(ETH_COINS[0]);
            title = "Coin on Ethereum";
            break;
    }

    /* Card grows with the row count: header + rows + the Back button. */
    lv_obj_t *card = open_modal(228,
        static_cast<lv_coord_t>(86 + (n * (PILL_H + 2))));

    make_label(card, title, COL_DIM,
               &lv_font_montserrat_14, LV_ALIGN_TOP_MID, 0, 2);

    for (size_t i = 0; i < n; i++) {
        lv_coord_t y = static_cast<lv_coord_t>(22 + (i * (PILL_H + 2)));
        lv_obj_t  *p = make_pill(card, coins[i].name, coins[i].sub, PICK_W, y,
                                 ACT_CHAIN_OF(coins[i].chain), true /* leaf */);
        lv_obj_align(make_asset_badge(p, coins[i].chain),
                     LV_ALIGN_LEFT_MID, PILL_ICON_X, 0);
    }

    /* Back, not Cancel: step 2 of two, so the way out is step 1. */
    make_button(card, "Back", COL_SURFACE, COL_TEXT, PICK_W, 40,
                LV_ALIGN_BOTTOM_MID, 0, -2, ACT_NET_PICK,
                &lv_font_montserrat_20);
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

/**
 * @brief Screen title, centred but never underneath the top-left icon button.
 *
 * A centred 20px title and a hard-left icon button are two independent claims on
 * the same row, and on a 240px screen the wide ones collide: "Authorise browser"
 * measures 187px, so centred it starts at x=26 and the back arrow — which ends at
 * x=46 — was drawn straight through its first two characters.
 *
 * So the title gets the gap between the two gutters and nothing more. If it does
 * not fit at 20px it drops to the 14px face, where "Authorise browser" is 131px
 * and fits with room to spare. Shrinking beats both alternatives: overlapping is
 * the bug being fixed, and dot-eliding a title on a screen whose entire job is to
 * say what it wants is worse than a smaller one. The width cap and LONG_DOT stay
 * on as the backstop for a title too long even at 14px.
 *
 * @param has_icon true when make_icon_button() has put a glyph in the top-left.
 */
static lv_obj_t *make_title(const char *txt, bool has_icon) {
    /* Symmetric, so the title stays optically centred on the screen rather than
     * centred in the leftover space beside the button. */
    const lv_coord_t gutter = has_icon ? (MENU_BTN_X + MENU_BTN_W + 4) : 8;
    const lv_coord_t avail  = SCR_W - (2 * gutter);

    const bool small = lv_txt_get_width(txt, strlen(txt), &lv_font_montserrat_20,
                                        0, LV_TEXT_FLAG_NONE) > avail;
    /* +4 keeps the shorter 14px cap optically level with the 20px arrow glyph
     * beside it, which is drawn from the same HDR_TITLE_Y baseline. */
    lv_obj_t *l = make_label(lv_scr_act(), txt, COL_TITLE,
                             small ? &lv_font_montserrat_14
                                   : &lv_font_montserrat_20,
                             LV_ALIGN_TOP_MID, 0,
                             small ? (HDR_TITLE_Y + 4) : HDR_TITLE_Y);
    lv_obj_set_width(l, avail);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    return l;
}

/* Identical header on every screen: eyebrow title + rule + burger menu. */
/* Title + divider only — the burger (settings) lives solely on the amount
 * screen so settings can't be opened mid-transaction. */
static void build_header(const char *title) {
    (void)make_title(title, false);
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
    lv_obj_t *sub = make_label(lv_scr_act(), s_welcome_sub,
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

/* Phone setup: the same screen at every step, captioned with the current one.
 *
 * One QR code, not two. It carries "WIFI:T:WPA;S:...;P:...;;", which both iOS
 * and Android cameras join a network from directly — and the captive portal then
 * opens the form by itself, so there is never a URL to scan or type. The SSID
 * and passphrase are printed underneath for the camera that will not play along.
 * All three are for joining, so all three go away once a browser has been let in:
 * a code still sitting there on the payout step reads as "scan this again".
 *
 * There is deliberately no "use this screen instead" button any more. Past the
 * admin code the wizard is a browser flow: typing a payout address or a venue
 * passphrase on a resistive 240x320 panel is the thing this module exists to
 * avoid, and keeping a second, worse path meant maintaining two of everything.
 * The one step that stays on the panel is the admin code, because that is the step
 * whose entire value is that it never crosses a network.
 *
 * The last step is not a form at all: it thanks the operator and offers Finish,
 * which is what applies the settings (see main). */
static void build_prov(void) {
    clear_screen();
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_white(), LV_PART_MAIN);

    const prov_step_t step = static_cast<prov_step_t>(s_prov_step);

    /* The end of the wizard gets the whole screen: nothing left to scan, and the
     * one thing to say is that it worked. */
    if (step == PROV_STEP_DONE) {
        lv_obj_t *chk = make_label(lv_scr_act(), LV_SYMBOL_OK, COL_SUCCESS,
                                   &lv_font_montserrat_48, LV_ALIGN_TOP_MID, 0, 76);
        pop_in(chk);
        make_label(lv_scr_act(), "All set", COL_TEXT, &lv_font_montserrat_20,
                   LV_ALIGN_TOP_MID, 0, 142);
        lv_obj_t *b = make_label(lv_scr_act(),
                                 "Thank you - your terminal is configured.\n\n"
                                 "It restarts once to apply everything.",
                                 COL_DIM, &lv_font_montserrat_14,
                                 LV_ALIGN_TOP_MID, 0, 176);
        lv_obj_set_width(b, SCR_W - 32);
        lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 176);

        (void)make_button(lv_scr_act(), "Finish", COL_ACCENT, COL_BG,
                          SCR_W - 24, ACT_BTN_H, LV_ALIGN_BOTTOM_MID, 0, -10,
                          ACT_PROV_FINISH, &lv_font_montserrat_20);
        return;
    }

    /* The title names the step, and it is the biggest thing on the screen. An
     * earlier cut put "Set up from your phone" here for every step and moved
     * only a line of 14px grey caption between them — which made a step
     * advancing indistinguishable from nothing happening, since the QR code and
     * the credentials below are identical throughout.
     *
     * The step numbers count the whole flow, panel steps included: 1 is the admin
     * code the operator has just created on this screen, so "Step 2" here lines up
     * with what they have actually done rather than with this module's own idea of
     * where the flow starts. */
    /* The Wi-Fi-only re-join has no numbers to count — there was no code to create
     * and no address to set — so it says what it is instead. */
    const bool  numbered = !prov_wifi_only();
    const char *eyebrow  = "Setup";
    const char *title    = "Nothing to set";
    /* Each hint describes THIS screen, not the one after it. */
    const char *hint     = "";
    switch (step) {
        case PROV_STEP_AUTH:
            eyebrow = "Step 2";
            title   = "Scan with your phone";
            hint    = "Point your camera at the code";
            break;
        case PROV_STEP_ADDR:
            eyebrow = "Step 4";
            title   = "Payout addresses";
            hint    = "Continue on your phone";
            break;
        case PROV_STEP_WIFI:
            eyebrow = numbered ? "Step 5" : "Wi-Fi";
            title   = "Wi-Fi network";
            hint    = numbered ? "Continue on your phone"
                               : "Scan with your phone";
            break;
        default:
            break;
    }

    make_label(lv_scr_act(), eyebrow, COL_DIM, &lv_font_montserrat_14,
               LV_ALIGN_TOP_MID, 0, 6);
    make_label(lv_scr_act(), title, COL_TEXT, &lv_font_montserrat_20,
               LV_ALIGN_TOP_MID, 0, 22);
    make_label(lv_scr_act(), hint, COL_DIM,
               &lv_font_montserrat_14, LV_ALIGN_TOP_MID, 0, 48);

    /* The QR code and the AP credentials are for joining, so they go away the
     * moment the phone has joined and been let in. Left up on the later steps they
     * read as "scan this again", which is the one thing that cannot help: the
     * operator is looking for the payout form, not for a network. */
    if (!prov_authed()) {
        lv_obj_t *qr = lv_qrcode_create(lv_scr_act(), 112, COL_TEXT, COL_BG);
        if (qr != NULL) {
            const char *payload = prov_qr_payload();
            (void)lv_qrcode_update(qr, payload, strlen(payload));
            lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 70);
            /* White quiet zone: a code drawn hard against a coloured edge is a code
             * half the scanners in the world will not see. */
            lv_obj_set_style_border_color(qr, COL_BG, LV_PART_MAIN);
            lv_obj_set_style_border_width(qr, 4, LV_PART_MAIN);
        }

        /* Labelled, both of them: the bare SSID over a "Pass" line read as a title
         * and a note, and somebody typing them into a phone's Wi-Fi sheet is
         * filling in two named boxes. */
        char line[64];
        (void)snprintf(line, sizeof(line), "SSID: %s", prov_ap_ssid());
        make_label(lv_scr_act(), line, COL_TEXT, &lv_font_montserrat_14,
                   LV_ALIGN_TOP_MID, 0, 194);
        (void)snprintf(line, sizeof(line), "Password: %s", prov_ap_pass());
        make_label(lv_scr_act(), line, COL_DIM, &lv_font_montserrat_14,
                   LV_ALIGN_TOP_MID, 0, 212);
    } else {
        lv_obj_t *on = make_label(lv_scr_act(),
                                  "Connected - the setup page is open in the "
                                  "browser.",
                                  COL_DIM, &lv_font_montserrat_14,
                                  LV_ALIGN_TOP_MID, 0, 110);
        lv_obj_set_width(on, SCR_W - 40);
        lv_label_set_long_mode(on, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(on, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(on, LV_ALIGN_TOP_MID, 0, 110);
    }

    /* Where the bottom button used to be. A spinner instead: past the QR code the
     * operator is meant to be looking at the browser they joined from, and this is
     * the only thing on the panel that says the terminal is still with them.
     *
     * "a connection", not "your phone": the AP takes a laptop typing the SSID and
     * passphrase just as happily as a camera that scanned them, and a panel naming
     * the wrong device reads as "this will not work from here". */
    lv_obj_t *sp = lv_spinner_create(lv_scr_act(), 1000, 60);
    lv_obj_set_size(sp, 24, 24);
    lv_obj_align(sp, LV_ALIGN_BOTTOM_MID, 0, -34);
    lv_obj_set_style_arc_width(sp, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_width(sp, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(sp, COL_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_arc_color(sp, COL_ACCENT, LV_PART_INDICATOR);
    make_label(lv_scr_act(), "Waiting for a connection", COL_DIM,
               &lv_font_montserrat_14, LV_ALIGN_BOTTOM_MID, 0, -10);
}

/* A payout address or a token contract proposed from a browser, shown for
 * acceptance here. The modal is the security boundary, not decoration: a browser
 * can propose a value, only the panel in the operator's hands can store one — and
 * that holds for the card-derived route too, which comes through the same
 * handshake rather than writing straight to NVS.
 *
 * The value is drawn wrapped at 14 px rather than elided — the operator is being
 * asked to compare it against their own records character by character, so every
 * character has to be on the screen. */
static void build_prov_confirm(void) {
    prov_ask_t kind = PROV_ASK_NONE;
    char label[40] = "";
    char addr[SETTINGS_PAYOUT_MAX] = "";
    if (!prov_pending(&kind, label, sizeof(label), addr, sizeof(addr))) { return; }

    const bool contract = (kind == PROV_ASK_CONTRACT_ETH) ||
                          (kind == PROV_ASK_CONTRACT_TRON);

    lv_obj_t *card = open_modal(228, 276);

    make_label(card, contract ? "Set token contract?" : "Set payout address?",
               COL_TEXT, &lv_font_montserrat_20, LV_ALIGN_TOP_MID, 0, 2);
    /* Elided by width: the labels run to "TRC-20 token contract", which is longer
     * than the 16-character network name this row used to carry. */
    lv_obj_t *l = make_label(card, label, COL_DIM, &lv_font_montserrat_14,
                             LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_width(l, 204);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 28);

    lv_obj_t *a = make_label(card, addr, COL_TEXT, &lv_font_montserrat_14,
                             LV_ALIGN_TOP_LEFT, 4, 50);
    lv_obj_set_width(a, 204);
    lv_label_set_long_mode(a, LV_LABEL_LONG_WRAP);

    lv_obj_t *warn = make_label(card,
                                contract
                                ? "This decides which token is charged. A wrong "
                                  "contract moves a different asset."
                                : "Takings will be sent here. Check it against "
                                  "your own records before accepting.",
                                COL_DIM, &lv_font_montserrat_14,
                                LV_ALIGN_TOP_LEFT, 4, 118);
    lv_obj_set_width(warn, 204);
    lv_label_set_long_mode(warn, LV_LABEL_LONG_WRAP);

    /* Reject is the wide, plainly-labelled one and Accept is the deliberate tap:
     * the safe answer to "a stranger's address appeared on my terminal" is no. */
    (void)make_button(card, "Accept", COL_ACCENT, COL_BG, 206, 40,
                      LV_ALIGN_BOTTOM_MID, 0, -46, ACT_PROV_OK,
                      &lv_font_montserrat_20);
    (void)make_button(card, "Reject", COL_SURFACE, COL_TEXT, 206, 40,
                      LV_ALIGN_BOTTOM_MID, 0, -2, ACT_PROV_NO,
                      &lv_font_montserrat_20);
}

/* "Hold your card to the reader" while an address is derived from it. Its own
 * screen rather than the transaction one: nothing is being paid, and that screen's
 * wording and its Cancel semantics both belong to a sale. */
static void build_card_wait(void) {
    clear_screen();
    build_header("Cryptnox card");

    make_tap_mark(64);

    make_label(lv_scr_act(), "Hold card to reader", COL_TEXT,
               &lv_font_montserrat_20, LV_ALIGN_TOP_MID, 0, 172);

    lv_obj_t *info = make_label(lv_scr_act(), s_tx_info, COL_DIM,
                                &lv_font_montserrat_14, LV_ALIGN_TOP_MID, 0, 204);
    lv_obj_set_width(info, 216);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(info, LV_ALIGN_TOP_MID, 0, 204);

    make_button(lv_scr_act(), "Cancel", COL_SURFACE, COL_TEXT, 232, ACT_BTN_H,
                LV_ALIGN_BOTTOM_MID, 0, ACT_BTN_Y, ACT_CANCEL,
                &lv_font_montserrat_20);
}

static void build_splash(void) {
    clear_screen();
    /* The logo is black-on-white; put the whole splash on white so it blends. */
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_white(), LV_PART_MAIN);

    lv_obj_t *logo = lv_img_create(lv_scr_act());
    lv_img_set_src(logo, &logo_img);
    lv_obj_align(logo, LV_ALIGN_CENTER, LOGO_X_NUDGE, -36);
    /* No pop_in() here: at power-on the backlight has only just come up, so a
     * scale-in reads as the logo blinking. Kept for the tx check/cross. */

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

    /* Which boot step is running, so a slow start is legible rather than
     * looking frozen. Discreet, and elided by width to never overflow. */
    s_boot_step_lbl = make_label(lv_scr_act(), s_boot_step, COL_DIM,
                                 &lv_font_montserrat_14,
                                 LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_width(s_boot_step_lbl, SCR_W - 24);
    lv_label_set_long_mode(s_boot_step_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_boot_step_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(s_boot_step_lbl, LV_ALIGN_BOTTOM_MID, 0, -20);
}

static void build_amount(void) {
    clear_screen();
    /* No title/divider here — the keypad needs the vertical space. Keep just
     * the burger (settings) top-left. */
    add_menu_button();

    /* One row at 47..89: the amount centred on the screen, the asset selector at
     * its right-hand end. Taking money means reading a figure and a currency
     * together, and a selector centred on its own line above the figure was a
     * heading for it instead.
     *
     * The figure carries its ticker (see amount_update_display), so the group is
     * centred in the line minus the pill rather than on the screen — and it is
     * re-centred whenever either changes, see amount_row_place(). At the widest it
     * will ever be — 99999 in montserrat_28, small cents and " USDT", ~165px — it
     * still stops short of the narrowed pill, and the pill is only wide while the
     * amount is "0.00" and the ticker is blank.
     *
     * The 320px column is fully spoken for: keypad 90..266 and the Charge button
     * 266..312 below it, so the amount's row is paid for out of the keypad's
     * height (196 -> 176, keys 44 tall — still a comfortable target). */
    make_asset_button();

    /* Figure, small cents and ticker in one content-sized flex row, so the group
     * centres itself whatever the fonts measure — nothing here has to know how
     * wide montserrat_28 draws a 5, and the ticker appearing does not shove the
     * digits off centre by hand. Bottom-aligned across the row, which puts the
     * small cents on the big font's baseline, near enough (its descender is a pixel
     * or two, and a real baseline align is not on offer in LVGL 8). The ticker opts
     * out of that a few lines below. */
    lv_obj_t *row = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER);
    s_amount_row = row;   /* placed by amount_update_display below */

    s_amount_label = make_label(row, "0.00", COL_TEXT,
                                &lv_font_montserrat_28, LV_ALIGN_DEFAULT, 0, 0);
    s_amount_cents_label = make_label(row, "", COL_TEXT,
                                      &lv_font_montserrat_20, LV_ALIGN_DEFAULT, 0, 0);
    s_amount_ticker_label = make_label(row, "", COL_TEXT,
                                       &lv_font_montserrat_20, LV_ALIGN_DEFAULT, 0, 0);

    /* The ticker on the figure's centre line, which is what "USDC" beside a price
     * wants: its 22px box is 8px shorter than the figure's 30px one, and the row
     * bottom-aligns everything, so it was drawn 4px low. Drawn back up by half the
     * difference — the small cents stay where they are, because those belong to the
     * number and a cent off the figure's baseline is a typo, not a unit.
     *
     * translate, not padding or a cross-align of its own: it moves the glyphs
     * without touching what the row measures, so the fit arithmetic in
     * amount_row_place() still sees the real width. Taken from the fonts rather
     * than written as 4, so it follows a change of either. */
    lv_obj_set_style_translate_y(
        s_amount_ticker_label,
        -(lv_coord_t)((lv_font_montserrat_28.line_height -
                       lv_font_montserrat_20.line_height) / 2),
        LV_PART_MAIN);

    /* Numeric keypad: digits, double-zero, backspace (cents entry). */
    static const char *amap[] = {
        "1", "2", "3", "\n",
        "4", "5", "6", "\n",
        "7", "8", "9", "\n",
        "00", "0", LV_SYMBOL_BACKSPACE, ""
    };
    lv_obj_t *kb = lv_btnmatrix_create(lv_scr_act());
    lv_btnmatrix_set_map(kb, amap);
    lv_obj_set_size(kb, 232, 176);
    lv_obj_align(kb, LV_ALIGN_TOP_MID, 0, 90);
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
    lv_obj_t *cusdc = make_asset_badge(lv_scr_act(), settings_get_chain());
    lv_obj_align_to(cusdc, amt, LV_ALIGN_OUT_RIGHT_MID, 8, 0);

    make_label(lv_scr_act(), "To", COL_DIM, &lv_font_montserrat_14,
               LV_ALIGN_TOP_LEFT, 12, 118);
    lv_obj_t *addr = make_label(lv_scr_act(),
                                s_confirm_addr[0] ? s_confirm_addr : "-",
                                COL_TEXT, &lv_font_montserrat_14,
                                LV_ALIGN_TOP_LEFT, 12, 136);
    lv_label_set_long_mode(addr, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(addr, 216);

    make_label(lv_scr_act(), asset_caption(),
               COL_DIM, &lv_font_montserrat_14, LV_ALIGN_TOP_LEFT, 12, 182);
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

    /* Same keypad, two errands. A card read is not a payment: it gets the card
     * screen and its own event, so main cannot mistake it for a sale and the tx
     * screen's "Declined" wording never appears over a setup step. */
    if (s_pin_for_card) {
        s_pin_for_card = false;
        strncpy(s_tx_info, "Reading your payout addresses",
                sizeof(s_tx_info) - 1);
        s_tx_info[sizeof(s_tx_info) - 1] = '\0';
        request_screen(UI_SCREEN_CARD_WAIT);
        if (s_cb != NULL) { s_cb(UI_EVENT_CARD_PIN, 0); }
        return;
    }

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
    /* Echo each digit briefly so the operator can confirm the keypress, then
     * mask — a third of LVGL's 1500 ms default, which leaks the whole code. */
    lv_textarea_set_password_show_time(ta, 500);
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

    (void)make_title(s_pin_for_card ? "Card PIN" : "Enter PIN", true);
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
            /* Not the amount screen: this is the first-run path, so main answers by
             * raising the setup access point, which takes a second or two — the
             * payment keypad would flash up and be replaced by the setup screen.
             * Say what is actually happening instead. */
            set_wifi_progress("Starting setup", NULL);
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
            if (s_admin_for_portal) {
                /* This is the whole authorisation mechanism: the code was typed
                 * here, so the browser is let in without it ever having been on
                 * the network.
                 *
                 * Where to go back to differs by mode, and the setup screen is
                 * wrong for admin mode — it would draw a QR code beside an AP name
                 * and passphrase that only exist during setup. The code was just
                 * verified, so the settings page is a legitimate landing spot and
                 * the one the operator came from. */
                s_admin_for_portal = false;
                prov_auth_resolve(true);
                request_screen((prov_mode() == PROV_MODE_WIZARD)
                               ? UI_SCREEN_PROV : UI_SCREEN_SETTINGS);
            } else {
                request_screen(UI_SCREEN_SETTINGS);
            }
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
    (void)make_title(title, allow_cancel);
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
    /* Same screen either way — the escalating lockout, the note band and the
     * keypad are what this is, and only the door it opens differs. */
    build_admin_screen(s_admin_for_portal ? "Authorise browser" : "Admin code",
                       true);
}

/* Header with a back arrow (to amount entry) instead of the burger. */
static void build_header_back(const char *title) {
    (void)make_title(title, true);
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

    /* Why the picker opened, otherwise the operator lands in Wi-Fi setup with no
     * idea what failed. */
    lv_coord_t list_y = 48;
    if (s_wifi_note[0] != '\0') {
        lv_obj_t *note = make_label(lv_scr_act(), s_wifi_note, COL_DANGER,
                                    &lv_font_montserrat_14,
                                    LV_ALIGN_TOP_MID, 0, 50);
        lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(note, 216);
        lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        /* Measure rather than assume two lines: the list then clears the note at
         * any wrap depth, so a longer message can never overprint it. */
        lv_obj_update_layout(note);
        list_y = 50 + lv_obj_get_height(note) + 6;
    }

    if (s_ap_count == 0U) {
        make_label(lv_scr_act(), "No networks found", COL_DIM,
                   &lv_font_montserrat_14, LV_ALIGN_CENTER, 0, 0);
        make_button(lv_scr_act(), "Rescan", COL_ACCENT, COL_BG, 140, ACT_BTN_H,
                    LV_ALIGN_BOTTOM_MID, 0, ACT_BTN_Y, ACT_WIFI,
                    &lv_font_montserrat_20);
        return;
    }

    lv_obj_t *list = lv_list_create(lv_scr_act());
    lv_obj_set_size(list, SCR_W - 12, SCR_H - 4 - list_y);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, list_y);
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
        set_wifi_progress("Connecting to", s_wifi_ssid);
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
    /* Echo each character briefly, then mask — long enough to catch a typo,
     * a third of LVGL's 1500 ms default on a customer-facing screen. The eye
     * stays the way to re-read the whole passphrase, on the merchant's terms. */
    lv_textarea_set_password_show_time(s_wifi_pass_ta, 500);
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

    if (s_wifi_name[0] == '\0') {
        /* Nothing to name ("Scanning..."): the caption is the whole message. */
        make_label(lv_scr_act(), s_wifi_caption, COL_TEXT,
                   &lv_font_montserrat_20, LV_ALIGN_CENTER, 0, 0);
    } else {
        make_label(lv_scr_act(), s_wifi_caption, COL_DIM,
                   &lv_font_montserrat_14, LV_ALIGN_CENTER, 0, -30);

        /* LONG_DOT elides on real glyph metrics, so a 32-char SSID never
         * overflows. Width must be set before the long mode. */
        lv_obj_t *name = make_label(lv_scr_act(), s_wifi_name, COL_TEXT,
                                    &lv_font_montserrat_20, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_width(name, SCR_W - 24);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(name, LV_ALIGN_CENTER, 0, 0);   /* re-centre after the resize */
    }
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
    /* Apply the start scale before the first render: lv_anim_start() only calls
     * the exec cb on its first tick, so the object would flash at full size. */
    zoom_anim_cb(obj, 10);
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
    lv_obj_t *u  = make_asset_badge(lv_scr_act(), settings_get_chain());
    lv_obj_align_to(u, al, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
}

static void build_tx_status(void) {
    clear_screen();
    build_header("Transaction");

    char amt[24];
    format_amount(s_confirm_amount, amt, sizeof(amt));

    if (s_tx_state == UI_TX_STATE_PLACE_CARD) {
        make_tap_mark(64);

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

/* Startup fault. Not the transaction screen: its red cross and "Declined" made
 * a wiring problem read as a refused sale. No action button — nothing here is
 * recoverable from the touchscreen, so the body text says what to do. */
static void build_boot_error(void) {
    clear_screen();
    build_header("Startup");

    lv_obj_t *warn = make_label(lv_scr_act(), LV_SYMBOL_WARNING, COL_DANGER,
                                &lv_font_montserrat_48, LV_ALIGN_TOP_MID, 0, 62);
    pop_in(warn);

    const char *title;
    const char *body;
    switch (s_boot_err) {
        case UI_BOOT_ERR_WALLET:
            title = "Wallet not ready";
            body  = "The card reader answered but the Cryptnox wallet could not "
                    "be initialised.\n\n"
                    "Restart the terminal. If this keeps happening, the reader "
                    "or its firmware is at fault.";
            break;
        case UI_BOOT_ERR_NFC:
        default:
            title = "NFC reader not found";
            body  = "The PN532 module did not answer on the I2C bus.\n\n"
                    "Check the SDA/SCL wiring, the RST pin and the 3V3 supply, "
                    "then restart the terminal.";
            break;
    }

    make_label(lv_scr_act(), title, COL_TEXT, &lv_font_montserrat_20,
               LV_ALIGN_TOP_MID, 0, 124);

    lv_obj_t *b = make_label(lv_scr_act(), body, COL_DIM,
                             &lv_font_montserrat_14, LV_ALIGN_TOP_MID, 0, 158);
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(b, 216);
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    /* Technician detail, kept off the main message so the operator reads the
     * instruction and not the error code. Sits at the foot of the screen, but
     * never above the body: anchoring to the body instead of the screen edge
     * means a long instruction or a long esp_err name clips at the bottom
     * rather than overprinting the text the operator has to act on. */
    if (s_boot_detail[0] != '\0') {
        lv_obj_t *d = make_label(lv_scr_act(), s_boot_detail, COL_DIM,
                                 &lv_font_montserrat_14,
                                 LV_ALIGN_BOTTOM_MID, 0, -8);
        lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(d, 216);
        lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

        lv_obj_update_layout(b);
        lv_obj_update_layout(d);
        const lv_coord_t body_end = lv_obj_get_y(b) + lv_obj_get_height(b) + 8;
        if (lv_obj_get_y(d) < body_end) {
            lv_obj_align(d, LV_ALIGN_TOP_MID, 0, body_end);
        }
    }
}

static void render_requested_screen(void) {
    /* A modal lives on the top layer, so it would survive the screen swap and
     * sit there swallowing every touch. Nothing wants that. */
    close_modal();

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
        case UI_SCREEN_BOOT_ERROR:   build_boot_error();   break;
        case UI_SCREEN_ADMIN_SET:    build_admin_set();    break;
        case UI_SCREEN_ADMIN_UNLOCK: build_admin_unlock(); break;
        case UI_SCREEN_WELCOME:      build_welcome();      break;
        case UI_SCREEN_PROV:         build_prov();         break;
        case UI_SCREEN_CARD_WAIT:    build_card_wait();    break;
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

    /* White, not black: the backlight comes on below, before LVGL's first
     * frame, and the theme is white — black made power-on flash. */
    tft.fillScreen(TFT_WHITE);

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
        /* Boot-status update from the main task; targeted rather than a splash
         * rebuild, which would restart the logo on every step. */
        if (s_boot_step_dirty) {
            s_boot_step_dirty = false;
            if ((s_req_screen == UI_SCREEN_SPLASH) && (s_boot_step_lbl != NULL)) {
                lv_label_set_text(s_boot_step_lbl, s_boot_step);
            }
        }
        /* A value proposed from the config page. Built here, on the UI task, and
         * after the screen render above — a modal raised straight from the main or
         * HTTP task would be touching LVGL from two tasks at once. */
        if (s_addr_modal_dirty) {
            s_addr_modal_dirty = false;
            build_prov_confirm();
        }
        /* Same handoff for firmware uploaded from the config page — it replaces
         * the "browser at this address" card the operator is looking at. */
        if (s_ota_modal_dirty) {
            s_ota_modal_dirty = false;
            build_ota_confirm();
        }
        /* The admin page closes on its own deadline, and the check has to run
         * somewhere that may touch LVGL — so it runs here rather than in a timer
         * task. Deliberately NOT gated on the card still being up: the operator may
         * have left the modal (to enter the admin code, say), and a config server
         * that outlives its window because nobody was looking at a card is the
         * thing the window exists to prevent. Wizard mode has no deadline
         * (prov_window_left_min() is 0 there), hence the mode test. */
        /* ...and not while a firmware image is actually arriving. Stopping httpd
         * mid-transfer drops the socket, ota_abort() throws away what was written,
         * and the operator gets "the connection to the terminal dropped" after
         * three minutes of progress bar. This cannot hold the page open for ever:
         * ota_post() gives up on a socket that has gone quiet (UPLOAD_MAX_STALLS). */
        if ((prov_mode() == PROV_MODE_ADMIN) && (prov_window_left_min() == 0U) &&
            !ota_receiving()) {
            prov_stop();
            /* ...but NOT the firmware card. s_portal_modal is set by that card too
             * (build_ota_confirm), so this used to close the Install/Discard
             * decision the operator was in the middle of — on a deadline that had
             * been ticking through a multi-minute upload. The window is about the
             * config *page*, which is now down; an image already verified and
             * waiting on this screen needs no page to install. */
            if (s_portal_modal && !ota_staged(NULL, 0U, NULL)) { close_modal(); }
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

extern "C" void ui_show_wifi_list(const net_wifi_ap_t *aps, uint16_t n,
                                  const char *note) {
    s_ap_count = (n > WIFI_MAX_APS) ? WIFI_MAX_APS : n;
    for (uint16_t i = 0U; i < s_ap_count; i++) {
        s_aps[i] = aps[i];
    }
    /* Set on every call, so an earlier failure's note cannot linger. */
    if (note != NULL) {
        strncpy(s_wifi_note, note, sizeof(s_wifi_note) - 1);
        s_wifi_note[sizeof(s_wifi_note) - 1] = '\0';
    } else {
        s_wifi_note[0] = '\0';
    }
    request_screen(UI_SCREEN_WIFI_LIST);
}

extern "C" void ui_set_addresses(const char *token_contract, const char *dest_addr) {
    s_addr_usdc = token_contract;
    s_addr_dest = dest_addr;
}

extern "C" void ui_show_wifi_connecting(const char *ssid) {
    set_wifi_progress("Connecting to", ssid);
    request_screen(UI_SCREEN_WIFI_CONNECTING);
}

extern "C" void ui_set_boot_status(const char *step) {
    strncpy(s_boot_step, (step != NULL) ? step : "", sizeof(s_boot_step) - 1);
    s_boot_step[sizeof(s_boot_step) - 1] = '\0';
    s_boot_step_dirty = true;   /* applied by the UI task — LVGL is single-thread */
}

extern "C" void ui_show_boot_error(ui_boot_err_t kind, const char *detail) {
    s_boot_err = kind;
    if (detail != NULL) {
        strncpy(s_boot_detail, detail, sizeof(s_boot_detail) - 1);
        s_boot_detail[sizeof(s_boot_detail) - 1] = '\0';
    } else {
        s_boot_detail[0] = '\0';
    }
    request_screen(UI_SCREEN_BOOT_ERROR);
}

extern "C" void ui_show_welcome(const char *sub) {
    strncpy(s_welcome_sub,
            ((sub != NULL) && (sub[0] != '\0')) ? sub
                                                : "Let's configure your terminal.",
            sizeof(s_welcome_sub) - 1U);
    s_welcome_sub[sizeof(s_welcome_sub) - 1U] = '\0';
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

extern "C" void ui_stage_wifi_creds(const char *ssid, const char *pass) {
    strncpy(s_wifi_ssid, (ssid != NULL) ? ssid : "", sizeof(s_wifi_ssid) - 1U);
    s_wifi_ssid[sizeof(s_wifi_ssid) - 1U] = '\0';
    strncpy(s_wifi_pass, (pass != NULL) ? pass : "", sizeof(s_wifi_pass) - 1U);
    s_wifi_pass[sizeof(s_wifi_pass) - 1U] = '\0';
}

extern "C" void ui_show_prov(int step) {
    s_prov_step = step;
    request_screen(UI_SCREEN_PROV);
}

extern "C" void ui_show_prov_confirm(void) {
    s_addr_modal_dirty = true;
}

extern "C" void ui_show_prov_auth(void) {
    s_admin_for_portal = true;
    s_admin_confirming = false;
    s_admin_note[0]    = '\0';
    /* Re-arm the wait from the persisted attempt count, exactly as the burger tap
     * does: this is the same code and the same guessing budget, so it must not be
     * a cheaper door than the menu. */
    s_admin_lock_ms    = admin_penalty_ms(settings_admin_fail_count());
    s_admin_lock_start = lv_tick_get();
    request_screen(UI_SCREEN_ADMIN_UNLOCK);
}

extern "C" void ui_show_card_pin(void) {
    s_pin_for_card = true;
    request_screen(UI_SCREEN_PIN);
}

extern "C" void ui_show_card_wait(const char *note) {
    strncpy(s_tx_info, (note != NULL) ? note : "", sizeof(s_tx_info) - 1);
    s_tx_info[sizeof(s_tx_info) - 1] = '\0';
    request_screen(UI_SCREEN_CARD_WAIT);
}

extern "C" void ui_show_ota_confirm(void) {
    s_ota_modal_dirty = true;
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
