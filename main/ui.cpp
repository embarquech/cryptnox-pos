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
#include <time.h>        /* the status-band clock */

#include "lvgl.h"
#include "logo_img.h"
#include "logo_small.h"
#include "chain_icons.h"
#include "tap_icon.h"    /* the "tap your card" mark — tools/gen_tap_icon.py */
#include "assets.h"      /* the per-asset table: ticker, standard, caption, network */
#include "settings.h"
#include "touch_cal.h"   /* two-point calibration arithmetic, host-tested */
#include "provision.h"   /* QR payload + the pending payout-address handshake */
#include "ota.h"         /* running version + the update window and its handshake */
#include "ota_version.h" /* ota_version_display() — the 'v' is added for the screen */

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
/* Slate, not black. Full black is what the flat 2010s "minimal" look does, and a
 * 44px slab of #000 under a grey-on-white card reads as a placeholder rather than
 * a product. Nothing here is invented: the hue is the one the setup portal and
 * the docs site already use, so the panel and the browser page a technician
 * opens beside it match.
 *
 * WHICH slate, though, is a panel decision and not a web one. The portal's light
 * ink #2C3E50 was tried here first and came out pale — black sits at a contrast
 * of 21:1 on white and that value at 11:1, and halving the contrast on a 2.8"
 * TFT whose blacks are already lifted and whose gamma is soft reads as washed
 * out rather than as a deep navy. This is the portal's DARK-scheme accent
 * (--accf) instead: same hue, 13.5:1, which holds up on the panel.
 *
 * So: pick from the portal palette, but pick the dark end of it. A colour that
 * looks right in a browser is not evidence about this display.
 *
 * Text ON it stays COL_BG — white on this is 13.5:1, well past AA. */
#define COL_ACCENT   lv_color_hex(0x22303D)   /* slate — primary action button */
/* Sale-flow page chrome. Deliberately NO new hex values: the card idiom is
 * built out of the palette above, so the terminal keeps the black-on-white it
 * always had. The page is the existing surface grey and the card is the
 * existing white, which is the whole of what separates them. */
/* The page behind the white card. COL_BORDER, arrived at by walking the whole
 * ramp and coming back:
 *
 *   0xF2 (COL_SURFACE) — 13 levels off white; through an 8px side margin it
 *                        came out as no visible card at all.
 *   0xE0 (COL_BORDER)  — this. A distinct ground without weight.
 *   0xC4               — darker, not better.
 *   0x3B / 0x24 / 0x52 — dark-bezel treatments. They separate the card hardest
 *                        and are closest to the reference design's value, but
 *                        they invert the whole page: see the step colours below.
 *   0x9A (COL_DIM)     — unusable at any brightness. The home indicator IS
 *                        COL_DIM, so a page at that value swallows the only
 *                        thing on screen advertising the swipe-up gesture.
 *
 * Anything drawn ON the page must be rechecked whenever this moves. That is the
 * entire history of this block, and it is why the values below are not
 * independent of the one above. */
#define COL_PAGE     COL_BORDER    /* light grey — page behind the card */
#define COL_STEP_ON  COL_ACCENT    /* black — the step you are on       */
/* White, NOT COL_BORDER: the spent dashes sit ON the page, so hairline-grey
 * dashes disappeared into their own background the moment the page became
 * COL_BORDER. On a dark page these two swap — lit goes white, spent goes
 * COL_DIM — because a black lit dash is invisible there and white spent dashes
 * shout as loudly as the lit one. */
#define COL_STEP_OFF COL_BG        /* white — the steps you are not on  */
#define COL_HOME_BAR COL_DIM       /* grey — the swipe handle           */
#define COL_SUCCESS  lv_color_hex(0x1E9E50)   /* green — "Sent"                */
#define COL_DANGER   lv_color_hex(0xD63A3A)   /* red — failures / reset        */
#define COL_BORDER   lv_color_hex(0xE0E0E0)   /* light grey — hairlines        */
#define COL_TRON     lv_color_hex(0xE7392E)   /* Tron red — TRX asset badge    */
#define COL_USDT     lv_color_hex(0x26A17B)   /* Tether green — USDT badge     */
#define COL_ETH      lv_color_hex(0x627EEA)   /* Ethereum periwinkle — net chip*/

/******************************************************************
 * 3a. LVGL theme — extends the built-in default rather than replacing it
 *
 * Everything this file draws by hand already speaks one language: full-radius
 * pills, hairline greys, no chrome. The widgets it does NOT style by hand came
 * out in LVGL's defaults instead, and the tab bar is the one people notice —
 * the default tabview is flat rectangular buttons with a hard indicator, which
 * is what made the admin section look older than the screens around it.
 *
 * So: a theme, parented to the default one, that restyles the tab bar as a
 * segmented control and thins the scrollbars. A theme rather than more
 * hand-styling because it applies at creation, to every tabview this firmware
 * ever adds, instead of to the one place somebody remembered to touch.
 *
 * Deliberately narrow. It does not repaint buttons, pills or cards — those are
 * already explicit at each call site, and a theme fighting an explicit style is
 * a look that changes depending on which one ran last.
 ******************************************************************/
#define TAB_SEG_INSET   5      /* pill inset inside the 42px bar, top+bottom */
#define TAB_SEG_GAP     4      /* gap between segments                       */
#define SCROLLBAR_W     4      /* hairline, not the default block            */

static lv_theme_t s_theme;
static lv_style_t s_st_tabbar;    /* the bar the segments sit on   */
static lv_style_t s_st_tab;       /* one segment, not selected     */
static lv_style_t s_st_tab_sel;   /* the selected segment          */
static lv_style_t s_st_tabview;   /* the tabview container itself  */
static lv_style_t s_st_scrollbar;
static lv_style_t s_st_track;     /* slider/arc/spinner background */
static lv_style_t s_st_ink;       /* their filled part and knob    */
static lv_style_t s_st_field;     /* text entry                    */
static lv_style_t s_st_key;       /* one key of the keyboard       */

static void theme_styles_init(void)
{
    /* The bar itself stays white: the selected pill is the only ink, which is
     * what keeps a four-tab bar from reading as a toolbar on a 320px screen. */
    lv_style_init(&s_st_tabbar);
    lv_style_set_bg_color(&s_st_tabbar, COL_BG);
    lv_style_set_bg_opa(&s_st_tabbar, LV_OPA_COVER);
    lv_style_set_border_width(&s_st_tabbar, 0);
    lv_style_set_pad_ver(&s_st_tabbar, TAB_SEG_INSET);
    lv_style_set_pad_hor(&s_st_tabbar, TAB_SEG_GAP);
    lv_style_set_pad_column(&s_st_tabbar, TAB_SEG_GAP);

    lv_style_init(&s_st_tab);
    lv_style_set_bg_opa(&s_st_tab, LV_OPA_TRANSP);
    lv_style_set_border_width(&s_st_tab, 0);
    lv_style_set_radius(&s_st_tab, LV_RADIUS_CIRCLE);
    lv_style_set_text_color(&s_st_tab, COL_DIM);
    lv_style_set_text_font(&s_st_tab, &lv_font_montserrat_14);

    /* Filled pill, same shape as the selector rows and the action buttons. */
    lv_style_init(&s_st_tab_sel);
    lv_style_set_bg_color(&s_st_tab_sel, COL_ACCENT);
    lv_style_set_bg_opa(&s_st_tab_sel, LV_OPA_COVER);
    lv_style_set_radius(&s_st_tab_sel, LV_RADIUS_CIRCLE);
    lv_style_set_text_color(&s_st_tab_sel, COL_BG);
    lv_style_set_border_width(&s_st_tab_sel, 0);

    lv_style_init(&s_st_tabview);
    lv_style_set_bg_color(&s_st_tabview, COL_BG);
    lv_style_set_bg_opa(&s_st_tabview, LV_OPA_COVER);
    lv_style_set_border_width(&s_st_tabview, 0);

    lv_style_init(&s_st_scrollbar);
    lv_style_set_bg_color(&s_st_scrollbar, COL_BORDER);
    lv_style_set_bg_opa(&s_st_scrollbar, LV_OPA_COVER);
    lv_style_set_radius(&s_st_scrollbar, LV_RADIUS_CIRCLE);
    lv_style_set_width(&s_st_scrollbar, SCROLLBAR_W);
    lv_style_set_pad_right(&s_st_scrollbar, 2);

    /* Colour and radius only, from here down. Nothing below sets a size, a pad
     * or a length: these widgets sit in layouts positioned by hand at their
     * call sites, and a theme that moves geometry breaks a screen that works. */
    lv_style_init(&s_st_track);
    lv_style_set_bg_color(&s_st_track, COL_BORDER);
    lv_style_set_bg_opa(&s_st_track, LV_OPA_COVER);
    lv_style_set_radius(&s_st_track, LV_RADIUS_CIRCLE);
    lv_style_set_arc_color(&s_st_track, COL_BORDER);

    lv_style_init(&s_st_ink);
    lv_style_set_bg_color(&s_st_ink, COL_ACCENT);
    lv_style_set_bg_opa(&s_st_ink, LV_OPA_COVER);
    lv_style_set_radius(&s_st_ink, LV_RADIUS_CIRCLE);
    lv_style_set_arc_color(&s_st_ink, COL_ACCENT);
    lv_style_set_border_width(&s_st_ink, 0);
    lv_style_set_shadow_width(&s_st_ink, 0);   /* software-rendered: not free */

    /* A field reads as a filled grey rounded box rather than a bordered one —
     * the same surface the pills and cards use. */
    lv_style_init(&s_st_field);
    lv_style_set_bg_color(&s_st_field, COL_SURFACE);
    lv_style_set_bg_opa(&s_st_field, LV_OPA_COVER);
    lv_style_set_radius(&s_st_field, 10);
    lv_style_set_border_width(&s_st_field, 0);
    lv_style_set_text_color(&s_st_field, COL_TEXT);

    /* Default LVGL keys are flat grey blocks butted together, which is the most
     * dated surface left. White rounded keys on the grey field read current. */
    lv_style_init(&s_st_key);
    lv_style_set_bg_color(&s_st_key, COL_BG);
    lv_style_set_bg_opa(&s_st_key, LV_OPA_COVER);
    lv_style_set_radius(&s_st_key, 8);
    lv_style_set_border_width(&s_st_key, 0);
    lv_style_set_text_color(&s_st_key, COL_TEXT);
}

static void theme_apply(lv_theme_t *th, lv_obj_t *obj)
{
    LV_UNUSED(th);

    lv_obj_add_style(obj, &s_st_scrollbar, LV_PART_SCROLLBAR);

    if (lv_obj_check_type(obj, &lv_tabview_class)) {
        lv_obj_add_style(obj, &s_st_tabview, LV_PART_MAIN);
        return;
    }

    /* The tab bar is a button matrix, and so is the PIN pad. The parent is what
     * tells them apart — without this check the keypad becomes a segmented
     * control too, which is a very confusing way to type a PIN. */
    if (lv_obj_check_type(obj, &lv_btnmatrix_class)) {
        lv_obj_t *parent = lv_obj_get_parent(obj);
        if ((parent != NULL) && lv_obj_check_type(parent, &lv_tabview_class)) {
            lv_obj_add_style(obj, &s_st_tabbar, LV_PART_MAIN);
            lv_obj_add_style(obj, &s_st_tab, LV_PART_ITEMS);
            lv_obj_add_style(obj, &s_st_tab_sel,
                             LV_PART_ITEMS | LV_STATE_CHECKED);
        }
        return;
    }

    if (lv_obj_check_type(obj, &lv_slider_class)) {
        lv_obj_add_style(obj, &s_st_track, LV_PART_MAIN);
        lv_obj_add_style(obj, &s_st_ink, LV_PART_INDICATOR);
        lv_obj_add_style(obj, &s_st_ink, LV_PART_KNOB);
    } else if (lv_obj_check_type(obj, &lv_arc_class)
               || lv_obj_check_type(obj, &lv_spinner_class)) {
        lv_obj_add_style(obj, &s_st_track, LV_PART_MAIN);
        lv_obj_add_style(obj, &s_st_ink, LV_PART_INDICATOR);
    } else if (lv_obj_check_type(obj, &lv_textarea_class)) {
        lv_obj_add_style(obj, &s_st_field, LV_PART_MAIN);
    } else if (lv_obj_check_type(obj, &lv_keyboard_class)) {
        lv_obj_add_style(obj, &s_st_field, LV_PART_MAIN);
        lv_obj_add_style(obj, &s_st_key, LV_PART_ITEMS);
    }
}

/**
 * @brief Install the theme. After lv_disp_drv_register(), before any object.
 *
 * Copied from the active theme and parented to it, so the default styling still
 * runs first and this only adds on top — the pattern LVGL documents for
 * extending a theme rather than writing one from nothing.
 */
static void theme_init(void)
{
    theme_styles_init();

    lv_theme_t *base = lv_disp_get_theme(NULL);
    s_theme = *base;
    lv_theme_set_parent(&s_theme, base);
    lv_theme_set_apply_cb(&s_theme, theme_apply);
    lv_disp_set_theme(NULL, &s_theme);
}

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
 * It carries the ticker, and the figure beside it is a bare number. The ticker
 * used to ride on the figure instead, which put the currency in two places at
 * once — a mark on the pill and a word on the amount — and neither said which one
 * you tap to change it. One says both now: what is being charged, and that it is
 * a control.
 *
 * No fixed width any more, because the ticker's is not fixed: the pill is
 * content-sized flex, and amount_row_place() measures what it came out as rather
 * than being told. Written as constants, a four-letter ticker in a pill sized for
 * three is a dot-elided "USD…" on the row that says which money. 42 tall is 3px of
 * air above and below the 36px badge (BADGE_SZ, with the icon helpers below). */
#define ASSET_BTN_H     42
/* Both in CARD coordinates — the amount screen builds into the white card. */
#define ASSET_BTN_X     (-8)   /* right edge inset                     */
#define ASSET_BTN_Y     10     /* the figure's row is centred on THIS  */
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

/* Raw XPT2046 counts at the panel's edges. Cached from NVS rather than read per
 * sample: indev_read runs every few milliseconds and is no place to open flash.
 * The defaults are the 200/3800 this file used to hardcode — right enough on
 * most CYDs to reach the calibration screen on a panel nobody has calibrated. */
static uint16_t s_cal_xmin = 200U, s_cal_xmax = 3800U;
static uint16_t s_cal_ymin = 200U, s_cal_ymax = 3800U;

static void touch_cal_load(void) {
    settings_get_touch_cal(&s_cal_xmin, &s_cal_xmax, &s_cal_ymin, &s_cal_ymax);
}

/* Uncalibrated sample, for the calibration screen itself — the only caller that
 * must not go through the mapping it is measuring. */
static bool touch_raw(int16_t *rx, int16_t *ry) {
    if (!touch.tirqTouched() || !touch.touched()) { return false; }
    TS_Point p = touch.getPoint();
    *rx = p.x;
    *ry = p.y;
    return true;
}

/* @p sz is the raw pressure, for the second-contact guard — see
 * touch_jump_filter(). Higher means lower resistance across the panel, which is
 * a harder press or, the case that matters here, one more finger. */
static bool touch_to_screen(int16_t *sx, int16_t *sy, int16_t *sz) {
    if (!touch.tirqTouched() || !touch.touched()) {
        return false;
    }
    TS_Point p = touch.getPoint();
    *sz = (int16_t)p.z;
    int16_t mx = map(p.x, s_cal_xmin, s_cal_xmax, 0, SCR_W);
    int16_t my = map(p.y, s_cal_ymin, s_cal_ymax, 0, SCR_H);
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

/* The white card the sale-flow screens draw into — see build_page(). NULL on
 * every other screen, which is how the driver below tells the two apart. */
static lv_obj_t *s_page_card = NULL;

/* Second-contact guard — see touch_jump_filter(). Sale screens only: every
 * control there is a tap, so nothing legitimately jumps, while the admin panel's
 * sliders and scrolling lists cross far more than 25px in a 30ms read period. */
static touch_jump_t s_jump = { 0, 0, 0, false };
static bool         s_jump_logged = false;   /* one log line per press */

/* Swipe up from the bottom edge — the admin panel's door now that the burger is
 * gone. Detected here, in the driver, rather than as an LVGL gesture: the screen
 * it has to work on is covered by a keypad and a Charge button, and a gesture
 * delivered to a button is one the screen underneath never sees.
 *
 * The travel is deliberately longer than a fat-finger slip on the Charge button
 * directly above the handle — that button commits a sale, so the two must not be
 * confusable in either direction. */
#define SWIPE_BAND_H   44    /* press must START in this bottom band */
#define SWIPE_MIN_DY   58    /* ...and travel at least this far up   */
static bool          s_swipe_armed  = false;
static int16_t       s_swipe_y0     = 0;
static volatile bool s_swipe_admin  = false;   /* handed to the UI task loop */

static void indev_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    (void)drv;
    int16_t x, y, z = 0;
    bool pressed = touch_to_screen(&x, &y, &z);

    /* Two fingers read as one point between them — hold the first one's. Ahead
     * of the swipe so an artifact cannot arm that either, and off once the swipe
     * IS armed: that drag is the one place a sale screen moves a finger far in
     * one read period, and a false swipe only opens a PIN-locked screen. */
    const int16_t raw_x = x, raw_y = y;
    if (!pressed || ((s_page_card != NULL) && !s_swipe_armed)) {
        touch_jump_filter(&s_jump, pressed, z, &x, &y);
    }

    /* The numbers TOUCH_Z_STEP_PCT is set from. Once per press, not per read —
     * the guard firing is an event, and at INFO because the log is capped there
     * (CONFIG_LOG_MAXIMUM_LEVEL=3), which compiles ESP_LOGD/V away entirely.
     * Never fires with a thumb down: lower the percentage. Fires on ordinary
     * one-finger taps: raise it. */
    if (!pressed) {
        s_jump_logged = false;
    } else if (!s_jump_logged && ((x != raw_x) || (y != raw_y))) {
        s_jump_logged = true;
        ESP_LOGI(TAG, "second contact: %d,%d held at %d,%d (z=%d, z_min=%d)",
                 (int)raw_x, (int)raw_y, (int)x, (int)y, (int)z,
                 (int)s_jump.z_min);
    }

    /* Bottom-edge swipe up. Armed on a press that starts in the band, fired
     * once it has travelled far enough; the rest of the drag is swallowed via
     * s_wait_release so the button under the finger never sees a click. The
     * screen it is allowed on is decided by the handler, not here. */
    if (pressed) {
        if (!s_swipe_armed && (y >= (SCR_H - SWIPE_BAND_H))) {
            s_swipe_armed = true;
            s_swipe_y0    = y;
        } else if (s_swipe_armed && ((s_swipe_y0 - y) >= SWIPE_MIN_DY)) {
            s_swipe_armed  = false;
            s_swipe_admin  = true;
            s_wait_release = true;
            data->state    = LV_INDEV_STATE_REL;
            return;
        }
    } else {
        s_swipe_armed = false;
    }

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

/* Tx-status payload. The info line is updated in place while the screen is up
 * (the confirmation countdown) — a request_screen per tick would restart the
 * spinner, so the label is held and s_tx_info_dirty drives a targeted set, the
 * same hand-off the boot step uses. */
static ui_tx_state_t s_tx_state    = UI_TX_STATE_PLACE_CARD;
static char          s_tx_info[64] = "";
static lv_obj_t     *s_tx_info_lbl = NULL;
static volatile bool s_tx_info_dirty = false;

/* Touch calibration. Two targets: two points are the whole of the linear map
 * touch_to_screen() applies, so a four-corner routine would be averaging away a
 * tilt this driver cannot express anyway.
 * ponytail: two-point linear. If a panel turns out skewed rather than offset
 * and scaled, the upgrade is an affine map and four corners.
 *
 * Steps 0 and 1 capture a corner each; step 2 applies the result live and asks
 * the operator to confirm it by tapping a button with it. That tap is the test:
 * a calibration bad enough to make Save unreachable is never stored, and the
 * deadline puts the old numbers back for the operator who cannot hit anything
 * at all. */
#define CAL_INSET       20      /* target centre, in from each corner       */
#define CAL_VERIFY_MS   20000U  /* un-confirmed calibration reverts after this */
static uint8_t  s_cal_step = 0U;
static int16_t  s_cal_raw[2][2] = {{0, 0}, {0, 0}};
static bool     s_cal_pressed = false;
static int16_t  s_cal_last_x = 0, s_cal_last_y = 0;
static uint16_t s_cal_prev[4] = {0, 0, 0, 0};   /* restored on cancel/timeout */
static uint32_t s_cal_deadline = 0U;
static lv_obj_t *s_cal_countdown = NULL;

/* The sheet that rises on a swipe up. While it is set, build_admin_screen()
 * builds into it and does NOT clear the screen — the sale screen has to stay
 * put underneath or there is nothing for the sheet to slide over, which is the
 * entire point of the gesture. Live only for the length of one dispatch in
 * render_requested_screen(); the object itself outlives the pointer. */
static lv_obj_t     *s_sheet         = NULL;
static volatile bool s_sheet_pending = false;

/* The card-wait note. Its own buffer, not the transaction screen's: those are
 * the two screens a card is held to, one of them is a sale and the other is
 * setup, and sharing 64 bytes let "Reading your payout addresses" turn up under
 * a spinner on a payment. */
static char          s_card_note[64] = "";

/* Amount entry — keypad input string (e.g. "12.50") and its display label. The
 * cents are their own label so they can be set in a smaller font past 100; below
 * that they stay in the main label and this one holds "". */
static lv_obj_t *s_amount_label = NULL;
static lv_obj_t *s_amount_cents_label = NULL;
/* The flex row the two of them sit in — kept because its placement is not
 * fixed: see amount_row_place(). */
static lv_obj_t *s_amount_row = NULL;
static uint64_t  s_amount_cents = 0;      /* amount entered, in cents          */

/* The asset selector on the amount row, and the chevron it drops when it has to
 * make room. Both die with the screen — cleared in clear_screen(). */
static lv_obj_t *s_asset_btn   = NULL;
static lv_obj_t *s_asset_arrow = NULL;

/* The Charge button, kept because it is enabled and disabled as digits arrive
 * and leave — see charge_set_enabled(). */
static lv_obj_t *s_charge_btn  = NULL;

/* The status band's clock. Built per screen by build_page(), so it exists on the
 * sale flow and nowhere else — the admin page's tab bar owns y=0..42 and the
 * setup screens own their own headers, which is the same collision that took the
 * Wi-Fi mark off those screens. Retexted in place by the status timer. */
static lv_obj_t *s_clock_lbl   = NULL;
/* ...and whether it has already been tapped and is waiting on main. Its own
 * flag rather than reading the button's state, because the keypad is still live
 * during that wait and amount_update_display() would otherwise hand the button
 * straight back on the next digit — see charge_set_busy(). */
static bool      s_charge_busy = false;

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
static lv_obj_t     *s_pin_eye_lbl   = NULL;   /* same, on the card-PIN keypad */

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

/* The Tx tab's two gas rows, same hand-off again. The caps are written straight
 * through from the config page on the HTTP task, and the page is reached from a
 * card raised OVER this screen — so the rows are retexted where they stand
 * rather than by rebuilding the screen, which would take that card down with it
 * while the operator is still reading the address off it. */
static lv_obj_t     *s_fee_max_lbl       = NULL;
static lv_obj_t     *s_fee_prio_lbl      = NULL;
static volatile bool s_fees_dirty        = false;

/* Phone setup (UI_SCREEN_PROV). The step is an int, not a prov_step_t, for the
 * same reason ui_show_prov takes one — provision.h includes ui.h. Both are
 * written by the main task and read by the UI task, like the splash line above. */
static volatile int  s_prov_step         = 0;
/* Why the last card read came to nothing, shown on the setup screen. The reason
 * also goes to the browser (prov_set_note), but the person who just tapped is
 * looking at the panel — and without this the panel simply snapped back to the
 * step it came from, which reads as the terminal resetting mid-read. Cleared
 * whenever a fresh read starts. */
static char          s_prov_msg[64]      = "";
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

/******************************************************************
 * 6. Button actions
 ******************************************************************/
enum BtnAction {
    ACT_CONFIRM, ACT_CANCEL, ACT_SEND, ACT_NEW,
    ACT_CLOSE, ACT_PIN_CANCEL, ACT_PIN_REVEAL,
    ACT_WIFI, ACT_WIFI_CANCEL, ACT_WIFI_PASS_REVEAL,
    ACT_ADMIN_CANCEL, ACT_WELCOME_OK,
    /* Config portal: accept/reject a proposed value, finish the wizard. */
    ACT_PROV_OK, ACT_PROV_NO, ACT_PROV_FINISH,
    ACT_RESET, ACT_RESET_CONFIRM, ACT_MODAL_CLOSE,
    /* Asset selector (amount screen and the Tx tab): network, then the coin. */
    ACT_NET_PICK, ACT_NET_ETH, ACT_NET_POLY, ACT_NET_TRON,
    /* The admin web page: open it (QR to scan), close it, resolve an upload. */
    ACT_PORTAL, ACT_PORTAL_CLOSE, ACT_OTA_OK, ACT_OTA_NO,
    /* Touch calibration: start it, keep the result, put the old one back. */
    ACT_TOUCH_CAL, ACT_CAL_SAVE, ACT_CAL_CANCEL,
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
/* The networks the picker offers are pos_net_t (assets.h) — step 1 of the picker
 * is a network, and several chains share one (USDC and USDT are both Ethereum).
 * This file used to carry its own copy of that enum. */

static void open_network_picker(void);
static void open_coin_picker(pos_net_t net);
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

/** The selected asset's row — ticker, standard, caption, network. */
static const pos_asset_t *asset(void) {
    return pos_asset_of(settings_get_chain());
}

/** Ticker of the asset being charged, for the selector and the amount screens. */
static const char *asset_name(void) {
    return asset()->ticker;
}

/**
 * Which network that asset lives on — the selector's subtitle.
 *
 * Names the deployment, not just the family: "Ethereum" and "Ethereum Sepolia"
 * differ by the only thing that decides whether a sale settles in money, and this
 * subtitle is where an operator finds out which one the terminal is on.
 */
static const char *asset_network(void) {
    const pos_net_info_t *ni = pos_net_info(asset()->net);
    return settings_net_str(ni->long_test, ni->long_main);
}

/** Caption for the address row above "Send to": TRX has no contract to show. */
static const char *asset_caption(void) {
    return asset()->caption;
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

/* Left gutter the figure may never cross — see amount_row_place(). */
#define AMOUNT_ROW_MIN_X  4

/******************************************************************
 * 6b. Sale-flow page chrome
 *
 * Teal page, step dashes across the top, white rounded card, and the home
 * indicator at the bottom that is the admin panel's handle. Screens built with
 * build_page() place their children into the CARD, so their coordinates are the
 * card's own — (0,0) is its top-left corner, not the screen's.
 *
 * Only the sale runs on this chrome. The admin panel behind the swipe keeps the
 * full-screen white it had: it is a settings app, not part of the customer's
 * transaction, and the two reading differently is the point.
 ******************************************************************/
#define CARD_X    8
#define CARD_Y    28
#define CARD_W    (SCR_W - (2 * CARD_X))   /* 224 */
#define CARD_H    262                      /* 28..290; home bar sits below */
#define CARD_PAD  10
/* Bottom action button, in card coordinates. */
#define CARD_BTN_H  44
/* Corner radius for every button on the panel — see make_button. */
#define BTN_RADIUS  6
#define CARD_BTN_Y  (-10)
#define CARD_BTN_W  (CARD_W - (2 * CARD_PAD))

/* The status band, left to right: clock, the TEST chip when there is one, the
 * progress rail, the Wi-Fi mark.
 *
 * Fixed zones, not measured ones. Every occupant is a known string in a known
 * font, and a rail that re-measures itself against the clock would change
 * length when the minute ticks from 09:59 to 10:00 — a band that twitches on
 * its own is the thing this layout exists to stop.
 *
 * Everything sits on one optical centre line at y≈12: montserrat_14 draws a
 * 16px box (so text at 4), the Wi-Fi mark is 13 tall at 6, the rail is 3 at 11.
 * Move one and move the others.
 *
 * CLOCK_W and CHIP_W are reserves rather than the real widths — "00:00"
 * measures ~36 against 42, "TEST" with its pads ~54 against 56. The slack is
 * what keeps the rail clear of them without anyone having to re-measure. */
#define BAND_Y      4      /* text top: clock and chip                  */
#define CLOCK_X     10
#define CLOCK_Y     BAND_Y
#define CLOCK_W     42
#define CHIP_X      (CLOCK_X + CLOCK_W + 6)
#define CHIP_W      56
#define CHIP_Y      3      /* 20px tall, so 3 centres it on the band    */
#define RAIL_GAP    8      /* clearance from whatever is either side    */
#define RAIL_Y      11
#define RAIL_H      3      /* one line                                  */
/* The band's right end belongs to the Wi-Fi mark: its width plus its inset from
 * the screen edge. A reserve like the two above, because the mark's geometry is
 * declared in section 8b, below every user of it — and section 8b carries a
 * static_assert that it still fits in here, so shrinking the mark costs nothing
 * and growing it past this fails the build instead of drawing over the rail. */
#define SIG_ZONE_W  40

/* Steps of one sale. PAY_STEP_NONE draws no rail — a card read during setup
 * is not a sale and must not claim a place in its progress. */
enum {
    PAY_STEP_NONE   = -1,
    PAY_STEP_AMOUNT = 0,
    PAY_STEP_REVIEW,
    PAY_STEP_AUTH,
    PAY_STEP_TAP,
    PAY_STEP__COUNT
};

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
 * room before the digits do. What gives way is the centring, by as little as it
 * takes: the row slides left only far enough to clear the selector, so a typical
 * amount stays centred and a five-figure one drifts. Both ends of that sum are
 * measured rather than predicted — the digits because the font decides them, and
 * the selector because its ticker and its chevron both change its width.
 *
 * Nothing on this row is ever dropped to make it fit. The figure is money; the
 * ticker that says which money lives on the selector, where it does not have to
 * compete with the digits for the same 240 pixels.
 */
static void amount_row_place(void) {
    if (s_amount_row == NULL) { return; }

    /* Centred, then measured there — whether it fits is a question about the pill
     * and the string that were just set, not about arithmetic. Both are
     * content-sized, so both are measured: the pill drops its chevron once there is
     * an amount, and the ticker inside it is 3 or 4 characters wide. */
    lv_obj_align(s_amount_row, LV_ALIGN_TOP_MID, 0, ASSET_BTN_Y);
    lv_obj_update_layout(s_amount_row);

    /* Measured off the pill's own left edge rather than recomputed from the
     * screen width: the row lives on the card now, so a figure derived from
     * SCR_W would be answering about the wrong box. Everything here is in
     * absolute coordinates, which is what lv_obj_get_coords reports. */
    lv_area_t   r, pr;
    lv_obj_get_coords(s_amount_row, &r);
    lv_coord_t  stop = r.x2;
    if (s_asset_btn != NULL) {
        lv_obj_get_coords(s_asset_btn, &pr);
        stop = pr.x1 - AMOUNT_ROW_GAP;
    }
    lv_coord_t  x = (r.x2 > stop) ? (stop - r.x2) : 0;

    /* ...but never off the left edge. The shift above is measured against the
     * pill alone, so a figure wide enough simply kept sliding until its leading
     * digits were outside the screen — and the leading digits are the ones that
     * decide what the customer is charged. Losing the right-hand end to the
     * pill is survivable; losing the left-hand end silently is not, so the
     * clamp wins and the two overlap instead.
     *
     * Against the row's PARENT, not the screen: that parent is the white card,
     * inset from both edges, so the gutter the figure must not cross is the
     * card's and not the panel's. */
    lv_area_t cr;
    lv_obj_get_coords(lv_obj_get_parent(s_amount_row), &cr);
    if ((r.x1 + x) < (cr.x1 + AMOUNT_ROW_MIN_X)) {
        x = (cr.x1 + AMOUNT_ROW_MIN_X) - r.x1;
    }

    lv_obj_align(s_amount_row, LV_ALIGN_TOP_MID, x,
                 ASSET_BTN_Y + ((ASSET_BTN_H - lv_area_get_height(&r)) / 2));
}

/**
 * Charge is available only once there is something to charge.
 *
 * 0.00 is not a sale, and the button was fully lit for it — the operator's tap
 * did nothing and the screen said nothing about why, which on a resistive panel
 * reads as a missed touch rather than as a refusal. btn_event_cb has always
 * dropped the event; this is what makes that visible.
 *
 * LV_STATE_DISABLED rather than hiding the button: LVGL's hit test drops taps on
 * a disabled object (lv_obj_pos.c), so the state IS the whole gate, and a greyed
 * button still holds the place the operator is about to aim at. The label is
 * recoloured by hand because make_button() sets its colour on the label itself,
 * where the button's state does not reach it — the same reason pill_disable()
 * walks its children.
 */
static void charge_set_enabled(bool on) {
    if (s_charge_btn == NULL) { return; }   /* not the amount screen */

    if (on) { lv_obj_clear_state(s_charge_btn, LV_STATE_DISABLED); }
    else    { lv_obj_add_state(s_charge_btn, LV_STATE_DISABLED); }

    lv_obj_set_style_bg_color(s_charge_btn, on ? COL_ACCENT : COL_SURFACE,
                              LV_PART_MAIN);
    lv_obj_t *lbl = lv_obj_get_child(s_charge_btn, 0);
    if (lbl != NULL) {
        lv_obj_set_style_text_color(lbl, on ? COL_BG : COL_DIM, LV_PART_MAIN);
    }
}

/**
 * Charge has been tapped: inert until this screen is next built.
 *
 * A gate, not a look — it does not repaint the button. What it guards against is
 * a second UI_EVENT_AMOUNT_CONFIRMED queued behind the first. The duplicate is
 * harmless where it is raised, since main answers it with the same confirm
 * screen, but it outlives the screen it was meant for: an operator who
 * double-tapped Charge and then tapped Confirm gets pulled back out of the PIN
 * keypad by the echo of their own first tap.
 *
 * It used to grey the button and relabel it "Checking...", from when the balance
 * check ran at this point and took a network round trip to answer. That check
 * moved to the card tap, where the payer is actually known — so there is nothing
 * to wait for here and nothing to announce. The screen changes immediately, and
 * a button that repaints on its way out is a flicker, not feedback.
 */
static void charge_set_busy(void) {
    if (s_charge_btn == NULL) { return; }
    s_charge_busy = true;
    /* LVGL's hit test drops taps on a disabled object (lv_obj_pos.c), so the
     * state alone is the whole gate. */
    lv_obj_add_state(s_charge_btn, LV_STATE_DISABLED);
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
     * it keeps its chevron. Once there is an amount, it gets out of its way and the
     * digits take the room back. The ticker stays either way — it is on the pill,
     * not on the figure, so it costs the digits nothing. */
    asset_btn_set_compact(s_amount_cents != 0ULL);
    /* Left alone while a tap is in flight: this runs on every keypress and the
     * keypad stays live, so a digit typed in that window would otherwise re-arm
     * a button that has already asked. Skipped rather than called with false,
     * so the button keeps its ordinary look on the way out. */
    if (!s_charge_busy) {
        charge_set_enabled(s_amount_cents != 0ULL);
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

/**
 * Flip a masked field between hidden and shown, and swap its eye glyph to match.
 *
 * Toggled in place, never via request_screen(): rebuilding the screen would
 * discard what has been typed, which on the PIN keypad costs the operator an
 * attempt the card is counting. The glyph shows what the next tap does.
 *
 * Both callers may pass NULL — the action can only be raised from the screen that
 * built the field, but that is a fact about two files agreeing rather than
 * something the compiler checks.
 */
static void code_field_reveal(lv_obj_t *ta, lv_obj_t *eye_lbl) {
    if (ta == NULL) { return; }
    const bool masked = lv_textarea_get_password_mode(ta);
    lv_textarea_set_password_mode(ta, !masked);
    if (eye_lbl != NULL) {
        lv_label_set_text(eye_lbl, masked ? LV_SYMBOL_EYE_CLOSE
                                          : LV_SYMBOL_EYE_OPEN);
    }
}

/**
 * @brief Open the admin panel's front door — the code screen, not the panel.
 *
 * One lock for the whole menu: Wi-Fi, fee caps and the factory reset are all
 * merchant operations, and the reset in particular must not be one tap away
 * from a customer left alone with the terminal.
 *
 * No code stored means first-run setup has not finished, so the request is
 * ignored rather than let through. main creates the code before anything else,
 * so this state is never reachable for long — but it WAS reachable, by backing
 * out of the first-run Wi-Fi picker onto the amount screen while main was still
 * waiting.
 *
 * The swipe is now the only caller — the burger it replaced is gone. It still
 * arms the same lock the button did: a gesture that skipped the penalty would
 * be a cheaper way in than the control it replaced.
 */
static void open_admin_entry(void) {
    s_settings_return = s_req_screen;   /* remember where we came from */
    if (!settings_has_admin_code()) { return; }
    s_settings_tab     = 0U;   /* a fresh open starts on Screen */
    s_admin_confirming = false;
    s_admin_for_portal = false;
    s_admin_note[0]    = '\0';
    /* Re-arm the wait from the persisted attempt count. The wait itself has to
     * live in RAM — persisting a deadline would need a trustworthy absolute
     * clock, and the wall clock is exactly what an attacker on the network can
     * move — so a power cycle used to clear it and bring the cost of one guess
     * down to a single reboot. Deriving it here instead makes the escalation
     * survive reboots, at no extra write. */
    s_admin_lock_ms    = admin_penalty_ms(settings_admin_fail_count());
    s_admin_lock_start = lv_tick_get();
    /* Arrive as a sheet: the gesture pulled it up, so it should look pulled up.
     * Set here rather than at the gesture, so the early return above cannot
     * leave the flag armed for an unrelated visit to this screen. */
    s_sheet_pending = true;
    request_screen(UI_SCREEN_ADMIN_UNLOCK);
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
        /* The contract and payout strings belong to the chain, not to the
         * widgets, so repoint them before the rebuild reads them — otherwise the
         * Tx tab redraws with the previous asset's contract and, across the
         * Ethereum/Tron divide, the previous network's payout address. */
        ui_refresh_addresses();
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
                /* Before the callback, not after. What main does with the event
                 * is its business — it may answer in a microsecond or after a
                 * network round trip — and this screen must have stopped taking
                 * taps by the time either can happen. */
                charge_set_busy();
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
        case ACT_TOUCH_CAL:
            settings_persist();
            /* Keep what is in force, so a cancel — or an operator who can no
             * longer hit anything — gets the working panel back. */
            s_cal_prev[0] = s_cal_xmin; s_cal_prev[1] = s_cal_xmax;
            s_cal_prev[2] = s_cal_ymin; s_cal_prev[3] = s_cal_ymax;
            s_cal_step    = 0U;
            s_cal_pressed = false;
            request_screen(UI_SCREEN_TOUCH_CAL);
            break;
        case ACT_CAL_SAVE:
            settings_set_touch_cal(s_cal_xmin, s_cal_xmax,
                                   s_cal_ymin, s_cal_ymax);
            /* Read back what was actually stored: settings_set_touch_cal
             * refuses a collapsed span, and the panel must then keep running
             * on the numbers in NVS rather than the ones it rejected. */
            touch_cal_load();
            request_screen(UI_SCREEN_SETTINGS);
            break;
        case ACT_CAL_CANCEL:
            s_cal_xmin = s_cal_prev[0]; s_cal_xmax = s_cal_prev[1];
            s_cal_ymin = s_cal_prev[2]; s_cal_ymax = s_cal_prev[3];
            request_screen(UI_SCREEN_SETTINGS);
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
            code_field_reveal(s_wifi_pass_ta, s_wifi_eye_lbl);
            break;
        case ACT_PIN_REVEAL:
            code_field_reveal(s_pin_ta, s_pin_eye_lbl);
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
            open_coin_picker((act == ACT_NET_TRON) ? POS_NET_TRON :
                             (act == ACT_NET_POLY) ? POS_NET_POLY : POS_NET_ETH);
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

    /* Base look — FLAT fill (kill the default theme's gradient, which washes a
     * dark fill toward light), no default border.
     *
     * BTN_RADIUS is 6, down from 10. At 44px tall a 10px radius is a quarter of
     * the height gone to corner on each side, which is the shape a toy app uses;
     * 6 still reads as softened but leaves the button a rectangle, which is what
     * a terminal taking money should look like. The full-radius pill stays where
     * it belongs — the asset selector and the chips, which are not actions. */
    lv_obj_set_style_bg_color(btn, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_NONE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, BTN_RADIUS, LV_PART_MAIN);
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

/* The photo's secondary button: white with a hairline, not a grey slab. Cancel
 * on a payment screen is the answer that costs nothing, so it should read as
 * available without competing with the one that commits. */
static lv_obj_t *make_ghost_button(lv_obj_t *parent, const char *label,
                                   lv_coord_t w, lv_align_t align,
                                   lv_coord_t x, lv_coord_t y, BtnAction act) {
    lv_obj_t *b = make_button(parent, label, COL_BG, COL_TEXT, w, CARD_BTN_H,
                              align, x, y, act, &lv_font_montserrat_20);
    lv_obj_set_style_border_width(b, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(b, COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    return b;
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

/* The network's own mark, and the little chip that says which network a token
 * lives on. Indexed by pos_net_t, so the order here follows assets.h. The images
 * are the one asset fact that cannot live in that table — they are LVGL types,
 * and the table is kept host-testable. */
static const lv_img_dsc_t *const NET_ICON[POS_NET__COUNT] = {
    &icon_eth, &icon_poly, &icon_tron
};
static const lv_img_dsc_t *const NET_CHIP[POS_NET__COUNT] = {
    &chip_eth, &chip_poly, &chip_tron
};

/* A token's own mark, by ticker. Two of them, so a lookup rather than a table —
 * and USDC is the fallback for the same reason the old switch defaulted to it. */
static const lv_img_dsc_t *coin_icon(const char *ticker) {
    return (strcmp(ticker, "USDT") == 0) ? &icon_usdt : &icon_usdc;
}

/* The selected asset. A native coin IS its network, so it carries no chip —
 * the chip only says "this token lives over there", which is meaningless
 * stacked on the network's own logo. */
static lv_obj_t *make_asset_badge(lv_obj_t *parent, pos_chain_t chain) {
    const pos_asset_t *a = pos_asset_of(chain);
    const int n = (int)a->net;
    /* The network's own coin wears its network mark and no chip: a chip says
     * "this token, on that network", and there is no second thing to say when
     * the asset IS the network. */
    if (a->native) { return make_icon_box(parent, NET_ICON[n], NULL); }
    return make_icon_box(parent, coin_icon(a->ticker), NET_CHIP[n]);
}

/* The asset selector itself: the badge above turned into a tappable pill, at the
 * right-hand end of the amount's row — mark, ticker, chevron.
 *
 * A flex row at LV_SIZE_CONTENT rather than three aligned children in a fixed box.
 * The ticker is 3 or 4 characters depending on the asset, so the pill's width is
 * not a number this file can know; LVGL measures it, the align keeps the right
 * edge pinned, and amount_row_place() reads back what it came out as. The old
 * fixed widths existed because the pill was wordless and every one of its children
 * was a known size. */
static lv_obj_t *make_asset_button(lv_obj_t *parent) {
    s_asset_btn = lv_btn_create(parent);
    lv_obj_set_size(s_asset_btn, LV_SIZE_CONTENT, ASSET_BTN_H);
    lv_obj_align(s_asset_btn, LV_ALIGN_TOP_RIGHT, ASSET_BTN_X, ASSET_BTN_Y);
    lv_obj_set_style_bg_color(s_asset_btn, COL_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(s_asset_btn, LV_GRAD_DIR_NONE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_asset_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_asset_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_asset_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_asset_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_asset_btn,
                              lv_color_mix(lv_color_black(), COL_SURFACE, 20),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_asset_btn, btn_event_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(
                            static_cast<intptr_t>(ACT_NET_PICK)));

    /* Pads and gap written out: the theme's button padding is what used to shove
     * the badge on top of the chevron, and a flex row obeys them rather than
     * ignoring them the way aligned children did. */
    lv_obj_set_flex_flow(s_asset_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_asset_btn, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_asset_btn, ASSET_BTN_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_column(s_asset_btn, 5, LV_PART_MAIN);
    lv_obj_clear_flag(s_asset_btn, LV_OBJ_FLAG_SCROLLABLE);

    (void)make_asset_badge(s_asset_btn, settings_get_chain());
    make_label(s_asset_btn, asset_name(), COL_TEXT, &lv_font_montserrat_14,
               LV_ALIGN_DEFAULT, 0, 0);
    s_asset_arrow = make_label(s_asset_btn, LV_SYMBOL_RIGHT, COL_DIM,
                               &lv_font_montserrat_14, LV_ALIGN_DEFAULT, 0, 0);
    return s_asset_btn;
}

/* Chevron off once there is an amount to read, so the digits get its width back.
 * The ticker stays: it is the one thing on this row that says which money, and it
 * only becomes worth reading once there is a figure beside it. Hidden rather than
 * deleted, and the pill re-measures itself — right-aligned, so only the left edge
 * moves and the thing under the thumb stays where it was. */
static void asset_btn_set_compact(bool compact) {
    if (s_asset_arrow == NULL) { return; }   /* not the amount screen */
    if (compact) { lv_obj_add_flag(s_asset_arrow, LV_OBJ_FLAG_HIDDEN); }
    else         { lv_obj_clear_flag(s_asset_arrow, LV_OBJ_FLAG_HIDDEN); }
}

/* The bare network mark, for the network picker's own rows — no coin is chosen
 * at that step, so there is nothing to badge it with. */
static lv_obj_t *make_net_badge(lv_obj_t *parent, pos_net_t net) {
    return make_icon_box(parent, NET_ICON[(int)net], NULL);
}

/* "Tap here" mark — NFC waves in a ring. Drawn by tools/gen_tap_icon.py into
 * main/tap_icon.c; run that script to change it.
 *
 * IT IS THE MARK ON THE CASE. POS-C1 has this moulded into its back above "TAP
 * TO PAY", and the panel telling a customer where to tap while drawing a
 * different symbol to the one under their card is the reason the old
 * card-and-waves version was replaced. If the tooling changes, this changes
 * with it — that is the whole constraint on this artwork.
 *
 * ON PROVENANCE, kept because "just use a public-domain icon" is a trap worth
 * writing down. Two separate rights are in play and a PD licence settles only
 * one: copyright, where any particular drawing is somebody's work, which is why
 * this one is authored in the generator from a circle and arcs with no stock
 * file traced or vendored anywhere in the tree; and trademark, where EMVCo's
 * Contactless Indicator is four bare arcs and a CC0 file grants nothing against
 * a mark. That second question is answered by the product rather than by this
 * file — Cryptnox tools its own cases with this symbol, and the firmware
 * follows the hardware.
 *
 * ALPHA_8BIT: one alpha byte per pixel, coloured at draw time from the object's
 * img_recolor style. LVGL's fast path for that format does not support angle or
 * zoom — set either and it falls back to a path with no decoded data and draws
 * nothing — so this must stay an untransformed image. Aligned TOP_MID at @p y,
 * occupying TAP_MARK_SZ square: the same box every previous version of this
 * mark has had. */
#define TAP_MARK_SZ   96

/* The mark's ink, and the one place to change it. Black — but it was COL_TITLE
 * (mid-grey) for as long as the mark was drawn in solid strokes, and the reason
 * it moved is worth keeping, because it is the same reason in both directions.
 *
 * The argument against black was WEIGHT: a 96px block of solid line art on a
 * card whose text is otherwise grey read as a separate object dropped onto the
 * screen. Grey pulled it back into the ramp. Then the mark was redrawn in the
 * case's outline style — two hairlines per stroke around a hollow middle — and
 * its ink dropped by roughly two thirds. The weight problem it was solving
 * stopped existing, and grey hairlines instead landed where COL_DIM always
 * would have: faint enough to read as disabled, which is wrong for the one
 * thing on the screen the customer is being asked to act on.
 *
 * So the rule, not the value: this tracks the artwork's weight. Go back to
 * solid strokes in gen_tap_icon.py and this goes back to COL_TITLE. The amount
 * keeps full black either way — the figure being charged is read first. */
#define COL_TAP_MARK  COL_TEXT

static void make_tap_mark(lv_obj_t *parent, lv_coord_t y) {
    lv_obj_t *img = lv_img_create(parent);
    lv_img_set_src(img, &tap_icon);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    /* The mask carries shape only; this is where its colour comes from. */
    lv_obj_set_style_img_recolor(img, COL_TAP_MARK, LV_PART_MAIN);
    lv_obj_set_style_img_recolor_opa(img, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(img, LV_ALIGN_TOP_MID, 0, y);
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

/* A read-only row on the settings tabs: dim caption over its value, as one
 * flex item. The tabs are flex columns (see build_settings), so a row's
 * position is where it was appended and not a y this file works out — which is
 * what the fourteen hand-computed offsets here used to be, every one of them
 * re-derived whenever a row above it wrapped to a second line or disappeared on
 * Tron. Returns the value label, for the callers that update it in place.
 *
 * Values wrap by default: these are addresses. An SSID wants LV_LABEL_LONG_DOT
 * instead, which the caller sets — 32 arbitrary characters over two lines push
 * the rows below them about for no gain. */
static lv_obj_t *make_field(lv_obj_t *parent, const char *caption,
                            const char *value, lv_color_t col = COL_TEXT) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, TAB_W, LV_SIZE_CONTENT);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 4, LV_PART_MAIN);

    make_label(box, caption, COL_DIM, &lv_font_montserrat_14,
               LV_ALIGN_DEFAULT, 0, 0);
    lv_obj_t *v = make_label(box, value, col, &lv_font_montserrat_14,
                             LV_ALIGN_DEFAULT, 0, 0);
    lv_obj_set_width(v, TAB_W);
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
    return v;
}

static void sheet_y_cb(void *obj, int32_t v) {
    lv_obj_set_y(static_cast<lv_obj_t *>(obj), static_cast<lv_coord_t>(v));
}

/* Full-screen opaque panel parked one screen-height below the fold. */
static lv_obj_t *sheet_open(void) {
    lv_obj_t *sh = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(sh);
    lv_obj_set_size(sh, SCR_W, SCR_H);
    lv_obj_set_pos(sh, 0, SCR_H);
    lv_obj_set_style_bg_color(sh, COL_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sh, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(sh, 16, LV_PART_MAIN);
    lv_obj_clear_flag(sh, LV_OBJ_FLAG_SCROLLABLE);
    /* Clickable (the LVGL default) on purpose: it must swallow taps meant for
     * the screen it is covering, which is still fully built underneath. */
    return sh;
}

static void sheet_slide_in(lv_obj_t *sh) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, sh);
    lv_anim_set_exec_cb(&a, sheet_y_cb);
    lv_anim_set_values(&a, SCR_H, 0);
    lv_anim_set_time(&a, 260);
    /* Decelerating, like every sheet anybody has used on a phone — a linear
     * slide reads as mechanical at this size. */
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
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
    s_charge_btn   = NULL;
    s_charge_busy  = false;   /* the button the flag was about is gone */
    s_clock_lbl    = NULL;
    s_pin_ta       = NULL;   /* deleted by lv_obj_clean — drop the dangling ref */
    s_pin_eye_lbl  = NULL;
    s_wifi_pass_ta = NULL;
    s_wifi_eye_lbl   = NULL;
    s_boot_step_lbl  = NULL;
    s_tx_info_lbl    = NULL;
    s_cal_countdown  = NULL;
    s_page_card      = NULL;
    s_fee_max_lbl    = NULL;
    s_fee_prio_lbl   = NULL;
    s_admin_ta       = NULL;
    s_admin_note_lbl = NULL;
    s_reset_btn    = NULL;
    s_close_btn    = NULL;
}

/* The UTC offset the clock adds, cached. settings_get_tz_offset_min() opens
 * NVS, and clock_refresh() runs every three seconds — the same argument the
 * touch calibration makes about indev_read. Reloaded when the config page
 * stores a new one, through ui_clock_changed(). */
static int16_t       s_tz_off_min = 0;
static volatile bool s_tz_dirty   = true;

/**
 * @brief Retext the band's clock from the system clock plus the stored offset.
 *
 * "--:--" until the time is real. SNTP sets it a few seconds into boot, and
 * before that the clock reads 1970 — a till showing a confident wrong time is
 * worse than one admitting it has none, and the 1970 case is exactly what a
 * terminal that came up without an uplink would display all day.
 *
 * gmtime_r on an already-offset instant rather than localtime_r on a timezone:
 * that is the whole of why the offset is a number and not a TZ string. tzset
 * and localtime pull in newlib's timezone machinery, which measured 64 KB of
 * the app slot — against an operator picking their offset from a list on the
 * config page, and moving it twice a year where DST applies.
 *
 * Minute resolution, so the 3 s status timer that drives the Wi-Fi mark is
 * ample and no second timer is needed.
 */
static void clock_refresh(void) {
    if (s_clock_lbl == NULL) { return; }

    if (s_tz_dirty) {
        s_tz_dirty   = false;
        s_tz_off_min = settings_get_tz_offset_min();
    }

    const time_t utc = time(NULL);
    /* Nov 2023, comfortably after any build and before any real sale. Tested on
     * the unshifted instant: whether the clock has been set is a question about
     * SNTP, and a -12:00 offset would drag a just-set clock back under a
     * threshold applied after it. */
    if (utc < (time_t)1700000000) {
        lv_label_set_text(s_clock_lbl, "--:--");
        return;
    }

    const time_t local = utc + ((time_t)s_tz_off_min * 60);
    struct tm    tmv;
    if (gmtime_r(&local, &tmv) == NULL) {
        lv_label_set_text(s_clock_lbl, "--:--");
        return;
    }
    char b[8];
    (void)snprintf(b, sizeof(b), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    lv_label_set_text(s_clock_lbl, b);
}

/**
 * @brief Lay the sale-flow chrome and return the white card to build into.
 *
 * @param step Which dash is lit (PAY_STEP_*), or PAY_STEP_NONE for no dashes.
 * @return The card. Children placed in it use ITS coordinates, so (0,0) is the
 *         card's top-left corner — CARD_W x CARD_H, not the screen.
 */
static lv_obj_t *build_page(int step) {
    clear_screen();
    lv_obj_set_style_bg_color(lv_scr_act(), COL_PAGE, LV_PART_MAIN);

    /* One rail, in the band between the clock and the Wi-Fi mark, filled to the
     * step reached.
     *
     * This replaces four dashes, and it gives something up: a sale is a fixed
     * number of discrete steps, and dashes answered "how many more" in a way a
     * continuous bar cannot.
     *
     * It starts after the TEST chip when there is one, which is why this reads
     * the same setting the chip does rather than being told: add_test_chip()
     * runs later, from build_amount, and the rail has to be laid out before
     * anyone knows whether that call will draw anything. The two agree through
     * CHIP_X and CHIP_W and nothing else. */
    if (step >= 0) {
        const bool       testnet = !settings_get_mainnet();
        const lv_coord_t x0      = (testnet ? (CHIP_X + CHIP_W)
                                            : (CLOCK_X + CLOCK_W)) + RAIL_GAP;
        const lv_coord_t x1      = SCR_W - SIG_ZONE_W - RAIL_GAP;
        const lv_coord_t w       = (x1 > x0) ? (lv_coord_t)(x1 - x0) : 0;

        if (w > 0) {
            lv_obj_t *track = lv_obj_create(lv_scr_act());
            lv_obj_remove_style_all(track);
            lv_obj_set_size(track, w, RAIL_H);
            lv_obj_set_pos(track, x0, RAIL_Y);
            lv_obj_set_style_bg_color(track, COL_STEP_OFF, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(track, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_radius(track, LV_RADIUS_CIRCLE, LV_PART_MAIN);
            lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);

            /* step is 0-based, so the first step already shows a quarter
             * filled — the customer has done something by the time they are
             * looking at it. */
            lv_obj_t *fill = lv_obj_create(track);
            lv_obj_remove_style_all(fill);
            lv_obj_set_size(fill,
                            (lv_coord_t)((w * (step + 1)) / PAY_STEP__COUNT),
                            RAIL_H);
            lv_obj_set_pos(fill, 0, 0);
            lv_obj_set_style_bg_color(fill, COL_STEP_ON, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_radius(fill, LV_RADIUS_CIRCLE, LV_PART_MAIN);
            lv_obj_clear_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
        }
    }

    /* The clock, left end of the status band. Built here so it lives exactly
     * where the band does — on the sale flow — and retexted by the status timer
     * rather than by rebuilding the screen every minute. */
    s_clock_lbl = make_label(lv_scr_act(), "", COL_TITLE,
                             &lv_font_montserrat_14,
                             LV_ALIGN_TOP_LEFT, CLOCK_X, CLOCK_Y);
    clock_refresh();

    lv_obj_t *card = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, CARD_W, CARD_H);
    lv_obj_set_pos(card, CARD_X, CARD_Y);
    lv_obj_set_style_bg_color(card, COL_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 14, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* The handle the admin panel is behind. Drawn rather than merely implied:
     * a gesture with nothing on screen saying it exists is a gesture nobody
     * finds, and this one replaced a button that was visibly there. */
    lv_obj_t *home = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(home);
    lv_obj_set_size(home, 84, 5);
    lv_obj_align(home, LV_ALIGN_BOTTOM_MID, 0, -9);
    lv_obj_set_style_radius(home, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(home, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(home, COL_HOME_BAR, LV_PART_MAIN);

    s_page_card = card;
    return card;
}

/******************************************************************
 * 7b. Settings — full-screen page (swipe up from the bottom edge)
 ******************************************************************/
/* Brightness only. It is the one setting this page still changes — the fee caps
 * moved to the config page, which writes them itself. */
static void settings_persist(void) {
    settings_set_brightness(s_brightness);
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

static void build_settings(void) {
    clear_screen();   /* white, full screen */

    lv_obj_t *tv = lv_tabview_create(lv_scr_act(), LV_DIR_TOP, 42);
    lv_obj_set_size(tv, SCR_W, SCR_H - 54);
    lv_obj_align(tv, LV_ALIGN_TOP_MID, 0, 0);
    /* Tabview and tab bar are the theme's (section 3a) — the underline this
     * used to draw by hand is now a filled pill, on every tabview. */

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
    /* Three of the four tabs are a stack of rows, so they are flex columns and
     * every row below is appended rather than placed. What this replaces is the
     * point: fourteen hand-computed y offsets, two of them conditional on
     * whether the chain has gas fees and whether the last update rolled back,
     * plus align_to fixups for the rows that wrap. Nearly every comment in this
     * function used to be about a label that overprinted the one under it.
     *
     * Screen keeps its two offsets: a caption, a percentage aligned to the
     * opposite edge and a slider is a layout, not a stack of rows. */
    lv_obj_t *columns[3] = { t_wifi, t_tx, t_about };
    for (int i = 0; i < 3; i++) {
        lv_obj_set_flex_flow(columns[i], LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(columns[i], 14, LV_PART_MAIN);
    }
    /* About is centred on its logo; the other two read left. */
    lv_obj_set_flex_align(t_about, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

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

    /* The other thing about this screen that varies per unit. Resistive
     * overlays are not identical board to board, and the answer used to be
     * "edit ui.cpp and rebuild" — which is not an answer an operator has. */
    lv_obj_t *calpill = make_pill(t_screen, "Touch", "Calibrate the panel",
                                  TAB_W, 76, ACT_TOUCH_CAL);
    lv_obj_align(make_glyph_disc(calpill, LV_SYMBOL_EDIT, COL_ACCENT, COIN_SZ),
                 LV_ALIGN_LEFT_MID, PILL_ICON_X, 0);

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
    lv_obj_t *wl = make_field(t_wifi, "Network",
                              have_wifi ? cur_ssid : "Not configured");
    /* An SSID is 32 arbitrary characters; elide it on real glyph widths. */
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
        (void)make_field(t_wifi, "Signal", sig);
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
                                TAB_W, 0, ACT_PORTAL);
    lv_obj_align(make_glyph_disc(cpill, LV_SYMBOL_SETTINGS, COL_ACCENT, COIN_SZ),
                 LV_ALIGN_LEFT_MID, PILL_ICON_X, 0);

    /* ── Transaction tab: which asset, what it will call, where the funds go and
     * what gas it will pay.
     *
     * Everything here is read-only *except* the asset selector at the top. The
     * stored settings — contract, payout address, fee caps — are proposed from the
     * config page in AP mode and accepted on this panel, because a resistive screen
     * is the wrong place to retype an address. The asset is a different kind of
     * thing: which one a sale is charged in is a shift-time choice made with a
     * customer waiting, so it stays a tap away both here and on the amount row. ── */
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

    (void)make_field(t_tx, asset_caption(),
                     (s_addr_usdc != NULL) ? s_addr_usdc : "-");
    (void)make_field(t_tx, "Send to",
                     (s_addr_dest != NULL) ? s_addr_dest : "-");

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
                                 LV_ALIGN_DEFAULT, 0, 0);
        lv_obj_set_width(w, TAB_W);
        lv_label_set_long_mode(w, LV_LABEL_LONG_WRAP);
    }

    /* The gas the terminal is willing to pay, reported as two more read-only rows.
     * Tron has no such setting at all — a transfer there is paid in bandwidth and
     * the token's energy cap is compile-time — so on Tron the rows are simply not
     * there, rather than a paragraph explaining an absent control. */
    if (!tron) {
        char fee[16];
        snprintf(fee, sizeof(fee), "%u",
                 static_cast<unsigned>(settings_get_max_fee_gwei()));
        s_fee_max_lbl = make_field(t_tx, "Max fee (Gwei)", fee);

        snprintf(fee, sizeof(fee), "%u",
                 static_cast<unsigned>(settings_get_priority_fee_gwei()));
        s_fee_prio_lbl = make_field(t_tx, "Priority fee (Gwei)", fee);
    }

    /* The way out of a read-only tab. Without it the fees above are two numbers an
     * operator can see and has no way to reach — "set it from the Configure page"
     * is only an instruction if the page is one tap from the rows it is about.
     * Same action as the Wi-Fi tab's row and the About tab's Update: one config
     * page, reachable from wherever somebody went looking for the setting.
     *
     * The subtitle names the fees on the chains that have them, because that is
     * what this row was added for; on Tron there are none, so it falls back to
     * saying where it goes. Both sit inside make_pill's 140px subtitle cap — "Set
     * fees in a browser" does not, and arrived dot-elided. */
    lv_obj_t *tpill = make_pill(t_tx, "Configure",
                                tron ? "Open in a browser" : "Fees in a browser",
                                TAB_W, 0, ACT_PORTAL);
    lv_obj_align(make_glyph_disc(tpill, LV_SYMBOL_SETTINGS, COL_ACCENT, COIN_SZ),
                 LV_ALIGN_LEFT_MID, PILL_ICON_X, 0);

    /* ── About tab: small C logo, name, version, info ── */
    lv_obj_t *blogo = lv_img_create(t_about);
    lv_img_set_src(blogo, &logo_small);   /* 40px dedicated image */

    make_label(t_about, "cryptnox-pos", COL_TEXT, &lv_font_montserrat_20,
               LV_ALIGN_DEFAULT, 0, 0);
    /* Straight out of the running image's header rather than a #define, so that
     * after an update this reads as the firmware that is actually executing. */
    char about_ver[OTA_VERSION_SHOWN_MAX];
    make_label(t_about,
               ota_version_display(ota_running_version(), about_ver,
                                   sizeof(about_ver)),
               COL_DIM, &lv_font_montserrat_14, LV_ALIGN_DEFAULT, 0, 0);

    /* An update that installed, booted and was then reverted leaves this tab
     * reading the old version with nothing to say why — which is how "I updated
     * it and nothing happened" gets reported about a mechanism that worked
     * exactly as designed. One red line, under the version it is about. It clears
     * itself: the next update overwrites the slot this verdict is read from.
     *
     * The rows below shift down by the line's height when it is there, rather
     * than the line being squeezed into the gap: this tab scrolls, so there is
     * somewhere for them to go, and a warning overlapping the Update button is
     * worse than no warning. The column does that shift by itself now — it was
     * the y_update/y_about pair of ternaries. */
    if (ota_last_update_failed()) {
        make_label(t_about, "Last update rolled back", COL_DANGER,
                   &lv_font_montserrat_14, LV_ALIGN_DEFAULT, 0, 0);
    }

    /* The update row. Tapping it opens the config page on the venue network and
     * shows a QR code to scan, so it belongs behind the admin code with the rest
     * of the settings. Same action as the Configure row on the Wi-Fi tab — one
     * page, reachable from wherever the operator went looking for it. */
    /* Subtitle kept short deliberately: make_pill caps it at
     * TAB_W - PILL_TEXT_X - PILL_TEXT_PAD_R = 140 px and dot-elides the rest,
     * and a row whose own label is cut off reads as a bug. */
    lv_obj_t *upill = make_pill(t_about, "Update", "From a browser",
                                TAB_W, 0, ACT_PORTAL);
    lv_obj_align(make_glyph_disc(upill, LV_SYMBOL_DOWNLOAD, COL_ACCENT, COIN_SZ),
                 LV_ALIGN_LEFT_MID, PILL_ICON_X, 0);

    /* Deliberately says nothing about which assets or which networks. That list
     * changes with the firmware and this label cannot be edited from anywhere, so
     * a terminal a year from now would be reading out a stale one — and the Tx tab
     * already answers the question from the live settings. */
    lv_obj_t *about = make_label(t_about,
                                 "Crypto payment terminal\n"
                                 "for Cryptnox cards\n\n"
                                 "Based on cryptnox-sdk-esp32 1.0.0\n"
                                 "(c) Cryptnox 2026 - Educational use only\n\n"
                                 "Licensed under LGPL-3.0-or-later\n\n"
                                 "Third-party: ESP-IDF (Apache-2.0),\n"
                                 "LVGL (MIT), TFT_eSPI (FreeBSD/MIT),\n"
                                 "XPT2046_Touchscreen (MIT)",
                                 COL_DIM, &lv_font_montserrat_14,
                                 LV_ALIGN_DEFAULT, 0, 0);
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

/******************************************************************
 * 7c. Touch calibration
 *
 * The one constant in this file that cannot be a constant: resistive overlays
 * vary unit to unit, and the README used to answer a panel that taps 15px off
 * with "edit ui.cpp and rebuild". Two targets, then the operator confirms the
 * result by tapping a button drawn with it.
 ******************************************************************/

static void build_header(const char *title);   /* defined with the screens */

/* Crosshair rather than a filled dot: the target is the centre, and a dot
 * hides it under the finger that is aiming at it. */
static void cal_target(lv_coord_t cx, lv_coord_t cy) {
    const lv_coord_t arm = 12;
    static lv_point_t h[2], v[2];
    h[0] = { (lv_coord_t)(cx - arm), cy }; h[1] = { (lv_coord_t)(cx + arm), cy };
    v[0] = { cx, (lv_coord_t)(cy - arm) }; v[1] = { cx, (lv_coord_t)(cy + arm) };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *l = lv_line_create(lv_scr_act());
        lv_line_set_points(l, (i == 0) ? h : v, 2);
        lv_obj_set_style_line_width(l, 2, LV_PART_MAIN);
        lv_obj_set_style_line_color(l, COL_DANGER, LV_PART_MAIN);
        lv_obj_set_pos(l, 0, 0);
    }
}

/* Turn the two captured corners into the edge-to-edge range touch_to_screen()
 * maps against, and put it in force without storing it. Returns false on a
 * pair too close together to be two deliberate taps — which is what a stuck
 * panel or an impatient double-tap on one spot looks like. */
static bool cal_apply(void) {
    touch_cal_t c;
    if (!touch_cal_from_corners(s_cal_raw[0][0], s_cal_raw[0][1],
                                s_cal_raw[1][0], s_cal_raw[1][1],
                                CAL_INSET, SCR_W, SCR_H, &c)) {
        return false;
    }
    s_cal_xmin = c.x_min; s_cal_xmax = c.x_max;
    s_cal_ymin = c.y_min; s_cal_ymax = c.y_max;
    return true;
}

static void build_touch_cal(void) {
    clear_screen();

    if (s_cal_step < 2U) {
        const bool first = (s_cal_step == 0U);
        /* No header on these two: the targets sit in the corners, and the one
         * at the top left lands under a title bar's divider. Nothing is drawn
         * near a corner except the cross being aimed at. */
        lv_obj_t *ask = make_label(lv_scr_act(),
                   first ? "Tap the cross\nat the top left"
                         : "Now the one at\nthe bottom right",
                   COL_TEXT, &lv_font_montserrat_20, LV_ALIGN_CENTER, 0, -10);
        lv_obj_set_style_text_align(ask, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(ask, LV_ALIGN_CENTER, 0, -10);
        lv_obj_t *hint = make_label(lv_scr_act(),
                                    "Use a stylus or a fingernail - the centre "
                                    "of the cross, not near it.",
                                    COL_DIM, &lv_font_montserrat_14,
                                    LV_ALIGN_CENTER, 0, 60);
        lv_obj_set_width(hint, 200);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(hint, LV_ALIGN_CENTER, 0, 60);

        cal_target(first ? CAL_INSET : (SCR_W - 1 - CAL_INSET),
                   first ? CAL_INSET : (SCR_H - 1 - CAL_INSET));
        return;
    }

    /* Both corners captured. */
    build_header("Calibrate touch");
    if (!cal_apply()) {
        lv_obj_t *no = make_label(lv_scr_act(),
                   "Those two taps were\ntoo close together.\n\n"
                   "Nothing was changed.",
                   COL_DANGER, &lv_font_montserrat_20, LV_ALIGN_CENTER, 0, -20);
        lv_obj_set_style_text_align(no, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(no, LV_ALIGN_CENTER, 0, -20);
        make_button(lv_scr_act(), "Back", COL_ACCENT, COL_BG, 232, ACT_BTN_H,
                    LV_ALIGN_BOTTOM_MID, 0, ACT_BTN_Y, ACT_CAL_CANCEL,
                    &lv_font_montserrat_20);
        return;
    }

    /* The new map is live from here. Tapping Keep with it IS the test. */
    lv_obj_t *q = make_label(lv_scr_act(), "Keep this\ncalibration?", COL_TEXT,
                             &lv_font_montserrat_20, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_text_align(q, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(q, LV_ALIGN_TOP_MID, 0, 70);
    s_cal_countdown = make_label(lv_scr_act(), "", COL_DIM,
                                 &lv_font_montserrat_14,
                                 LV_ALIGN_TOP_MID, 0, 140);
    s_cal_deadline = lv_tick_get() + CAL_VERIFY_MS;

    make_button(lv_scr_act(), "Discard", COL_SURFACE, COL_TEXT, 104, ACT_BTN_H,
                LV_ALIGN_BOTTOM_LEFT, 10, ACT_BTN_Y, ACT_CAL_CANCEL,
                &lv_font_montserrat_20);
    make_button(lv_scr_act(), "Keep", COL_ACCENT, COL_BG, 104, ACT_BTN_H,
                LV_ALIGN_BOTTOM_RIGHT, -10, ACT_BTN_Y, ACT_CAL_SAVE,
                &lv_font_montserrat_20);
}

/* Sampling runs on the UI task, outside LVGL's input device: the calibration
 * screen is the one place that must read the panel before the mapping it is
 * measuring. The sample taken is the last one before release — a resistive
 * panel's first reading as the finger lands is its worst. */
static void touch_cal_poll(void) {
    if (s_cal_step >= 2U) { return; }

    int16_t rx, ry;
    if (touch_raw(&rx, &ry)) {
        s_cal_pressed = true;
        s_cal_last_x  = rx;
        s_cal_last_y  = ry;
        return;
    }
    if (!s_cal_pressed) { return; }

    s_cal_pressed = false;
    s_cal_raw[s_cal_step][0] = s_cal_last_x;
    s_cal_raw[s_cal_step][1] = s_cal_last_y;
    s_cal_step++;
    request_screen(UI_SCREEN_TOUCH_CAL);   /* next target, then the verify */
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
    /* The address, and it earns its line: joining the AP makes the phone open the
     * page by itself, in the Wi-Fi sign-in window — and that window cannot pick a
     * file, so the firmware update is the one job on the page it will not do. This
     * is what the operator opens in a real browser when that happens, and also the
     * answer when the page does not pop up at all.
     *
     * Three lines is what the card has left under the code and the credentials
     * (see ota_fit_card's ceiling); this is the third, so anything added here has
     * to replace one. */
    char note[96];
    snprintf(note, sizeof(note),
             "Or open 192.168.4.1\nVenue Wi-Fi off while open\nCloses in %u min",
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
     * this size costs two more lines than the card can spare — so step down.
     * Measured on the shown form: the 'v' is one of the twelve. */
    char staged_ver[OTA_VERSION_SHOWN_MAX];
    (void)ota_version_display(version, staged_ver, sizeof(staged_ver));
    lv_obj_t *ver = ota_text(card, cap, staged_ver, COL_TEXT,
                             (strlen(staged_ver) > 12U) ? &lv_font_montserrat_20
                                                        : &lv_font_montserrat_28);

    char running_ver[OTA_VERSION_SHOWN_MAX];
    char body[128];
    snprintf(body, sizeof(body),
             "Running %s.\n\nThe terminal restarts now. Not during a payment.",
             ota_version_display(ota_running_version(), running_ver,
                                 sizeof(running_ver)));
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
/* Which action opens step 2 for a network, in pos_net_t order. The pickers are
 * driven by assets.h now; this is the one thing left that is per-network and
 * belongs to this file, because an action is a UI concept. */
static const BtnAction NET_ACT[POS_NET__COUNT] = {
    ACT_NET_ETH, ACT_NET_POLY, ACT_NET_TRON
};

static void open_network_picker(void) {
    /* Same growth rule as step 2 — header + rows + the Cancel button — so a
     * fourth network added to assets.h widens the card rather than clipping. */
    lv_obj_t *card = open_modal(228,
        static_cast<lv_coord_t>(86 + (POS_NET__COUNT * (PILL_H + 2))));

    make_label(card, "Network", COL_DIM, &lv_font_montserrat_14,
               LV_ALIGN_TOP_MID, 0, 2);

    for (int i = 0; i < (int)POS_NET__COUNT; i++) {
        const pos_net_info_t *ni = pos_net_info(static_cast<pos_net_t>(i));
        /* Read here, not cached in a table: the subtitle names the deployment,
         * which is a setting. A function-local static would be initialised on the
         * first pass and then keep saying "Sepolia testnet" on a switched unit. */
        const char *sub = settings_net_str(ni->sub_test, ni->sub_main);
        /* A network with no payout address of its own is not offered. Otherwise a
         * terminal set up for Ethereum and interrupted before Tron would quietly
         * take Tron payments to the compile-time recipient — somebody else's
         * address — and look entirely normal doing it. */
        /* Polygon spends the Ethereum payout address — same EVM account, so the
         * one the operator stored works on both networks. */
        const bool have = settings_has_payout(i == (int)POS_NET_TRON);
        lv_coord_t y = static_cast<lv_coord_t>(22 + (i * (PILL_H + 2)));
        /* "No payout address" measured 133px against this pill's
         * PICK_W - PILL_TEXT_X - PILL_TEXT_PAD_R = 130px cap, so the one row
         * that explains why a network is greyed out arrived elided. */
        lv_obj_t  *p = make_pill(card, ni->name, have ? sub : "No payout set",
                                 PICK_W, y, NET_ACT[i]);
        lv_obj_align(make_net_badge(p, static_cast<pos_net_t>(i)),
                     LV_ALIGN_LEFT_MID, PILL_ICON_X, 0);
        if (!have) { pill_disable(p); }
    }

    make_button(card, "Cancel", COL_SURFACE, COL_TEXT, PICK_W, 40,
                LV_ALIGN_BOTTOM_MID, 0, -2, ACT_MODAL_CLOSE,
                &lv_font_montserrat_20);
}

/** Step 2 — the coin on the network chosen in step 1. */
static void open_coin_picker(pos_net_t net) {
    /* The rows are assets.h's, filtered on the network: three hand-kept tables
     * used to say the same thing here, and a fourth network meant a fourth table
     * plus another arm of the switch that chose between them. The table is ordered
     * for this loop — grouped by network, native coin first — so the order on
     * screen is the order there. No action column either: the chain IS the action
     * (ACT_CHAIN_OF). */
    size_t n = 0;
    for (size_t i = 0U; i < POS_ASSET_COUNT; i++) {
        if (POS_ASSETS[i].net == net) { n++; }
    }

    char title[24];
    (void)snprintf(title, sizeof(title), "Coin on %s", pos_net_info(net)->name);

    /* Card grows with the row count: header + rows + the Back button. */
    lv_obj_t *card = open_modal(228,
        static_cast<lv_coord_t>(86 + (n * (PILL_H + 2))));

    make_label(card, title, COL_DIM,
               &lv_font_montserrat_14, LV_ALIGN_TOP_MID, 0, 2);

    size_t row = 0;
    for (size_t i = 0U; i < POS_ASSET_COUNT; i++) {
        const pos_asset_t *a = &POS_ASSETS[i];
        if (a->net != net) { continue; }
        lv_coord_t y = static_cast<lv_coord_t>(22 + (row * (PILL_H + 2)));
        lv_obj_t  *p = make_pill(card, a->ticker, a->standard, PICK_W, y,
                                 ACT_CHAIN_OF(a->chain), true /* leaf */);
        lv_obj_align(make_asset_badge(p, a->chain),
                     LV_ALIGN_LEFT_MID, PILL_ICON_X, 0);
        row++;
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
static lv_obj_t *make_icon_button(const char *sym, BtnAction act,
                                  lv_obj_t *parent = NULL) {
    lv_obj_t *btn = lv_btn_create((parent != NULL) ? parent : lv_scr_act());
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

/* Which deployment, on the screen that takes the money. Nothing else in the
 * payment flow says test or production, and a firmware update with a new
 * BUILD_ID wipes NVS and brings the unit back on mainnet — so a terminal
 * somebody configured for testnet can start taking real payments with every
 * screen looking normal. Production stays silent, the same rule
 * settings_net_str follows: mainnet names carry no suffix, so the chip's
 * presence IS the warning.
 *
 * In the burger's band, opposite the burger, and NOT on the amount's row. It
 * was inside the asset pill first, which was wrong in a way worth recording:
 * amount_row_place() gives the figure whatever the pill leaves, so a chip in
 * the pill is width taken directly off the digits, and the amount walked off
 * the left edge of the screen at six of them. This band is empty from the
 * burger's right edge to x=240 and no layout reads it. */
static void add_test_chip(void) {
    if (settings_get_mainnet()) { return; }
    lv_obj_t *chip = lv_label_create(lv_scr_act());
    lv_label_set_text(chip, "TEST");
    lv_obj_set_style_text_color(chip, COL_BG, LV_PART_MAIN);
    lv_obj_set_style_text_font(chip, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(chip, COL_DANGER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(chip, 7, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(chip, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    /* Straight after the clock. It was top-LEFT, which is the clock's corner
     * now; it cannot go right, because that end is the Wi-Fi mark's and the mark
     * is on the top layer, so a right-aligned chip is drawn underneath it; and
     * it cannot go in the middle any more, because the rail is there. So it sits
     * in the band's one remaining slot, and the rail starts after it — see
     * build_page(), which reserves CHIP_W here whenever the chip is drawn. */
    lv_obj_align(chip, LV_ALIGN_TOP_LEFT, CHIP_X, CHIP_Y);
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
static lv_obj_t *make_title(const char *txt, bool has_icon,
                            lv_obj_t *parent = NULL) {
    /* Symmetric, so the title stays optically centred on the screen rather than
     * centred in the leftover space beside the button. Width comes from the
     * parent when there is one: on the sale flow this is drawn in the card, and
     * a gutter measured off SCR_W would centre it against the wrong box. */
    lv_obj_t *host = (parent != NULL) ? parent : lv_scr_act();
    lv_obj_update_layout(host);
    lv_coord_t hw = lv_obj_get_width(host);
    if (hw <= 0) { hw = SCR_W; }   /* not laid out yet — assume full width */
    const lv_coord_t gutter = has_icon ? (MENU_BTN_X + MENU_BTN_W + 4) : 8;
    const lv_coord_t avail  = hw - (2 * gutter);

    const bool small = lv_txt_get_width(txt, strlen(txt), &lv_font_montserrat_20,
                                        0, LV_TEXT_FLAG_NONE) > avail;
    /* +4 keeps the shorter 14px cap optically level with the 20px arrow glyph
     * beside it, which is drawn from the same HDR_TITLE_Y baseline. */
    lv_obj_t *l = make_label(host, txt, COL_TITLE,
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

        /* Only in this branch: a card is read from the browser's setup page, so
         * there is no route to a failed read that is not already past the QR
         * code. */
        if (s_prov_msg[0] != '\0') {
            lv_obj_t *m = make_label(lv_scr_act(), s_prov_msg, COL_DANGER,
                                     &lv_font_montserrat_14,
                                     LV_ALIGN_TOP_MID, 0, 154);
            lv_obj_set_width(m, SCR_W - 40);
            lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            lv_obj_align(m, LV_ALIGN_TOP_MID, 0, 154);
        }
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

    /* Stacked and measured, not placed at four hand-written offsets — the same
     * ota_text/ota_fit_card pattern the update modals in this file already use, and
     * for the reason written there: nothing on this card has a known height.
     *
     * Three of the four blocks vary. The address wraps to two lines or three
     * depending on the network and the glyphs it happens to contain; the label runs
     * from "ERC-20 token contract" to "Ethereum / Polygon payout address"; and the
     * warning is a sentence. The old layout put the warning at a fixed y=118 and the
     * buttons at a fixed offset off the bottom of a fixed 276px card, so a block
     * that came out one line taller than the strings it was measured on ran under
     * the Accept button — which is the one control on this screen that must be
     * legible, on the one screen that decides where a merchant's takings go.
     *
     * The label wraps now instead of dot-eliding. It was elided to fit one line,
     * and "Ethereum / Polygon payout add…" is a poor thing to read on the screen
     * that asks you to check an address character by character. */
    lv_obj_t *card = open_modal(OTA_CARD_W, 276);

    lv_obj_t *t = ota_text(card, NULL,
                           contract ? "Set token contract?"
                                    : "Set payout address?",
                           COL_TEXT, &lv_font_montserrat_20);
    lv_obj_t *l = ota_text(card, t, label, COL_DIM, &lv_font_montserrat_14);
    lv_obj_t *a = ota_text(card, l, addr, COL_TEXT, &lv_font_montserrat_14);
    /* Both cut to the sentence that changes a decision. The card grows to whatever
     * it is given, but only up to ota_fit_card's ceiling of one screen — past that
     * the buttons come back up over the text, which is the failure this rewrite is
     * fixing. A line that only restates the button under it ("...before
     * accepting") is the first thing to spend. */
    lv_obj_t *warn = ota_text(card, a,
                              contract
                              ? "A wrong contract charges a different asset."
                              : "Takings will be sent here. Check it against "
                                "your own records.",
                              COL_DIM, &lv_font_montserrat_14);
    ota_fit_card(card, warn, 86);   /* two 40px buttons at -46 and -2 */

    /* Reject is the wide, plainly-labelled one and Accept is the deliberate tap:
     * the safe answer to "a stranger's address appeared on my terminal" is no. */
    (void)make_button(card, "Accept", COL_ACCENT, COL_BG, OTA_TEXT_W, 40,
                      LV_ALIGN_BOTTOM_MID, 0, -46, ACT_PROV_OK,
                      &lv_font_montserrat_20);
    (void)make_button(card, "Reject", COL_SURFACE, COL_TEXT, OTA_TEXT_W, 40,
                      LV_ALIGN_BOTTOM_MID, 0, -2, ACT_PROV_NO,
                      &lv_font_montserrat_20);
}

/* "Hold your card to the reader" while an address is derived from it. Its own
 * screen rather than the transaction one: nothing is being paid, and that screen's
 * wording and its Cancel semantics both belong to a sale. */
static void build_card_wait(void) {
    /* Same chrome as the sale, deliberately without a lit dash: reading an
     * address off a card during setup is not a step of anybody's payment. */
    lv_obj_t *card = build_page(PAY_STEP_NONE);
    const lv_coord_t VW = CARD_W - (2 * CARD_PAD);

    make_label(card, "Cryptnox card", COL_DIM, &lv_font_montserrat_14,
               LV_ALIGN_TOP_MID, 0, 14);

    make_tap_mark(card, 40);

    make_label(card, "Hold card to reader", COL_TEXT,
               &lv_font_montserrat_20, LV_ALIGN_TOP_MID, 0, 146);

    lv_obj_t *info = make_label(card, s_card_note, COL_DIM,
                                &lv_font_montserrat_14, LV_ALIGN_TOP_MID, 0, 176);
    lv_obj_set_width(info, VW);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(info, LV_ALIGN_TOP_MID, 0, 176);

    (void)make_ghost_button(card, "Cancel", CARD_BTN_W,
                            LV_ALIGN_BOTTOM_MID, 0, CARD_BTN_Y, ACT_CANCEL);
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
    /* Step one of four. The burger that used to sit top-left is gone — the
     * admin panel is behind the swipe up from the home indicator now, so the
     * only chrome left on the teal is the step dashes and the test chip. */
    lv_obj_t *card = build_page(PAY_STEP_AMOUNT);
    add_test_chip();

    /* One row at 47..89: the amount centred on the screen, the asset selector at
     * its right-hand end. Taking money means reading a figure and a currency
     * together, and a selector centred on its own line above the figure was a
     * heading for it instead.
     *
     * The pill carries the ticker, so the figure is a bare number: the group is
     * centred in the line minus the pill rather than on the screen, and both are
     * re-measured whenever either changes — see amount_row_place().
     *
     * The card's 262px column is fully spoken for: the figure's row at 10..52,
     * keypad 58..206 and the Charge button 210..254 below it. Keys come out 37
     * tall rather than the 44 they had full-screen — the card's inset is paid
     * for out of the keypad, which is the only thing here with slack. */
    make_asset_button(card);

    /* Figure and small cents in one content-sized flex row, so the group centres
     * itself whatever the fonts measure — nothing here has to know how wide
     * montserrat_28 draws a 5. Bottom-aligned across the row, which puts the small
     * cents on the big font's baseline, near enough (its descender is a pixel or
     * two, and a real baseline align is not on offer in LVGL 8). */
    lv_obj_t *row = lv_obj_create(card);
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

    /* Numeric keypad: digits, double-zero, backspace (cents entry). */
    static const char *amap[] = {
        "1", "2", "3", "\n",
        "4", "5", "6", "\n",
        "7", "8", "9", "\n",
        "00", "0", LV_SYMBOL_BACKSPACE, ""
    };
    lv_obj_t *kb = lv_btnmatrix_create(card);
    lv_btnmatrix_set_map(kb, amap);
    lv_obj_set_size(kb, CARD_W - (2 * CARD_PAD), 148);
    lv_obj_align(kb, LV_ALIGN_TOP_MID, 0, 58);
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

    s_charge_btn = make_button(card, "Charge", COL_ACCENT, COL_BG,
                               CARD_BTN_W, CARD_BTN_H,
                               LV_ALIGN_BOTTOM_MID, 0, CARD_BTN_Y, ACT_CONFIRM,
                               &lv_font_montserrat_20);

    /* Also settles the Charge button: the screen is rebuilt on an asset change
     * with whatever was typed still standing, so it must not come back lit on an
     * empty figure or dead on a full one. */
    amount_update_display();   /* keep s_amount_units in sync with the string */
}

static void build_confirm(void) {
    /* Step two of four. */
    lv_obj_t *card = build_page(PAY_STEP_REVIEW);

    /* Ledger-style transaction review: dim caption / value rows, everything
     * the operator should verify — amount, beneficiary AND the USDC contract
     * the terminal is about to call.
     *
     * All coordinates are the card's. The 216px value width the addresses used
     * full-screen becomes the card's own usable width, or they would wrap
     * against a box wider than the one they are drawn in. */
    const lv_coord_t VW = CARD_W - (2 * CARD_PAD);
    char buf[24];
    format_amount(s_confirm_amount, buf, sizeof(buf));

    /* The vertical budget, because it is fully spent and the rows below are not
     * free to drift. 262px of card: the Total block to 58, the two address rows
     * to 180 at their two-line worst case, the test warning to 203, and the
     * buttons from 208. Both address rows DO hit two lines — a 42-character 0x
     * address measures ~336px against a 204px column — so the worst case is the
     * normal case and it has 5px of slack. Move anything down and the warning
     * goes back under the buttons, which is the bug this spacing fixes. */
    make_label(card, "Total", COL_DIM, &lv_font_montserrat_14,
               LV_ALIGN_TOP_LEFT, CARD_PAD, 8);

    /* What is being charged, and — on a test build — where. Opposite "Total" on
     * its own row, so it costs no height: the rows below have none to give.
     *
     * The amount screen says this in an icon and a TEST chip; this is the last
     * screen before the card is tapped and it is the one that should spell out
     * both. Mainnet names the coin and stops there, the same rule
     * settings_net_str follows — a production terminal should not be shouting a
     * network name nobody needs, which is what makes the testnet form stand out.
     * Red on testnet for the same reason the chip is. */
    /* The asset is named beside its own mark below now, the way the amount
     * screen's selector does it — so this row carries only what that cannot
     * say: which deployment. Mainnet stays silent, the rule settings_net_str
     * follows throughout, which is what makes the testnet form stand out. */
    const bool mainnet = settings_get_mainnet();
    if (!mainnet) {
        make_label(card, pos_net_info(asset()->net)->sub_test, COL_DANGER,
                   &lv_font_montserrat_14, LV_ALIGN_TOP_RIGHT, -CARD_PAD, 8);
    }

    lv_obj_t *amt = make_label(card, buf, COL_TEXT, &lv_font_montserrat_28,
                               LV_ALIGN_TOP_LEFT, CARD_PAD, 24);
    lv_obj_t *cusdc = make_asset_badge(card, settings_get_chain());
    lv_obj_align_to(cusdc, amt, LV_ALIGN_OUT_RIGHT_MID, 6, 0);
    /* Ticker beside the mark, as on the amount screen: the mark alone says
     * which asset to somebody who already knows the logos, and this is the
     * screen where that assumption is worth the least — it is the last one
     * before the card is tapped.
     *
     * The gaps are 6 and 4 rather than 8 and 8 because this row has a ceiling:
     * "99999.99" at montserrat_28 runs to about x=130, the 36px badge to 172,
     * and a four-letter ticker to 212 against the card's 214. It fits, with the
     * tighter gaps and not without them. */
    lv_obj_t *tick = make_label(card, asset_name(), COL_TEXT,
                                &lv_font_montserrat_14, LV_ALIGN_DEFAULT, 0, 0);
    lv_obj_align_to(tick, cusdc, LV_ALIGN_OUT_RIGHT_MID, 4, 0);

    make_label(card, "To", COL_DIM, &lv_font_montserrat_14,
               LV_ALIGN_TOP_LEFT, CARD_PAD, 64);
    lv_obj_t *addr = make_label(card,
                                s_confirm_addr[0] ? s_confirm_addr : "-",
                                COL_TEXT, &lv_font_montserrat_14,
                                LV_ALIGN_TOP_LEFT, CARD_PAD, 82);
    lv_label_set_long_mode(addr, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(addr, VW);

    make_label(card, asset_caption(),
               COL_DIM, &lv_font_montserrat_14, LV_ALIGN_TOP_LEFT, CARD_PAD, 126);
    lv_obj_t *ctr = make_label(card,
                               (s_addr_usdc != NULL) ? s_addr_usdc : "-",
                               COL_TEXT, &lv_font_montserrat_14,
                               LV_ALIGN_TOP_LEFT, CARD_PAD, 144);
    lv_label_set_long_mode(ctr, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ctr, VW);

    /* The warning in words, under the contract rather than at a y of its own:
     * that row wraps to one or two lines depending on the address family, so the
     * line below it cannot be a constant. Deliberately NOT given a width — a
     * wrapped second line here is exactly what used to run under the buttons,
     * and the sentence measures ~190px against the card's 204. */
    if (!mainnet) {
        lv_obj_t *tn = make_label(card, "Test network - no real funds",
                                  COL_DANGER, &lv_font_montserrat_14,
                                  LV_ALIGN_DEFAULT, 0, 0);
        lv_obj_update_layout(ctr);
        lv_obj_align_to(tn, ctr, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 6);
    }

    /* Cancel is the card-wait screen's ghost — white with a hairline, not a grey
     * slab. Backing out of a sale costs nothing and should read as available
     * without competing with the button that commits. */
    const lv_coord_t half = (CARD_W - (2 * CARD_PAD) - 8) / 2;
    make_ghost_button(card, "Cancel", half,
                      LV_ALIGN_BOTTOM_LEFT, CARD_PAD, CARD_BTN_Y, ACT_CANCEL);
    make_button(card, "Confirm", COL_ACCENT, COL_BG, half, CARD_BTN_H,
                LV_ALIGN_BOTTOM_RIGHT, -CARD_PAD, CARD_BTN_Y, ACT_SEND,
                &lv_font_montserrat_20);
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
        strncpy(s_card_note, "Reading your payout addresses",
                sizeof(s_card_note) - 1);
        s_card_note[sizeof(s_card_note) - 1] = '\0';
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
 * keypad is the only input. Shared by the card PIN and the admin code.
 *
 * Reveal is offered on the PIN only, by the caller: the field is 160px centred, so
 * an eye beside it has 30px of margin to sit in either way, but the two screens
 * are not the same question. The card PIN is the one people mistype and cannot
 * check — the card locks after a few tries — and the operator holding the card is
 * the person entitled to see it. The admin code is entered by the same person into
 * the same panel and is not worth the button. */
/* Show the hint while the field is empty, hide it the moment anything is typed.
 * Driven by the textarea's own VALUE_CHANGED, which LVGL sends from all four
 * mutating calls (add_char, add_text, del_char, set_text) — so backspacing back to
 * empty brings it back, and the reset in build_pin/admin_submit does too, with no
 * caller having to remember. */
static void code_hint_cb(lv_event_t *e) {
    lv_obj_t *ta   = lv_event_get_target(e);
    lv_obj_t *hint = static_cast<lv_obj_t *>(lv_event_get_user_data(e));
    const char *txt = lv_textarea_get_text(ta);
    if (hint == NULL) { return; }
    if ((txt != NULL) && (txt[0] != '\0')) {
        lv_obj_add_flag(hint, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(hint, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *make_code_field(uint32_t max_len, lv_coord_t x, lv_coord_t y,
                                 const char *hint, lv_obj_t *parent = NULL) {
    lv_obj_t *host = (parent != NULL) ? parent : lv_scr_act();
    lv_obj_t *ta = lv_textarea_create(host);
    lv_textarea_set_password_mode(ta, true);
    /* Echo each digit briefly so the operator can confirm the keypress, then
     * mask — a third of LVGL's 1500 ms default, which leaks the whole code. */
    lv_textarea_set_password_show_time(ta, 500);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, max_len);
    lv_textarea_set_text(ta, "");
    lv_obj_clear_flag(ta, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(ta, 160);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, x, y);
    lv_obj_set_style_text_align(ta, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ta, COL_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_text_color(ta, COL_TEXT, LV_PART_MAIN);
    lv_obj_set_style_border_color(ta, COL_BORDER, LV_PART_MAIN);

    /* What to type, in the box it is typed into. The panel's own screens are a
     * title, an empty box and a keypad, and a title naming the OUTCOME —
     * "Authorise browser" — left the operator with nothing on screen saying the
     * thing wanted was their admin code.
     *
     * A label of our own, NOT lv_textarea's placeholder. The placeholder cannot be
     * centred in a one-line textarea: draw_placeholder() sets LV_TEXT_FLAG_EXPAND
     * for one_line fields, which tells the renderer to ignore the area's width, and
     * a centre alignment inside an area of ignored width is a left alignment. So it
     * drew hard against the left inset while the digits it labels are centred —
     * which is what this reads as on the panel, a caption belonging to something
     * else. Aligned to the field itself, so it follows a field the caller has
     * shifted (the PIN screen moves its box left to make room for the eye). */
    if (hint != NULL) {
        lv_obj_t *l = make_label(host, hint, COL_DIM,
                                 &lv_font_montserrat_14, LV_ALIGN_DEFAULT, 0, 0);
        lv_obj_update_layout(ta);
        lv_obj_align_to(l, ta, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_event_cb(ta, code_hint_cb, LV_EVENT_VALUE_CHANGED, l);
    }
    return ta;
}

/* Numeric keypad: no key boxes — black glyphs on white, grey flash on press.
 * The map is static because lv_btnmatrix keeps the pointer. */
static lv_obj_t *make_numeric_keypad(lv_event_cb_t cb, lv_obj_t *parent = NULL,
                                     lv_coord_t w = 232, lv_coord_t h = 210) {
    static const char *kbd_map[] = {
        "1", "2", "3", "\n",
        "4", "5", "6", "\n",
        "7", "8", "9", "\n",
        LV_SYMBOL_BACKSPACE, "0", LV_SYMBOL_OK, ""
    };
    lv_obj_t *kb = lv_btnmatrix_create((parent != NULL) ? parent : lv_scr_act());
    lv_btnmatrix_set_map(kb, kbd_map);
    lv_obj_set_size(kb, w, h);
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
    /* Step three of four — authorising the card. A card read during first-run
     * setup borrows this screen too, and that is not a sale, so it gets the
     * chrome without a lit dash. */
    lv_obj_t *card = build_page(s_pin_for_card ? PAY_STEP_NONE : PAY_STEP_AUTH);
    /* Wipe any stale PIN from a previous attempt. */
    CW_Utils::secure_wipe(reinterpret_cast<uint8_t *>(s_pin), sizeof(s_pin));
    s_pin_len = 0;

    (void)make_title(s_pin_for_card ? "Card PIN" : "Enter PIN", true, card);
    /* Back (cancel) icon, top-left of the card. */
    (void)make_icon_button(LV_SYMBOL_LEFT, ACT_PIN_CANCEL, card);

    /* Field and eye centred as a pair, not the field alone: the button is 42 wide
     * with a 6px gap, so the field gives up half of that and the group's middle
     * stays on the keypad's centre line. Centring the field itself would put the
     * eye 8px off the right edge of a 240px panel. Passed in rather than re-aligned
     * afterwards, so the hint inside the box is placed against the final position. */
    s_pin_ta = make_code_field(9U, -(MENU_BTN_W + 6) / 2, 44, "Card PIN", card);

    /* Same reveal the Wi-Fi passphrase has, and for the same reason: a PIN typed
     * blind on a resistive panel and refused tells the operator nothing about which
     * of the two got it wrong — except that here the card counts the attempt, and
     * runs out of them. Masked by default, because this screen faces the customer.
     * make_icon_button() places itself top-left for the burger; move it beside the
     * field, and keep the label handle so the glyph can be swapped in place. */
    lv_obj_t *eye = make_icon_button(LV_SYMBOL_EYE_OPEN, ACT_PIN_REVEAL, card);
    lv_obj_align_to(eye, s_pin_ta, LV_ALIGN_OUT_RIGHT_MID, 6, 0);
    s_pin_eye_lbl = lv_obj_get_child(eye, 0);

    (void)make_numeric_keypad(pin_kbd_cb, card, CARD_W - (2 * CARD_PAD), 170);
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
static void build_admin_screen(const char *title, bool allow_cancel,
                               const char *hint) {
    /* Into the rising sheet when there is one, and then WITHOUT clearing the
     * screen: the sale screen underneath is what the sheet slides over. Every
     * other entry to this screen is an ordinary full-screen rebuild. */
    lv_obj_t *host = s_sheet;
    if (host == NULL) {
        clear_screen();
        host = lv_scr_act();
    }

    (void)make_title(title, allow_cancel, host);
    if (allow_cancel) {
        (void)make_icon_button(LV_SYMBOL_LEFT, ACT_ADMIN_CANCEL, host);
    }

    s_admin_ta = make_code_field(ADMIN_CODE_MAX, 0, 44, hint, host);

    /* Note band above the keypad: wrong code, mismatch, or the remaining wait. */
    s_admin_note_lbl = make_label(host, s_admin_note, COL_DANGER,
                                  &lv_font_montserrat_14, LV_ALIGN_TOP_MID, 0, 82);
    (void)make_numeric_keypad(admin_kbd_cb, host);
}

static void build_admin_set(void) {
    /* No way out: first-run setup is mandatory, since the whole menu — including
     * the factory reset — hides behind this code. */
    build_admin_screen(s_admin_confirming ? "Confirm code" : "Set admin code",
                       false,
                       s_admin_confirming ? "Type it again" : "New admin code");
}

static void build_admin_unlock(void) {
    /* Same screen either way — the escalating lockout, the note band and the
     * keypad are what this is, and only the door it opens differs.
     *
     * The hint does not differ, and that is the point: the title says which door,
     * the box says what the key is. "Authorise browser" over an empty field named
     * the outcome and left the operator to guess that the terminal wanted the
     * admin code — the one thing on that screen they had to know. */
    build_admin_screen(s_admin_for_portal ? "Authorise browser" : "Admin code",
                       true, "Admin code");
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
     * the field. Keep the label handle so the glyph can be swapped in place. Same
     * pair on the card-PIN screen — see build_pin(). */
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

/* "0x1234...abcd" — head and tail of a transaction hash. 66 characters do not
 * fit on a 240px panel at a readable size, and the ends are what somebody
 * matches against a block explorer. Tron hashes carry no 0x, which is why this
 * takes six characters from the front rather than skipping a prefix. */
static void hash_short(const char *h, char *out, size_t n) {
    size_t len = strlen(h);
    if ((len <= 14U) || (n < 16U)) {
        snprintf(out, n, "%s", h);
        return;
    }
    snprintf(out, n, "%.6s...%s", h, h + len - 4U);
}

/* "<amount> [USDC logo]" centred at offset y from the top. */
static void tx_amount_row(lv_obj_t *parent, const char *amt,
                          const lv_font_t *font, lv_coord_t y) {
    lv_obj_t *al = make_label(parent, amt, COL_TEXT, font,
                              LV_ALIGN_TOP_MID, -16, y);
    lv_obj_t *u  = make_asset_badge(parent, settings_get_chain());
    lv_obj_align_to(u, al, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
}

static void build_tx_status(void) {
    /* The last step of the sale — and the one the reference photo is of:
     * "Total", the figure, the prompt, the contactless mark, one way out. */
    lv_obj_t *card = build_page(PAY_STEP_TAP);
    const lv_coord_t VW = CARD_W - (2 * CARD_PAD);

    char amt[24];
    format_amount(s_confirm_amount, amt, sizeof(amt));

    if (s_tx_state == UI_TX_STATE_PLACE_CARD) {
        make_label(card, "Total", COL_DIM, &lv_font_montserrat_14,
                   LV_ALIGN_TOP_MID, 0, 14);
        tx_amount_row(card, amt, &lv_font_montserrat_28, 32);

        make_label(card, "Tap your card", COL_DIM, &lv_font_montserrat_20,
                   LV_ALIGN_TOP_MID, 0, 78);

        make_tap_mark(card, 110);

        (void)make_ghost_button(card, "Cancel", CARD_BTN_W,
                                LV_ALIGN_BOTTOM_MID, 0, CARD_BTN_Y, ACT_CANCEL);
        return;
    }

    if (s_tx_state == UI_TX_STATE_DONE) {
        lv_obj_t *chk = make_label(card, LV_SYMBOL_OK, COL_SUCCESS,
                                   &lv_font_montserrat_48, LV_ALIGN_TOP_MID, 0, 30);
        pop_in(chk);
        make_label(card, "Approved", COL_TEXT, &lv_font_montserrat_20,
                   LV_ALIGN_TOP_MID, 0, 92);
        tx_amount_row(card, amt, &lv_font_montserrat_28, 122);

        /* The hash main hands this screen with the DONE state. It used to be
         * dropped on the floor, which left the merchant a settled sale they
         * could not look up — the one screen where the number is worth having
         * was the only one not showing it. */
        if (s_tx_info[0] != '\0') {
            char shrt[24];
            hash_short(s_tx_info, shrt, sizeof(shrt));
            make_label(card, shrt, COL_DIM, &lv_font_montserrat_14,
                       LV_ALIGN_TOP_MID, 0, 168);
        }

        make_button(card, "New sale", COL_ACCENT, COL_BG, CARD_BTN_W, CARD_BTN_H,
                    LV_ALIGN_BOTTOM_MID, 0, CARD_BTN_Y, ACT_NEW,
                    &lv_font_montserrat_20);
        return;
    }

    if (s_tx_state == UI_TX_STATE_FAILED) {
        lv_obj_t *cross = make_label(card, LV_SYMBOL_CLOSE, lv_color_hex(0xEC5B5B),
                                     &lv_font_montserrat_48, LV_ALIGN_TOP_MID, 0, 30);
        pop_in(cross);
        make_label(card, "Declined", COL_TEXT,
                   &lv_font_montserrat_20, LV_ALIGN_TOP_MID, 0, 92);
        lv_obj_t *info = make_label(card, s_tx_info, COL_DIM,
                                    &lv_font_montserrat_14, LV_ALIGN_TOP_MID, 0, 124);
        lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(info, VW);
        lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(info, LV_ALIGN_TOP_MID, 0, 124);

        make_button(card, "New sale", COL_ACCENT, COL_BG, CARD_BTN_W, CARD_BTN_H,
                    LV_ALIGN_BOTTOM_MID, 0, CARD_BTN_Y, ACT_NEW,
                    &lv_font_montserrat_20);
        return;
    }

    /* PROCESSING / SIGNING / SENDING — animated spinner + status text. The
     * spinner keeps turning because lv_timer_handler runs on the UI task while
     * the main task is busy connecting/signing/broadcasting. */
    const char *state_str =
        (s_tx_state == UI_TX_STATE_PROCESSING) ? "Processing" :
        (s_tx_state == UI_TX_STATE_SIGNING)    ? "Signing"    :
        (s_tx_state == UI_TX_STATE_CONFIRMING) ? "Confirming" : "Authorizing";

    lv_obj_t *sp = lv_spinner_create(card, 1000, 60);
    lv_obj_set_size(sp, 60, 60);
    lv_obj_align(sp, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_set_style_arc_width(sp, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(sp, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(sp, COL_SURFACE, LV_PART_MAIN);      /* track */
    lv_obj_set_style_arc_color(sp, COL_ACCENT, LV_PART_INDICATOR);  /* moving arc */

    make_label(card, state_str, COL_TEXT, &lv_font_montserrat_20,
               LV_ALIGN_TOP_MID, 0, 112);

    /* Held: "Confirming" can stand for two minutes, and ui_set_tx_info() writes
     * the countdown straight into this label — rebuilding the screen per tick
     * would restart the spinner above it. */
    s_tx_info_lbl = make_label(card, s_tx_info, COL_DIM,
                               &lv_font_montserrat_14, LV_ALIGN_TOP_MID, 0, 146);
    lv_label_set_long_mode(s_tx_info_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_tx_info_lbl, VW);
    lv_obj_set_style_text_align(s_tx_info_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(s_tx_info_lbl, LV_ALIGN_TOP_MID, 0, 146);
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

/******************************************************************
 * 8b. Wi-Fi signal — the wave, top right of every screen
 *
 * Three arcs over a dot, the shape every phone uses, rather than the four
 * ascending bars this started as: the fan is read at a glance from across a
 * counter, where four 3px bars were a smudge.
 *
 * The dot is 4px, not 3: LVGL clamps the circle radius to half the side, so at
 * 3px there is nothing left to round and it drew as a small black square.
 *
 * The sizes below are one system, not four numbers. The dot reaches out to
 * SIG_DOT/2 and each arc covers r ± SIG_AW/2, so nothing may start inside what
 * came before it or the icon closes up into a blob:
 *   dot to first ring:  SIG_R0 - (SIG_AW / 2) - (SIG_DOT / 2)
 *   ring to ring:       SIG_RSTEP - SIG_AW
 * Both are 2px, and they have to agree: the dot is the first thing in the stack,
 * so a tighter gap under the first ring than between the rings reads as the dot
 * stuck to it. That is what SIG_R0 is for — at 4 the dot's gap was 1 against the
 * rings' 2, and at 3 (what this was drawn with) the two met outright.
 *
 * Lives on the top layer rather than being built per screen: it outlives the
 * screen swaps, so no build_* function has to remember it. A modal is created
 * on the same layer later, so it covers the wave — which is what a modal is for.
 ******************************************************************/
#define SIG_ARCS   3
#define SIG_AW     2      /* arc thickness                                  */
/* Shrunk from 5/4 — the whole mark is 22x13 now against the 28x16 it was. The
 * two gaps the block above insists must agree still do: dot to first ring is
 * SIG_R0 - (SIG_AW / 2) - (SIG_DOT / 2) = 1, and ring to ring is
 * SIG_RSTEP - SIG_AW = 1. They were 2 and 2 at the old sizes; what matters is
 * that they are equal, not what they are. At r=4 the inner ring's 140-degree
 * cut still bends 2.6px, so it reads as an arc rather than as a dash. */
#define SIG_R0     4      /* innermost arc radius; each ring adds SIG_RSTEP */
#define SIG_RSTEP  3
/* Even, and it has to stay even: an arc object is 2r + SIG_AW wide, so the fan's
 * centre lands on a whole coordinate, and only an even dot has its own centre
 * there too. At 5 it sat half a pixel right of and below the arcs it is struck
 * from — small, but at this size half a pixel is the thing you notice. */
#define SIG_DOT    4
/* Per-ring sweep, innermost first, centred on 12 o'clock. Two jobs. The inner
 * rings get more of their circle because how curved an arc looks is its sagitta,
 * r·(1 - cos(sweep/2)), and a 90° cut at r=5 bends 1.5px — a dash; at 120° it
 * bends 3.3px and reads as an arc. Second, the whole set sets the icon's width —
 * a ring is 2r·sin(sweep/2) across — so widening it is done here rather than by
 * pushing the radii out, which would grow the height with it. 9.4 / 14.7 / 19.9
 * across at these angles, against 8.7 / 12.7 / 17 before.
 *
 * The inner ring is the ceiling: at r=5 no angle can span more than 10px, and
 * past about 140° it stops looking like a cut from a circle and starts looking
 * like most of one. Widening beyond this means a bigger SIG_R0. */
static const uint16_t SIG_SWEEP[SIG_ARCS] = { 140, 110, 100 };

/* The outermost ring's outer edge, which is the icon's half-width and, with the
 * dot's bottom half, its height. Everything below is derived from it. */
#define SIG_REACH  (SIG_R0 + ((SIG_ARCS - 1) * SIG_RSTEP) + (SIG_AW / 2))
#define SIG_W      (2 * SIG_REACH)
#define SIG_H      (SIG_REACH + ((SIG_DOT + 1) / 2))
/* Inset from the right edge. The mark and this together are the band's
 * right-hand zone, which the progress rail stops short of — and it reserves
 * that zone by a constant of its own, because this section is below it in the
 * file. Resize the mark and this assert is what tells you the rail no longer
 * clears it. */
#define SIG_INSET  14
static_assert((SIG_W + SIG_INSET) <= SIG_ZONE_W,
              "Wi-Fi mark outgrew SIG_ZONE_W - the progress rail would run under it");

static lv_obj_t *s_sig_arc[SIG_ARCS];
static lv_obj_t *s_sig_dot = NULL;
static lv_obj_t *s_sig_box = NULL;

static void signal_refresh(lv_timer_t *t) {
    (void)t;
    int8_t rssi = 0;
    /* net_wifi_rssi() fails when the station is not associated — grey dot, no
     * arc, which is the answer the operator needs before blaming the card.
     * The cuts match the words on the settings page (Good / Fair / Weak), so
     * the icon and the "Signal" field there cannot disagree. */
    const bool up  = net_wifi_rssi(&rssi);
    const int  lit = !up ? 0 : (rssi >= -60) ? 3 : (rssi >= -70) ? 2 : 1;
    for (int i = 0; i < SIG_ARCS; i++) {
        /* Unlit is COL_DIM, not COL_BORDER: the sale flow's page IS COL_BORDER,
         * so a hairline-grey arc vanished into it and two arcs read as two arcs
         * total. Same trap the step dashes fell into — see COL_PAGE. */
        lv_obj_set_style_arc_color(s_sig_arc[i],
                                   (i < lit) ? COL_TEXT : COL_DIM,
                                   LV_PART_MAIN);
    }
    lv_obj_set_style_bg_color(s_sig_dot, up ? COL_TEXT : COL_DIM, LV_PART_MAIN);
    /* Off the splash (nothing is connected yet), off the calibration screen
     * where it would sit on the top-right corner target, and off the admin
     * screens — the settings page's tab bar owns y=0..42 across the full width,
     * so a mark on the top layer is drawn over the first tab. The signal is
     * reported properly on that page anyway, as the Wi-Fi tab's own Signal row,
     * in words rather than as three arcs on top of a button. */
    const bool show = (s_req_screen != UI_SCREEN_SPLASH) &&
                      (s_req_screen != UI_SCREEN_TOUCH_CAL) &&
                      (s_req_screen != UI_SCREEN_SETTINGS) &&
                      (s_req_screen != UI_SCREEN_ADMIN_UNLOCK) &&
                      (s_req_screen != UI_SCREEN_ADMIN_SET);
    if (show) { lv_obj_clear_flag(s_sig_box, LV_OBJ_FLAG_HIDDEN); }
    else      { lv_obj_add_flag(s_sig_box, LV_OBJ_FLAG_HIDDEN); }

    /* Same cadence, same reason — the band's two occupants refresh together. */
    clock_refresh();
}

static void signal_init(void) {
    s_sig_box = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_sig_box);
    lv_obj_set_size(s_sig_box, SIG_W, SIG_H);
    lv_obj_align(s_sig_box, LV_ALIGN_TOP_RIGHT, -SIG_INSET, 6);
    lv_obj_clear_flag(s_sig_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_sig_box, LV_OBJ_FLAG_CLICKABLE);

    /* Every ring is struck from the same point — the dot — so each arc object is
     * sized to its own ring and then centred on that point, not on the box. */
    const lv_coord_t cx = SIG_W / 2, cy = SIG_REACH;
    for (int i = 0; i < SIG_ARCS; i++) {
        const lv_coord_t d = (2 * (SIG_R0 + (i * SIG_RSTEP))) + SIG_AW;
        lv_obj_t *a = lv_arc_create(s_sig_box);
        lv_obj_remove_style_all(a);          /* also kills the knob and the
                                              * value indicator: both draw at
                                              * the default arc width of 0 */
        lv_obj_set_size(a, d, d);
        lv_obj_set_pos(a, cx - (d / 2), cy - (d / 2));
        /* 0° is 3 o'clock and angles run clockwise, so 270 is 12 o'clock and each
         * ring is cut symmetrically about it. */
        lv_arc_set_bg_angles(a, 270 - (SIG_SWEEP[i] / 2), 270 + (SIG_SWEEP[i] / 2));
        lv_obj_set_style_arc_width(a, SIG_AW, LV_PART_MAIN);
        /* Square-cut, not rounded: a rounded cap on a 2px stroke is a 2px circle
         * hung off the end, and the anti-aliasing landed it as a darker pixel
         * past each tip rather than as a taper. */
        lv_obj_set_style_arc_rounded(a, false, LV_PART_MAIN);
        lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
        s_sig_arc[i] = a;
    }

    s_sig_dot = lv_obj_create(s_sig_box);
    lv_obj_remove_style_all(s_sig_dot);
    lv_obj_set_size(s_sig_dot, SIG_DOT, SIG_DOT);
    lv_obj_set_pos(s_sig_dot, cx - (SIG_DOT / 2), cy - (SIG_DOT / 2));
    lv_obj_set_style_bg_opa(s_sig_dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_sig_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_clear_flag(s_sig_dot, LV_OBJ_FLAG_CLICKABLE);

    /* ponytail: 3 s poll. An event-driven update would mean a Wi-Fi handler
     * poking LVGL from the event task — add one only if the lag shows. */
    (void)lv_timer_create(signal_refresh, 3000, NULL);
    signal_refresh(NULL);
}

static void render_requested_screen(void) {
    /* A modal lives on the top layer, so it would survive the screen swap and
     * sit there swallowing every touch. Nothing wants that. */
    close_modal();

    /* The swipe's answer: build the admin code screen into a sheet parked below
     * the fold and slide it up over the sale screen, rather than swapping the
     * two instantly. Only for the gesture — every other route to this screen is
     * an ordinary rebuild, and a sheet rising with nothing behind it would be a
     * animation of nothing. */
    lv_obj_t *sheet = NULL;
    if (s_sheet_pending) {
        s_sheet_pending = false;
        if (s_req_screen == UI_SCREEN_ADMIN_UNLOCK) {
            sheet   = sheet_open();
            s_sheet = sheet;
        }
    }

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
        case UI_SCREEN_TOUCH_CAL:    build_touch_cal();    break;
    }

    /* The pointer's job ends with the dispatch — a later rebuild of the same
     * screen (a wrong code, say) must go back through the ordinary clearing
     * path rather than stacking a second set of widgets into this one. */
    s_sheet = NULL;
    if (sheet != NULL) { sheet_slide_in(sheet); }

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

    /* Now, not on the next poll — the bars are hidden on two screens, and a
     * three-second lag would leave them on the one they are hidden from. */
    if (s_sig_box != NULL) { signal_refresh(NULL); }
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
    touch_cal_load();            /* per-unit edge counts, before the first read */

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

    /* After the display exists (a theme belongs to one) and before the first
     * screen is built — lv_theme_apply runs at object creation, so anything
     * created earlier would keep the default look. */
    theme_init();
    signal_init();               /* top-right Wi-Fi bars, on the top layer */

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
        /* Swipe up from the bottom edge. Raised by indev_read, acted on here so
         * the driver stays a driver — and gated to the amount screen, which is
         * exactly where the burger used to be. Anywhere else the gesture is
         * dropped: opening the admin code screen out from under a customer
         * mid-sale, or on top of the panel it opens, is not a shortcut. */
        if (s_swipe_admin) {
            s_swipe_admin = false;
            if (s_req_screen == UI_SCREEN_AMOUNT) { open_admin_entry(); }
        }
        /* Touch calibration: the corner taps are read raw, and the result is
         * put back if nobody confirms it — an operator who cannot hit Keep is
         * exactly the case the deadline is for, and it is also the one who
         * cannot hit Discard. */
        if (s_req_screen == UI_SCREEN_TOUCH_CAL) {
            touch_cal_poll();
            if ((s_cal_step >= 2U) && (s_cal_countdown != NULL)) {
                int32_t left = (int32_t)(s_cal_deadline - lv_tick_get());
                if (left <= 0) {
                    s_cal_xmin = s_cal_prev[0]; s_cal_xmax = s_cal_prev[1];
                    s_cal_ymin = s_cal_prev[2]; s_cal_ymax = s_cal_prev[3];
                    request_screen(UI_SCREEN_SETTINGS);
                } else {
                    char c[40];
                    snprintf(c, sizeof(c), "Reverting in %d s",
                             (int)((left + 999) / 1000));
                    lv_label_set_text(s_cal_countdown, c);
                }
            }
        }
        /* Progress on the transaction screen, same targeted hand-off as the
         * boot step and for the same reason: the confirmation wait is up to two
         * minutes, and a rebuild per poll pass would restart its spinner. The
         * label only exists on the spinner states, so a NULL is the ordinary
         * case for every other screen. */
        if (s_tx_info_dirty) {
            s_tx_info_dirty = false;
            if ((s_req_screen == UI_SCREEN_TX_STATUS) && (s_tx_info_lbl != NULL)) {
                lv_label_set_text(s_tx_info_lbl, s_tx_info);
            }
        }
        /* Gas caps stored from the config page. The two labels only exist while
         * the Tx tab is built and not on Tron, so a NULL pair is the ordinary
         * case — every other screen reads the caps when it is next built. */
        if (s_fees_dirty) {
            s_fees_dirty = false;
            if ((s_fee_max_lbl != NULL) && (s_fee_prio_lbl != NULL)) {
                char f[16];
                snprintf(f, sizeof(f), "%u",
                         static_cast<unsigned>(settings_get_max_fee_gwei()));
                lv_label_set_text(s_fee_max_lbl, f);
                snprintf(f, sizeof(f), "%u",
                         static_cast<unsigned>(settings_get_priority_fee_gwei()));
                lv_label_set_text(s_fee_prio_lbl, f);
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

extern "C" void ui_fees_changed(void) {
    s_fees_dirty = true;   /* applied by the UI task — LVGL is single-thread */
}

extern "C" void ui_clock_changed(void) {
    /* Only the cache is invalidated here; the status timer picks it up within
     * three seconds and retexts the label on the UI task, which is the only
     * task allowed to touch it. */
    s_tz_dirty = true;
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
    s_prov_msg[0]  = '\0';   /* a new attempt starts; drop the last one's reason */
    request_screen(UI_SCREEN_PIN);
}

extern "C" void ui_set_prov_note(const char *msg) {
    strncpy(s_prov_msg, (msg != NULL) ? msg : "", sizeof(s_prov_msg) - 1);
    s_prov_msg[sizeof(s_prov_msg) - 1] = '\0';
}

extern "C" void ui_show_card_wait(const char *note) {
    strncpy(s_card_note, (note != NULL) ? note : "", sizeof(s_card_note) - 1);
    s_card_note[sizeof(s_card_note) - 1] = '\0';
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

extern "C" void ui_set_tx_info(const char *info) {
    strncpy(s_tx_info, (info != NULL) ? info : "", sizeof(s_tx_info) - 1);
    s_tx_info[sizeof(s_tx_info) - 1] = '\0';
    s_tx_info_dirty = true;
}
